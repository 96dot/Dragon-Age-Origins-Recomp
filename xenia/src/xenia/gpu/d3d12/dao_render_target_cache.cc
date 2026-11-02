// DAO: plain resolves copied straight from the render target (rather than
// from its dump to the EDRAM buffer), and the area they write for the
// textures of the resolved memory (dao_texture_cache.cc): the render target
// cache's part. And the game's bloom and depth of field drawn at the
// console's resolution, and its depth of field switched off.
#include "xenia/gpu/d3d12/d3d12_render_target_cache.h"

#include <array>
#include <cstdint>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/dao_live_settings.h"
#include "xenia/gpu/d3d12/d3d12_command_processor.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/xenos.h"
#include "xenia/ui/d3d12/d3d12_provider.h"
#include "xenia/ui/d3d12/d3d12_util.h"

DEFINE_bool(dao_resolve_to_textures, true,
            "Write plain render target resolves straight into the textures of "
            "the resolved memory, rather than untiling them at their next use.",
            "GPU");
DEFINE_bool(dao_native_bloom, true,
            "With resolution scaling, draw the game's bloom at the console's "
            "resolution, as the console does: its blur's taps are a console "
            "pixel apart, so at a higher resolution they leave gaps, and the "
            "glow becomes offset copies of bright edges.",
            "GPU");
DEFINE_bool(dao_native_depth_of_field, true,
            "With resolution scaling, draw the half-resolution passes of the "
            "game's own depth of field (in conversations; not the recomp's, "
            "dao_depth_of_field) at the console's resolution, as the console "
            "does: their blurs' taps are spaced in console pixels, so at a "
            "higher resolution they leave gaps, and the blurred background "
            "gets thin streaks. And blend the blur over the scene with "
            "bilinear filtering: the console's point sampling shows sharp "
            "blocks at a higher resolution.",
            "GPU");
DEFINE_bool(dao_console_depth_of_field, true,
            "The game's own depth of field in conversations (the console's "
            "light blur of what's behind and in front of the people "
            "speaking; not the recomp's, dao_depth_of_field). Off, its "
            "passes are skipped and the scene is drawn sharp, for the "
            "recomp's to blur instead (with temporal upscaling) or none.",
            "GPU");

namespace xe {
namespace gpu {
namespace d3d12 {

namespace {

// The bloom's pixel shaders (microcode hashes; the shaders are the game
// data's, which the title updates don't replace). At 640x360, half the
// console's 1280x720: the scene's bright parts, then a blur, horizontal and
// vertical (seven taps, their distance in console pixels from the vertex
// shader's constants). The tonemapper (kTonemapPixelShader in
// dao_upscale_impl.h) adds it to the scene.
constexpr uint64_t kBloomBrightPassPixelShader = 0x51B90349C6D6D3DA;
constexpr uint64_t kBloomBlurPixelShader = 0x44FF4F98F3485DB5;

// The game's own depth of field's half-resolution pixel shaders (microcode
// hashes, as the bloom's; the da_dof effect of postprocesseffects.erf, in
// conversations, not the recomp's depth of field of dao_upscale_dof.cc). At
// 640x360, as a frame trace shows: the scene and its blur radius from the
// depth, then a blur, horizontal, then vertical, then a small one against
// artifacts. The full-resolution pass that blends the result over the scene
// stays at the scale; it reads the blur (texture fetch constant 2) with the
// fetch constant's point sampling, made bilinear. It takes the scene (from
// the copy the effect resolved before its passes, fetch constant 0) where
// the blur's weight is 0; the weight, as its microcode computes it: the
// larger of the depth's and the blur's own, times its MaxBlur parameter
// (float constant 1's x), saturated.
constexpr uint64_t kGameDofDownsamplePixelShader = 0xFC6987BE51851414;
constexpr uint64_t kGameDofBlurHPixelShader = 0x3E2773AC40F6AB27;
constexpr uint64_t kGameDofBlurVPixelShader = 0xC9D916DC2650BA46;
constexpr uint64_t kGameDofSmallBlurPixelShader = 0xFE45551A76BA89A0;
constexpr uint64_t kGameDofBlendPixelShader = 0xCB448B49D98D1E8A;
constexpr uint32_t kGameDofBlendBlurFetchConstant = 2;
constexpr uint32_t kGameDofBlendMaxBlurFloatConstant = 1;

// The pitch of the bloom's and the depth of field's render targets, half the
// console's width. A draw of one of their shaders at a larger pitch (their
// blur material serves other effects too, the Fade's at full resolution)
// stays at the scale.
constexpr uint32_t kHalfResolutionPitch = 640;

// The game's depth of field's half-resolution passes (all but its blend).
bool IsGameDofPassPixelShader(uint64_t pixel_shader_hash) {
  return pixel_shader_hash == kGameDofDownsamplePixelShader ||
         pixel_shader_hash == kGameDofBlurHPixelShader ||
         pixel_shader_hash == kGameDofBlurVPixelShader ||
         pixel_shader_hash == kGameDofSmallBlurPixelShader;
}

}  // namespace

bool D3D12RenderTargetCache::DaoOnDraw(uint64_t pixel_shader_hash) {
  dao_draw_native_ = false;
  dao_linear_fetch_constant_ = UINT32_MAX;
  dao_zeroed_float_constant_pixel_ = UINT32_MAX;
  const uint32_t pitch =
      register_file().Get<reg::RB_SURFACE_INFO>().surface_pitch;
  // The game's depth of field off: its passes are skipped, from its
  // downsample (a shader no other effect draws) to its blend, the next draw
  // at a larger pitch. The clears and resolves between the passes still run,
  // so the blur the blend reads stays a finite image. The blend still draws,
  // with the blur's weight 0, the scene alone: the clears overwrite part of
  // the scene's render target in the EDRAM, and the blend draws over all of
  // it.
  if (pixel_shader_hash == kGameDofDownsamplePixelShader &&
      pitch <= kHalfResolutionPitch &&
      !dao_live::Load(dao_live::values.console_depth_of_field)) {
    dao_skipping_game_dof_ = true;
    static bool skip_logged = false;
    if (!skip_logged) {
      skip_logged = true;
      XELOGI("D3D12RenderTargetCache: the game's own depth of field is off: "
             "its passes are skipped");
    }
  }
  if (dao_skipping_game_dof_) {
    if (pitch <= kHalfResolutionPitch) {
      if (IsGameDofPassPixelShader(pixel_shader_hash)) {
        return false;
      }
    } else {
      dao_skipping_game_dof_ = false;
      if (pixel_shader_hash == kGameDofBlendPixelShader) {
        dao_zeroed_float_constant_pixel_ = kGameDofBlendMaxBlurFloatConstant;
      } else {
        static bool no_blend_logged = false;
        if (!no_blend_logged) {
          no_blend_logged = true;
          XELOGW("D3D12RenderTargetCache: the game's own depth of field's "
                 "passes were skipped, but its blend didn't follow (pixel "
                 "shader {:016X})",
                 pixel_shader_hash);
        }
      }
    }
  }
  if (!IsDrawResolutionScaled()) {
    return true;
  }
  // The depth of field's blend reads its blur bilinear. The bilinear sampler
  // skips GetSamplerParameters' fallback to point sampling for a format the
  // host can't filter, so the guest format is checked: k_8_8_8_8's host
  // formats (R8G8B8A8 UNORM and SNORM) are filterable on every D3D12 device.
  if (cvars::dao_native_depth_of_field &&
      pixel_shader_hash == kGameDofBlendPixelShader &&
      register_file().GetTextureFetch(kGameDofBlendBlurFetchConstant).format ==
          xenos::TextureFormat::k_8_8_8_8) {
    dao_linear_fetch_constant_ = kGameDofBlendBlurFetchConstant;
  }
  // Native render targets exist with host render targets only. The clears
  // before the passes stay scaled (the same clear shader serves every pass);
  // ownership transfers move the half-resolution targets between the scales.
  if (GetPath() != Path::kHostRenderTargets || pitch > kHalfResolutionPitch) {
    return true;
  }
  const bool bloom = cvars::dao_native_bloom &&
                     (pixel_shader_hash == kBloomBrightPassPixelShader ||
                      pixel_shader_hash == kBloomBlurPixelShader);
  const bool depth_of_field =
      cvars::dao_native_depth_of_field &&
      (pixel_shader_hash == kGameDofDownsamplePixelShader ||
       pixel_shader_hash == kGameDofBlurHPixelShader ||
       pixel_shader_hash == kGameDofBlurVPixelShader ||
       pixel_shader_hash == kGameDofSmallBlurPixelShader);
  dao_draw_native_ = bloom || depth_of_field;
  static bool bloom_logged = false;
  if (bloom && !bloom_logged) {
    bloom_logged = true;
    XELOGI("D3D12RenderTargetCache: drawing the game's bloom at the console's "
           "resolution");
  }
  static bool depth_of_field_logged = false;
  if (depth_of_field && !depth_of_field_logged) {
    depth_of_field_logged = true;
    XELOGI("D3D12RenderTargetCache: drawing the game's own depth of field at "
           "the console's resolution");
  }
  return true;
}

namespace shaders {
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/dao_resolve_direct_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/dao_resolve_direct_scaled_cs.h"
}  // namespace shaders

void D3D12RenderTargetCache::DaoInitializeDirectResolve(
    std::array<D3D12_ROOT_PARAMETER, 3>& resolve_copy_root_parameters,
    const D3D12_ROOT_SIGNATURE_DESC& resolve_copy_root_signature_desc) {
  const ui::d3d12::D3D12Provider& provider =
      command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  bool draw_resolution_scaled = IsDrawResolutionScaled();

  // Resolve copies straight from render targets: the source is an SRV table.
  if (cvars::dao_resolve_to_textures) {
    D3D12_DESCRIPTOR_RANGE resolve_direct_source_range = {};
    resolve_direct_source_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    resolve_direct_source_range.NumDescriptors = 1;
    resolve_copy_root_parameters[2].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    resolve_copy_root_parameters[2].DescriptorTable.NumDescriptorRanges = 1;
    resolve_copy_root_parameters[2].DescriptorTable.pDescriptorRanges =
        &resolve_direct_source_range;
    resolve_copy_root_parameters[0].Constants.Num32BitValues =
        (draw_resolution_scaled
             ? sizeof(draw_util::ResolveCopyShaderConstants::DestRelative)
             : sizeof(draw_util::ResolveCopyShaderConstants)) /
        sizeof(uint32_t);
    resolve_direct_root_signature_ = ui::d3d12::util::CreateRootSignature(
        provider, resolve_copy_root_signature_desc);
    if (resolve_direct_root_signature_) {
      resolve_direct_pipeline_ = ui::d3d12::util::CreateComputePipeline(
          device,
          draw_resolution_scaled ? shaders::dao_resolve_direct_scaled_cs
                                 : shaders::dao_resolve_direct_cs,
          draw_resolution_scaled ? sizeof(shaders::dao_resolve_direct_scaled_cs)
                                 : sizeof(shaders::dao_resolve_direct_cs),
          resolve_direct_root_signature_);
    }
    if (draw_resolution_scaled) {
      resolve_copy_root_parameters[0].Constants.Num32BitValues =
          sizeof(draw_util::ResolveCopyShaderConstants) / sizeof(uint32_t);
      resolve_direct_native_root_signature_ =
          ui::d3d12::util::CreateRootSignature(
              provider, resolve_copy_root_signature_desc);
      if (resolve_direct_native_root_signature_) {
        resolve_direct_native_pipeline_ =
            ui::d3d12::util::CreateComputePipeline(
                device, shaders::dao_resolve_direct_cs,
                sizeof(shaders::dao_resolve_direct_cs),
                resolve_direct_native_root_signature_);
      }
    }
    if (!resolve_direct_pipeline_ ||
        (draw_resolution_scaled && !resolve_direct_native_pipeline_)) {
      XELOGE(
          "D3D12RenderTargetCache: Failed to create the direct resolve copy "
          "pipelines");
    }
  }
}

bool D3D12RenderTargetCache::DaoCreateDirectResolveSource(
    D3D12RenderTarget& source, bool copy_native,
    ui::d3d12::util::DescriptorCpuGpuHandlePair& source_out) {
  if (!(copy_native ? resolve_direct_native_pipeline_
                    : resolve_direct_pipeline_) ||
      !command_processor_.RequestOneUseSingleViewDescriptors(1, &source_out)) {
    return false;
  }
  D3D12_SHADER_RESOURCE_VIEW_DESC source_srv_desc = {};
  source_srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  source_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  source_srv_desc.Shader4ComponentMapping =
      D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  source_srv_desc.Texture2D.MipLevels = 1;
  command_processor_.GetD3D12Provider().GetDevice()->CreateShaderResourceView(
      source.resource(), &source_srv_desc,
      source_out.first);
  return true;
}

D3D12RenderTargetCache::D3D12RenderTarget*
D3D12RenderTargetCache::GetResolvedArea(
    const draw_util::ResolveInfo& resolve_info,
    draw_util::ResolveCopyShaderIndex copy_shader, bool copy_dest_scaled,
    uint32_t dump_base, uint32_t dump_row_length_used, uint32_t dump_rows,
    uint32_t dump_pitch, D3D12TextureCache::ResolvedArea& area_out) const {
  if (!cvars::dao_resolve_to_textures || GetPath() != Path::kHostRenderTargets ||
      copy_shader != draw_util::ResolveCopyShaderIndex::kFast32bpp1x2xMSAA ||
      resolve_info.IsCopyingDepth() || dump_rectangles_.size() != 1) {
    return nullptr;
  }
  const draw_util::ResolveEdramInfo edram_info = resolve_info.color_edram_info;
  const reg::RB_COPY_DEST_INFO dest_info = resolve_info.copy_dest_info;
  if (edram_info.msaa_samples != xenos::MsaaSamples::k1X ||
      xenos::ColorRenderTargetFormat(edram_info.format) !=
          xenos::ColorRenderTargetFormat::k_8_8_8_8 ||
      dest_info.copy_dest_array ||
      xenos::ColorFormat(dest_info.copy_dest_format) !=
          xenos::ColorFormat::k_8_8_8_8 ||
      uint32_t(dest_info.copy_dest_endian) > uint32_t(xenos::Endian::k16in32)) {
    return nullptr;
  }
  // The render target owns the whole area, without EDRAM wrapping.
  const ResolveCopyDumpRectangle& rectangle = dump_rectangles_.front();
  if (rectangle.row_first || rectangle.rows != dump_rows ||
      rectangle.row_first_start ||
      rectangle.row_last_end != dump_row_length_used ||
      dump_base + (dump_rows - 1) * dump_pitch + dump_row_length_used >
          xenos::kEdramTileCount) {
    return nullptr;
  }
  auto& render_target =
      *static_cast<D3D12RenderTarget*>(rectangle.render_target);
  const RenderTargetKey key = render_target.key();
  if (key.is_depth || key.msaa_samples != xenos::MsaaSamples::k1X ||
      key.GetColorFormat() != xenos::ColorRenderTargetFormat::k_8_8_8_8 ||
      key.next_period || key.pitch_tiles_at_32bpp != edram_info.pitch_tiles ||
      key.base_tiles > edram_info.base_tiles ||
      copy_dest_scaled != (IsDrawResolutionScaled() && !key.scale_native)) {
    return nullptr;
  }
  // The area in the render target, laid out like the EDRAM from its base.
  const uint32_t pitch = edram_info.pitch_tiles;
  const uint32_t base_offset = edram_info.base_tiles - key.base_tiles;
  const uint32_t source_x =
      base_offset % pitch * xenos::kEdramTileWidthSamples +
      (resolve_info.coordinate_info.edram_offset_x_div_8 << 3);
  const uint32_t source_y =
      base_offset / pitch * xenos::kEdramTileHeightSamples +
      (resolve_info.coordinate_info.edram_offset_y_div_8 << 3);
  const uint32_t width = resolve_info.coordinate_info.width_div_8 << 3;
  const uint32_t height = resolve_info.height_div_8 << 3;
  const uint32_t scale_x = copy_dest_scaled ? draw_resolution_scale_x() : 1;
  const uint32_t scale_y = copy_dest_scaled ? draw_resolution_scale_y() : 1;
  const D3D12_RESOURCE_DESC desc = render_target.resource()->GetDesc();
  if (source_x + width > pitch * xenos::kEdramTileWidthSamples ||
      (source_x + width) * scale_x > desc.Width ||
      (source_y + height) * scale_y > desc.Height ||
      resolve_info.dao_rect[0] < 0 || resolve_info.dao_rect[1] < 0 ||
      uint32_t(resolve_info.dao_rect[2] - resolve_info.dao_rect[0]) != width ||
      uint32_t(resolve_info.dao_rect[3] - resolve_info.dao_rect[1]) != height) {
    return nullptr;
  }
  area_out.source = render_target.resource();
  area_out.source_x = source_x;
  area_out.source_y = source_y;
  // A 32bpp destination's base is that of the texture (4 KB macro tiles), and
  // the area is at the rectangle's place in it.
  area_out.dest_base =
      register_file()[XE_GPU_REG_RB_COPY_DEST_BASE] & 0x1FFFFFFF;
  area_out.dest_pitch_div_32 =
      resolve_info.copy_dest_coordinate_info.pitch_aligned_div_32;
  area_out.dest_x = uint32_t(resolve_info.dao_rect[0]);
  area_out.dest_y = uint32_t(resolve_info.dao_rect[1]);
  area_out.width = width;
  area_out.height = height;
  area_out.swap_red_blue = dest_info.copy_dest_swap != 0;
  area_out.endian = xenos::Endian(uint32_t(dest_info.copy_dest_endian));
  area_out.scaled = copy_dest_scaled;
  const bool fill = copy_dest_scaled && edram_info.fill_half_pixel_offset;
  area_out.fill_x = fill ? scale_x >> 1 : 0;
  area_out.fill_y = fill ? scale_y >> 1 : 0;
  return &render_target;
}

}  // namespace d3d12
}  // namespace gpu
}  // namespace xe
