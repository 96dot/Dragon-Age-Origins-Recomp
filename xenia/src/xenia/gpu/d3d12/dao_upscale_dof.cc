// DAO: cinematic depth of field in conversations and cutscenes, on the
// upscaled scene (see dao_dof.hlsli) - see dao_upscale.h.
#include "xenia/gpu/d3d12/dao_upscale_impl.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "xenia/base/logging.h"
#include "xenia/dao_live_settings.h"
#include "xenia/gpu/d3d12/d3d12_command_processor.h"

DEFINE_bool(dao_depth_of_field, true,
            "Depth of field in conversations and cutscenes with temporal "
            "upscaling: the background (and anything close to the camera) "
            "blurred around the speaker, as a film camera's lens does.",
            "GPU");
DEFINE_double(dao_depth_of_field_scale, 1.0,
              "The depth of field's blur, as a multiple of its default "
              "(larger: a wider aperture), 0.25 to 4.",
              "GPU");
DEFINE_bool(dao_depth_of_field_always, false,
            "Depth of field in every 3D scene, not only conversations and "
            "cutscenes (for testing).",
            "GPU");
DEFINE_bool(dao_show_depth_of_field, false,
            "Shows depth of field's blur instead of the scene (red: behind "
            "the focus, blue: in front of it, brighter: more blur), for "
            "debugging.",
            "GPU");

namespace xe {
namespace gpu {
namespace d3d12 {

namespace shaders {
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/dao_dof_blur_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/dao_dof_combine_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/dao_dof_focus_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/dao_dof_prefilter_cs.h"
}  // namespace shaders

namespace {

// The game modes with shots of their own (DAORecomp/kernel/game_modes.h):
// conversations, cutscenes.
constexpr int32_t kModeConversation = 4;
constexpr int32_t kModeCutscene = 5;
// The blur of a surface at infinity with the lens focused at the near plane,
// as a fraction of the display height, at a 60 degree vertical field of view
// (the game's in exploration; conversation shots are narrower, and blur more,
// as a longer lens does, by the square root of the zoom here: the game's
// closest shots, near 20 degrees, would be all blur otherwise); and the most
// blur, as a fraction of it too.
constexpr float kDofAperture = 0.009f;
constexpr float kDofMaxRadius = 0.014f;
constexpr double kDofReferenceHalfFov = std::numbers::pi / 6.0;
// The blur's samples: one per this many half size pixels of its disc (about
// 2 apart, which the combine's tent fills), in steps of 8, within limits
// (at 4K the most, 64; at 800 lines about 32).
constexpr float kDofAreaPerSample = 4.0f;
constexpr uint32_t kDofMinSamples = 16;
constexpr uint32_t kDofMaxSamples = 64;
// How long the effect takes to come and go with the game mode, and the time
// constant the focus follows a change in the scene with (ms).
constexpr float kDofEaseMs = 300.0f;
constexpr float kDofFocusMs = 150.0f;
// A change of the field of view (a ratio) that is a cut to another shot.
constexpr float kDofFovCut = 1.005f;

}  // namespace

bool DaoUpscalerImpl::InitializeDepthOfField() {
  const ui::d3d12::D3D12Provider& provider = cp_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  // Constants, t0 to t3, u0 (separate tables, the one-use descriptors aren't
  // contiguous), the linear sampler.
  D3D12_DESCRIPTOR_RANGE ranges[kDofTables] = {};
  for (uint32_t i = 0; i < kDofTables; ++i) {
    const bool uav = i == kDofTables - 1;
    ranges[i].RangeType =
        uav ? D3D12_DESCRIPTOR_RANGE_TYPE_UAV : D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[i].NumDescriptors = 1;
    ranges[i].BaseShaderRegister = uav ? 0 : i;
  }
  D3D12_ROOT_PARAMETER parameters[1 + kDofTables] = {};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[0].Constants.Num32BitValues =
      sizeof(DofConstants) / sizeof(uint32_t);
  for (uint32_t i = 0; i < kDofTables; ++i) {
    parameters[1 + i].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1 + i].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
  }
  D3D12_STATIC_SAMPLER_DESC linear_sampler = {};
  linear_sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  linear_sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  linear_sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  linear_sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  linear_sampler.MaxLOD = D3D12_FLOAT32_MAX;
  linear_sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC desc = {};
  desc.NumParameters = UINT(std::size(parameters));
  desc.pParameters = parameters;
  desc.NumStaticSamplers = 1;
  desc.pStaticSamplers = &linear_sampler;
  dof_root_signature_.Attach(
      ui::d3d12::util::CreateRootSignature(provider, desc));
  if (!dof_root_signature_) {
    XELOGE("Depth of field: failed to create the root signature");
    return false;
  }
  dof_focus_pipeline_.Attach(ui::d3d12::util::CreateComputePipeline(
      device, shaders::dao_dof_focus_cs, sizeof(shaders::dao_dof_focus_cs),
      dof_root_signature_.Get()));
  dof_prefilter_pipeline_.Attach(ui::d3d12::util::CreateComputePipeline(
      device, shaders::dao_dof_prefilter_cs,
      sizeof(shaders::dao_dof_prefilter_cs), dof_root_signature_.Get()));
  dof_blur_pipeline_.Attach(ui::d3d12::util::CreateComputePipeline(
      device, shaders::dao_dof_blur_cs, sizeof(shaders::dao_dof_blur_cs),
      dof_root_signature_.Get()));
  dof_combine_pipeline_.Attach(ui::d3d12::util::CreateComputePipeline(
      device, shaders::dao_dof_combine_cs,
      sizeof(shaders::dao_dof_combine_cs), dof_root_signature_.Get()));
  if (!dof_focus_pipeline_ || !dof_prefilter_pipeline_ ||
      !dof_blur_pipeline_ || !dof_combine_pipeline_) {
    XELOGE("Depth of field: failed to create the pipelines");
    return false;
  }
  return true;
}

bool DaoUpscalerImpl::UpdateDepthOfField(float frame_ms,
                                         uint32_t display_width,
                                         uint32_t display_height) {
  const int32_t mode = dao_live::Load(dao_live::values.camera_mode);
  const float fov = dao_live::Load(dao_live::values.camera_fov);
  const bool wanted =
      dof_available_ && dao_live::Load(dao_live::values.depth_of_field) &&
      (cvars::dao_depth_of_field_always || mode == kModeConversation ||
       mode == kModeCutscene) &&
      (display_width != dof_failed_size_[0] ||
       display_height != dof_failed_size_[1]);
  // Eased in and out (a frame of up to 100 ms counted as such).
  const float step = std::clamp(frame_ms, 0.0f, 100.0f) / kDofEaseMs;
  const float before = dof_strength_;
  dof_strength_ = wanted ? std::min(dof_strength_ + step, 1.0f)
                         : std::max(dof_strength_ - step, 0.0f);
  // The focus taken at once when the effect starts, after a frame not
  // upscaled (the depth is of another scene, or none), and on a cut: another
  // mode, or another field of view (a conversation's shots have their own).
  const bool fov_cut = !(fov > 0.0f && dof_fov_ > 0.0f &&
                         std::max(fov / dof_fov_, dof_fov_ / fov) < kDofFovCut);
  if (before <= 0.0f || reset_history_ || mode != dof_mode_ || fov_cut) {
    dof_snap_ = true;
  }
  dof_mode_ = mode;
  dof_fov_ = fov;
  if ((before > 0.0f) != (dof_strength_ > 0.0f)) {
    XELOGI("Depth of field: {} (game mode {}, field of view {:.1f} degrees)",
           dof_strength_ > 0.0f ? "on" : "off", mode,
           fov * 180.0f / std::numbers::pi_v<float>);
  }
  if (dof_strength_ <= 0.0f) {
    return false;
  }
  const uint32_t half_width = (display_width + 1) / 2;
  const uint32_t half_height = (display_height + 1) / 2;
  if (!EnsureTexture(dof_focus_, DXGI_FORMAT_R32_FLOAT, 2, 1,
                     "depth of field focus") ||
      !EnsureTexture(dof_half_, DXGI_FORMAT_R16G16B16A16_FLOAT, half_width,
                     half_height, "depth of field half size") ||
      !EnsureTexture(dof_blurred_, DXGI_FORMAT_R16G16B16A16_FLOAT, half_width,
                     half_height, "depth of field blur") ||
      !EnsureTexture(dof_, DXGI_FORMAT_R16G16B16A16_FLOAT, display_width,
                     display_height, "depth of field")) {
    // Not trying again every frame: until the display size changes.
    dof_failed_size_[0] = display_width;
    dof_failed_size_[1] = display_height;
    dof_strength_ = 0.0f;
    return false;
  }
  return true;
}

void DaoUpscalerImpl::RecordDepthOfField(
    uint32_t render_width, uint32_t render_height, bool linear_color,
    float frame_ms,
    const ui::d3d12::util::DescriptorCpuGpuHandlePair* descriptors) {
  // descriptors: depth, the focus, the focus (unordered), the scene, half
  // size (unordered), half size, the blur (unordered), the blur, the output
  // (unordered).
  ID3D12Device* device = cp_.GetD3D12Provider().GetDevice();
  D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
  uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  srv.Format = depth_.format;
  device->CreateShaderResourceView(depth_.resource.Get(), &srv,
                                   descriptors[0].first);
  srv.Format = dof_focus_.format;
  device->CreateShaderResourceView(dof_focus_.resource.Get(), &srv,
                                   descriptors[1].first);
  uav.Format = dof_focus_.format;
  device->CreateUnorderedAccessView(dof_focus_.resource.Get(), nullptr, &uav,
                                    descriptors[2].first);
  srv.Format = upscaled_.format;
  device->CreateShaderResourceView(upscaled_.resource.Get(), &srv,
                                   descriptors[3].first);
  uav.Format = dof_half_.format;
  device->CreateUnorderedAccessView(dof_half_.resource.Get(), nullptr, &uav,
                                    descriptors[4].first);
  srv.Format = dof_half_.format;
  device->CreateShaderResourceView(dof_half_.resource.Get(), &srv,
                                   descriptors[5].first);
  uav.Format = dof_blurred_.format;
  device->CreateUnorderedAccessView(dof_blurred_.resource.Get(), nullptr,
                                    &uav, descriptors[6].first);
  srv.Format = dof_blurred_.format;
  device->CreateShaderResourceView(dof_blurred_.resource.Get(), &srv,
                                   descriptors[7].first);
  uav.Format = dof_.format;
  device->CreateUnorderedAccessView(dof_.resource.Get(), nullptr, &uav,
                                    descriptors[8].first);

  const uint32_t display_width = dof_.width;
  const uint32_t display_height = dof_.height;
  DofConstants constants = {};
  constants.display_size[0] = display_width;
  constants.display_size[1] = display_height;
  constants.half_size[0] = dof_half_.width;
  constants.half_size[1] = dof_half_.height;
  constants.render_size[0] = render_width;
  constants.render_size[1] = render_height;
  // A narrower field of view (a longer lens) blurs more, by the tangent of
  // half the angle (at the same framing).
  const float fov = dof_fov_ > 0.0f ? dof_fov_ : 1.0f;
  const float lens = float(std::sqrt(std::tan(kDofReferenceHalfFov) /
                                     std::tan(std::clamp(fov * 0.5, 0.05, 1.5))));
  const float scale =
      float(std::clamp(cvars::dao_depth_of_field_scale, 0.25, 4.0));
  constants.aperture = kDofAperture * float(display_height) * lens * scale;
  constants.max_radius = kDofMaxRadius * float(display_height) * scale;
  // Smoothed (a smoothstep of the eased strength).
  constants.strength =
      dof_strength_ * dof_strength_ * (3.0f - 2.0f * dof_strength_);
  constants.focus_rate =
      1.0f - std::exp(-std::clamp(frame_ms, 0.0f, 100.0f) / kDofFocusMs);
  constants.gamma = linear_color ? 0.0f : OutputGamma();
  const float half_radius = constants.max_radius * 0.5f;
  const uint32_t samples =
      uint32_t(std::numbers::pi_v<float> * half_radius * half_radius /
               kDofAreaPerSample) +
      7;
  constants.samples =
      std::clamp(samples / 8 * 8, kDofMinSamples, kDofMaxSamples);
  constants.flags = (dof_snap_ ? kDofFlagSnap : 0) |
                    (cvars::dao_show_depth_of_field ? kDofFlagShow : 0) |
                    (depth_greater_draws_ > depth_less_draws_
                         ? kDofFlagDepthReversed
                         : 0);
  dof_snap_ = false;

  // The focus.
  Transition(depth_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Transition(upscaled_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Transition(dof_focus_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  const ui::d3d12::util::DescriptorCpuGpuHandlePair* focus_tables[kDofTables] =
      {&descriptors[0], &descriptors[0], &descriptors[0], &descriptors[0],
       &descriptors[2]};
  DispatchDepthOfField(dof_focus_pipeline_.Get(), constants, focus_tables, 1,
                       1);
  // The half size scene.
  Transition(dof_focus_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Transition(dof_half_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  const ui::d3d12::util::DescriptorCpuGpuHandlePair*
      prefilter_tables[kDofTables] = {&descriptors[0], &descriptors[1],
                                      &descriptors[3], &descriptors[3],
                                      &descriptors[4]};
  DispatchDepthOfField(dof_prefilter_pipeline_.Get(), constants,
                       prefilter_tables, (dof_half_.width + 7) / 8,
                       (dof_half_.height + 7) / 8);
  // The blur.
  Transition(dof_half_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Transition(dof_blurred_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  const ui::d3d12::util::DescriptorCpuGpuHandlePair* blur_tables[kDofTables] =
      {&descriptors[0], &descriptors[1], &descriptors[5], &descriptors[5],
       &descriptors[6]};
  DispatchDepthOfField(dof_blur_pipeline_.Get(), constants, blur_tables,
                       (dof_half_.width + 7) / 8, (dof_half_.height + 7) / 8);
  // Over the sharp scene.
  Transition(dof_blurred_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Transition(dof_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  const ui::d3d12::util::DescriptorCpuGpuHandlePair*
      combine_tables[kDofTables] = {&descriptors[0], &descriptors[1],
                                    &descriptors[3], &descriptors[7],
                                    &descriptors[8]};
  DispatchDepthOfField(dof_combine_pipeline_.Get(), constants, combine_tables,
                       (display_width + 7) / 8, (display_height + 7) / 8);
  Transition(dof_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

void DaoUpscalerImpl::DispatchDepthOfField(
    ID3D12PipelineState* pipeline, const DofConstants& constants,
    const ui::d3d12::util::DescriptorCpuGpuHandlePair* const* tables,
    uint32_t groups_x, uint32_t groups_y) {
  cp_.SubmitBarriers();
  DeferredCommandList& command_list = cp_.GetDeferredCommandList();
  command_list.D3DSetComputeRootSignature(dof_root_signature_.Get());
  command_list.D3DSetComputeRoot32BitConstants(
      0, sizeof(constants) / sizeof(uint32_t), &constants, 0);
  for (uint32_t i = 0; i < kDofTables; ++i) {
    command_list.D3DSetComputeRootDescriptorTable(1 + i, tables[i]->second);
  }
  cp_.SetExternalPipeline(pipeline);
  command_list.D3DDispatch(groups_x, groups_y, 1);
}

}  // namespace d3d12
}  // namespace gpu
}  // namespace xe
