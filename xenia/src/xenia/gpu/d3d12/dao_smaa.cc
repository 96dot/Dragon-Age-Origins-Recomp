// DAO: SMAA 1x on 3D scenes without temporal upscaling, in place of the
// game's 2xMSAA (see dao_smaa.hlsli) - see dao_upscale.h.
#include "xenia/gpu/d3d12/dao_upscale_impl.h"

#include <cstring>

#include "xenia/base/logging.h"
#include "xenia/dao_live_settings.h"
#include "xenia/gpu/d3d12/d3d12_command_processor.h"
#include "xenia/gpu/d3d12/dao_gpu_timing.h"

// SMAA's precomputed area and search textures (thirdparty/smaa).
#include "AreaTex.h"
#include "SearchTex.h"

DEFINE_bool(dao_smaa, false,
            "Without temporal upscaling, anti-alias 3D scenes with SMAA "
            "(rendered single-sampled) instead of the game's 2xMSAA.",
            "GPU");

namespace xe {
namespace gpu {
namespace d3d12 {

namespace shaders {
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/dao_smaa_edges_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/dao_smaa_weights_cs.h"
#include "xenia/gpu/shaders/bytecode/d3d12_5_1/dao_upscale_composite_smaa_cs.h"
}  // namespace shaders

bool DaoUpscalerImpl::InitializeSmaa() {
  const ui::d3d12::D3D12Provider& provider = cp_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  // Constants, t0 to t2, u0 (separate tables, the one-use descriptors aren't
  // contiguous), the linear and point samplers.
  D3D12_DESCRIPTOR_RANGE ranges[kSmaaTables] = {};
  for (uint32_t i = 0; i < kSmaaTables; ++i) {
    const bool uav = i == kSmaaTables - 1;
    ranges[i].RangeType =
        uav ? D3D12_DESCRIPTOR_RANGE_TYPE_UAV : D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[i].NumDescriptors = 1;
    ranges[i].BaseShaderRegister = uav ? 0 : i;
  }
  D3D12_ROOT_PARAMETER parameters[1 + kSmaaTables] = {};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[0].Constants.Num32BitValues =
      sizeof(SmaaConstants) / sizeof(uint32_t);
  for (uint32_t i = 0; i < kSmaaTables; ++i) {
    parameters[1 + i].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1 + i].DescriptorTable.NumDescriptorRanges = 1;
    parameters[1 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
  }
  D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
  for (uint32_t i = 0; i < 2; ++i) {
    samplers[i].Filter = i ? D3D12_FILTER_MIN_MAG_MIP_POINT
                           : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[i].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[i].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[i].ShaderRegister = i;
    samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_ROOT_SIGNATURE_DESC desc = {};
  desc.NumParameters = UINT(std::size(parameters));
  desc.pParameters = parameters;
  desc.NumStaticSamplers = UINT(std::size(samplers));
  desc.pStaticSamplers = samplers;
  smaa_root_signature_.Attach(
      ui::d3d12::util::CreateRootSignature(provider, desc));
  if (!smaa_root_signature_) {
    XELOGE("SMAA: failed to create the root signature");
    return false;
  }
  smaa_edges_pipeline_.Attach(ui::d3d12::util::CreateComputePipeline(
      device, shaders::dao_smaa_edges_cs, sizeof(shaders::dao_smaa_edges_cs),
      smaa_root_signature_.Get()));
  smaa_weights_pipeline_.Attach(ui::d3d12::util::CreateComputePipeline(
      device, shaders::dao_smaa_weights_cs,
      sizeof(shaders::dao_smaa_weights_cs), smaa_root_signature_.Get()));
  // The last pass is the composite's.
  composite_smaa_pipeline_.Attach(ui::d3d12::util::CreateComputePipeline(
      device, shaders::dao_upscale_composite_smaa_cs,
      sizeof(shaders::dao_upscale_composite_smaa_cs),
      composite_root_signature_.Get()));
  if (!smaa_edges_pipeline_ || !smaa_weights_pipeline_ ||
      !composite_smaa_pipeline_) {
    XELOGE("SMAA: failed to create the pipelines");
    return false;
  }
  return true;
}

bool DaoUpscalerImpl::SmaaWanted() const {
  return smaa_available_ && dao_live::Load(dao_live::values.smaa) &&
         GetTemporalUpscaler() == TemporalUpscaler::kOff && !smaa_failed_;
}

bool DaoUpscalerImpl::EnsureSmaaLookup() {
  if (smaa_area_.resource && smaa_search_.resource) {
    return true;
  }
  // Both through one upload buffer, each at a placed footprint.
  ID3D12Device* device = cp_.GetD3D12Provider().GetDevice();
  struct Lookup {
    Texture& texture;
    DXGI_FORMAT format;
    uint32_t width, height, pitch;
    const unsigned char* bytes;
    const char* name;
  } lookups[2] = {
      {smaa_area_, DXGI_FORMAT_R8G8_UNORM, AREATEX_WIDTH, AREATEX_HEIGHT,
       AREATEX_PITCH, areaTexBytes, "SMAA area"},
      {smaa_search_, DXGI_FORMAT_R8_UNORM, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT,
       SEARCHTEX_PITCH, searchTexBytes, "SMAA search"},
  };
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[2];
  uint64_t upload_size = 0;
  for (uint32_t i = 0; i < 2; ++i) {
    Lookup& lookup = lookups[i];
    if (!EnsureTexture(lookup.texture, lookup.format, lookup.width,
                       lookup.height, lookup.name)) {
      return false;
    }
    const D3D12_RESOURCE_DESC texture_desc =
        lookup.texture.resource->GetDesc();
    uint64_t size;
    device->GetCopyableFootprints(&texture_desc, 0, 1, upload_size,
                                  &footprints[i], nullptr, nullptr, &size);
    upload_size = (footprints[i].Offset + size +
                   D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) &
                  ~uint64_t(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
  }
  D3D12_RESOURCE_DESC buffer_desc;
  ui::d3d12::util::FillBufferResourceDesc(buffer_desc, upload_size,
                                          D3D12_RESOURCE_FLAG_NONE);
  ComPtr<ID3D12Resource> upload;
  void* mapping;
  D3D12_RANGE read_range = {};
  if (FAILED(device->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesUpload, D3D12_HEAP_FLAG_NONE,
          &buffer_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
          IID_PPV_ARGS(&upload))) ||
      FAILED(upload->Map(0, &read_range, &mapping))) {
    XELOGE("SMAA: failed to create the upload buffer of its lookup textures");
    // Released: kept, they'd be taken as uploaded (the check above).
    ReleaseTexture(smaa_area_);
    ReleaseTexture(smaa_search_);
    return false;
  }
  for (uint32_t i = 0; i < 2; ++i) {
    const Lookup& lookup = lookups[i];
    for (uint32_t y = 0; y < lookup.height; ++y) {
      std::memcpy(static_cast<uint8_t*>(mapping) + footprints[i].Offset +
                      size_t(y) * footprints[i].Footprint.RowPitch,
                  lookup.bytes + size_t(y) * lookup.pitch, lookup.pitch);
    }
  }
  upload->Unmap(0, nullptr);
  DeferredCommandList& command_list = cp_.GetDeferredCommandList();
  for (uint32_t i = 0; i < 2; ++i) {
    Texture& texture = lookups[i].texture;
    Transition(texture, D3D12_RESOURCE_STATE_COPY_DEST);
    cp_.SubmitBarriers();
    D3D12_TEXTURE_COPY_LOCATION dest = {};
    dest.pResource = texture.resource.Get();
    dest.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION source = {};
    source.pResource = upload.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = footprints[i];
    command_list.D3DCopyTextureRegion(&dest, 0, 0, 0, &source, nullptr);
    Transition(texture, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }
  // Released once the GPU is done with the copies.
  cp_.resources_for_deletion_.emplace_back(cp_.GetCurrentSubmission(),
                                           upload.Detach());
  return true;
}

void DaoUpscalerImpl::ReleaseSmaa() {
  ReleaseTexture(smaa_edges_);
  ReleaseTexture(smaa_weights_);
  ReleaseTexture(smaa_out_);
}

ID3D12Resource* DaoUpscalerImpl::CompositeSmaa(
    ID3D12Resource* frontbuffer,
    const D3D12_SHADER_RESOURCE_VIEW_DESC& frontbuffer_srv,
    D3D12_SHADER_RESOURCE_VIEW_DESC& srv_out, bool& hdr_out) {
  const uint32_t width = render_size_[0], height = render_size_[1];
  // True HDR blends its linear scene (the edges are the SDR image's).
  if (true_hdr_ && !EnsureHdrScene()) {
    return NotUpscaled("no HDR scene", frontbuffer, frontbuffer_srv, srv_out,
                       hdr_out);
  }
  if (!EnsureSmaaLookup() ||
      !EnsureTexture(smaa_edges_, DXGI_FORMAT_R8G8_UNORM, width, height,
                     "SMAA edges") ||
      !EnsureTexture(smaa_weights_, DXGI_FORMAT_R8G8B8A8_UNORM, width, height,
                     "SMAA weights") ||
      !EnsureTexture(smaa_out_,
                     true_hdr_ ? HdrFormat() : DXGI_FORMAT_R8G8B8A8_UNORM,
                     width, height, "SMAA")) {
    // Not trying again every frame: until the render size changes.
    smaa_failed_ = true;
    ReleaseSmaa();
    return true_hdr_ ? CompositeHdr(frontbuffer, frontbuffer_srv, srv_out,
                                    hdr_out)
                     : NotUpscaled("SMAA unavailable", frontbuffer,
                                   frontbuffer_srv, srv_out, hdr_out);
  }
  // Views: the image before the UI, the edges (unordered), the edges, the
  // area and search textures, the weights (unordered), the weights, the scene
  // (true HDR's), the front buffer, the output (unordered).
  constexpr uint32_t kViews = 10;
  ui::d3d12::util::DescriptorCpuGpuHandlePair views[kViews];
  if (!cp_.RequestOneUseSingleViewDescriptors(kViews, views)) {
    return NotUpscaled("no descriptors", frontbuffer, frontbuffer_srv, srv_out,
                       hdr_out);
  }
  ID3D12Device* device = cp_.GetD3D12Provider().GetDevice();
  D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
  uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  srv.Format = color_.format;
  device->CreateShaderResourceView(color_.resource.Get(), &srv,
                                   views[0].first);
  uav.Format = smaa_edges_.format;
  device->CreateUnorderedAccessView(smaa_edges_.resource.Get(), nullptr, &uav,
                                    views[1].first);
  srv.Format = smaa_edges_.format;
  device->CreateShaderResourceView(smaa_edges_.resource.Get(), &srv,
                                   views[2].first);
  srv.Format = smaa_area_.format;
  device->CreateShaderResourceView(smaa_area_.resource.Get(), &srv,
                                   views[3].first);
  srv.Format = smaa_search_.format;
  device->CreateShaderResourceView(smaa_search_.resource.Get(), &srv,
                                   views[4].first);
  uav.Format = smaa_weights_.format;
  device->CreateUnorderedAccessView(smaa_weights_.resource.Get(), nullptr,
                                    &uav, views[5].first);
  srv.Format = smaa_weights_.format;
  device->CreateShaderResourceView(smaa_weights_.resource.Get(), &srv,
                                   views[6].first);
  if (true_hdr_) {
    srv.Format = hdr_.format;
    device->CreateShaderResourceView(hdr_.resource.Get(), &srv,
                                     views[7].first);
  } else {
    views[7] = views[0];
  }
  device->CreateShaderResourceView(frontbuffer, &frontbuffer_srv,
                                   views[8].first);
  uav.Format = smaa_out_.format;
  device->CreateUnorderedAccessView(smaa_out_.resource.Get(), nullptr, &uav,
                                    views[9].first);

  SmaaConstants constants = {};
  constants.metrics[0] = 1.0f / float(width);
  constants.metrics[1] = 1.0f / float(height);
  constants.metrics[2] = float(width);
  constants.metrics[3] = float(height);
  constants.size[0] = width;
  constants.size[1] = height;
  // The edges (reading t0 only: the other tables are placeholders, as in the
  // weights pass).
  Transition(color_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Transition(smaa_edges_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  const ui::d3d12::util::DescriptorCpuGpuHandlePair* edge_tables[kSmaaTables] =
      {&views[0], &views[0], &views[0], &views[1]};
  DispatchSmaa(smaa_edges_pipeline_.Get(), constants, edge_tables);
  const bool timing = dao_timing::GpuTiming::Enabled();
  if (timing) {
    dao_timing::GpuTiming::Get().Mark(cp_, "SMAA edges");
  }
  // The blending weights.
  Transition(smaa_edges_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Transition(smaa_weights_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  const ui::d3d12::util::DescriptorCpuGpuHandlePair*
      weight_tables[kSmaaTables] = {&views[2], &views[3], &views[4],
                                    &views[5]};
  DispatchSmaa(smaa_weights_pipeline_.Get(), constants, weight_tables);
  if (timing) {
    dao_timing::GpuTiming::Get().Mark(cp_, "SMAA weights");
  }
  // The blending, with the UI: the composite's tables, the scene (t0), the
  // front buffer (t1), the image before the UI (t2), no motion (t3, unread:
  // the image's view), the weights (t4), unread t5, the output.
  Transition(smaa_weights_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  if (true_hdr_) {
    Transition(hdr_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }
  Transition(smaa_out_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  const ui::d3d12::util::DescriptorCpuGpuHandlePair tables[kCompositeTables] = {
      views[7], views[8], views[0], views[0], views[6], views[0], views[9]};
  CompositeConstants composite = {};
  composite.dest_size[0] = width;
  composite.dest_size[1] = height;
  composite.render_size[0] = float(width);
  composite.render_size[1] = float(height);
  composite.mode = true_hdr_ ? kCompositeHdr : kCompositeSdr;
  DispatchComposite(tables, composite, composite_smaa_pipeline_.Get());
  Transition(smaa_out_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  if (timing) {
    dao_timing::GpuTiming::Get().Mark(cp_, "SMAA blending, UI composite");
  }
  if (!smaa_logged_) {
    smaa_logged_ = true;
    XELOGI("SMAA: anti-aliasing {}x{}{} (image before UI from pixel shader "
           "{:016X})",
           width, height, true_hdr_ ? " in HDR" : "", hudless_pixel_shader_);
  }
  // Taken before EndFrame, which releases the output if SMAA was just turned
  // off (kept alive until the GPU is done with this frame) and updates
  // true_hdr_ for the next.
  ID3D12Resource* const out = smaa_out_.resource.Get();
  hdr_out = true_hdr_;
  srv_out = {};
  srv_out.Format = smaa_out_.format;
  srv_out.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv_out.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv_out.Texture2D.MipLevels = 1;
  EndFrame(false, "SMAA", true);
  return out;
}

void DaoUpscalerImpl::DispatchSmaa(
    ID3D12PipelineState* pipeline, const SmaaConstants& constants,
    const ui::d3d12::util::DescriptorCpuGpuHandlePair* const* tables) {
  cp_.SubmitBarriers();
  DeferredCommandList& command_list = cp_.GetDeferredCommandList();
  command_list.D3DSetComputeRootSignature(smaa_root_signature_.Get());
  command_list.D3DSetComputeRoot32BitConstants(
      0, sizeof(constants) / sizeof(uint32_t), &constants, 0);
  for (uint32_t i = 0; i < kSmaaTables; ++i) {
    command_list.D3DSetComputeRootDescriptorTable(1 + i, tables[i]->second);
  }
  cp_.SetExternalPipeline(pipeline);
  command_list.D3DDispatch((constants.size[0] + 7) / 8,
                           (constants.size[1] + 7) / 8, 1);
}

}  // namespace d3d12
}  // namespace gpu
}  // namespace xe
