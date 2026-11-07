// DAO: motion vectors for the temporal upscalers. Right after each scene draw,
// its vertex shader ran again for each vertex the draw uses (dao_upscale.cc),
// writing their clip-space positions with stream output, with the jitter
// (taken out here); the positions one frame ago are what the matching draw of
// the previous frame wrote. This pass draws the draw's triangles again (its
// indices, from the shared memory) into the motion vector target, keeping only
// the pixels where they are the visible surface in the scene depth. The scene
// depth is also in the depth buffer, so the depth test can reject most of the
// hidden pixels before the pixel shader (conservatively: the vertex shader
// moves the triangles towards the camera by a bit more than the tolerance, and
// the slope-scaled depth bias by twice the larger of the x and y slopes, while
// the pixel shader allows their sum).
ByteAddressBuffer dao_mv_positions : register(t0);
ByteAddressBuffer dao_mv_previous_positions : register(t1);
Texture2D<float> dao_mv_scene_depth : register(t2);
ByteAddressBuffer dao_mv_shared_memory : register(t3);

// dao_mv_index_flags.
static const uint kDaoMvIndexed = 1u;
static const uint kDaoMvIndex32 = 1u << 1;
static const uint kDaoMvStrip = 1u << 2;
static const uint kDaoMvEndianShift = 4;

cbuffer DaoMvConstants : register(b0) {
  // Byte offsets of this draw's current and previous positions (float4 each,
  // from the vertex *_first on, *_count of them, in dao_mv_positions and
  // dao_mv_previous_positions).
  uint dao_mv_current_offset;
  uint dao_mv_previous_offset;
  uint dao_mv_current_first;
  uint dao_mv_current_count;
  uint dao_mv_previous_first;
  uint dao_mv_previous_count;
  // The draw's indices (a byte address in the shared memory) and how its
  // vertex shader makes them vertex indices: kDaoMv* flags with the
  // xenos::Endian, then the offset added, the 24-bit mask and the clamp.
  uint dao_mv_index_address;
  uint dao_mv_index_flags;
  uint dao_mv_vertex_index_offset;
  uint dao_mv_vertex_index_min;
  uint dao_mv_vertex_index_max;
  uint dao_mv_padding0;
  // Host viewport of the draw, in pixels relative to the screen (the window
  // offset of the tile removed).
  float2 dao_mv_viewport_offset;
  float2 dao_mv_viewport_extent;
  float2 dao_mv_render_size;
  // FSR jitter of this frame, in pixels: the scene was rasterized shifted by it.
  float2 dao_mv_jitter;
  float dao_mv_z_min;
  float dao_mv_z_max;
  // Added to the depth for the depth test: towards the camera, negative if
  // nearer is less, positive if nearer is greater, 0 if the draw's depth test
  // is neither (then the pass has none).
  float dao_mv_depth_test_offset;
  float dao_mv_padding;
  // The jitter in the current and previous positions (NDC offset per W).
  float2 dao_mv_current_ndc_jitter;
  float2 dao_mv_previous_ndc_jitter;
};

// Larger depth differences from the scene are other surfaces
// (kMotionDepthTolerance in dao_upscale.cc must be the same).
static const float kDaoMvDepthTolerance = 2e-5;

struct DaoMvInterpolators {
  float4 position : SV_Position;
  // Previous clip-space position (x, y, w).
  float3 previous : TEXCOORD0;
  // The depth without the depth test's tolerance (interpolated like
  // SV_Position.z).
  noperspective float depth : TEXCOORD1;
};

// The vertex index the draw's vertex shader fetched for the index (or for the
// vertex number, not indexed), as it does it (DxbcShaderTranslator's
// StartVertexShader_LoadVertexIndex and RemapAndConvertVertexIndices).
uint DaoMvVertexIndex(uint index) {
  uint endian = (dao_mv_index_flags >> kDaoMvEndianShift) & 3u;
  // xenos::Endian: 1 8-in-16, 2 8-in-32, 3 16-in-32.
  if (endian == 1u || endian == 2u) {
    index = ((index & 0x00FF00FFu) << 8) | ((index >> 8) & 0x00FF00FFu);
  }
  if (endian == 2u || endian == 3u) {
    index = (index << 16) | (index >> 16);
  }
  index = (index + dao_mv_vertex_index_offset) & 0xFFFFFFu;
  return clamp(index, dao_mv_vertex_index_min, dao_mv_vertex_index_max);
}

// Clip space of the draw's host viewport to screen pixels (y down).
float2 DaoMvToScreen(float2 clip_xy, float w) {
  float2 ndc = clip_xy / w;
  return dao_mv_viewport_offset +
         float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * dao_mv_viewport_extent;
}
