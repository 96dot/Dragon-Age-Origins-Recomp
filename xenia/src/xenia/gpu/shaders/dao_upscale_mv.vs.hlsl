#include "dao_upscale_mv.hlsli"

DaoMvInterpolators main(uint vertex_id : SV_VertexID) {
  DaoMvInterpolators output;
  // The draw's triangles as a list: a strip's triangle i is its vertices i to
  // i + 2 (the winding doesn't matter here).
  uint position = vertex_id;
  if (dao_mv_index_flags & kDaoMvStrip) {
    position = vertex_id / 3u + vertex_id % 3u;
  }
  uint index = position;
  if (dao_mv_index_flags & kDaoMvIndexed) {
    if (dao_mv_index_flags & kDaoMvIndex32) {
      index = dao_mv_shared_memory.Load(dao_mv_index_address + position * 4u);
    } else {
      uint address = dao_mv_index_address + position * 2u;
      index = dao_mv_shared_memory.Load(address & ~3u);
      index = (address & 2u) ? index >> 16 : index & 0xFFFFu;
    }
  }
  uint vertex = DaoMvVertexIndex(index);
  // A vertex whose positions weren't written (the indices changed since the
  // draw, or a frame ago it used others) is culled with its triangle.
  uint current_vertex = vertex - dao_mv_current_first;
  uint previous_vertex = vertex - dao_mv_previous_first;
  if (current_vertex >= dao_mv_current_count ||
      previous_vertex >= dao_mv_previous_count) {
    output.position = asfloat(0x7FC00000).xxxx;
    output.previous = float3(0.0, 0.0, 1.0);
    output.depth = 0.0;
    return output;
  }
  float4 current = asfloat(
      dao_mv_positions.Load4(dao_mv_current_offset + current_vertex * 16u));
  float4 previous = asfloat(dao_mv_previous_positions.Load4(
      dao_mv_previous_offset + previous_vertex * 16u));
  // Written as drawn, jittered: without the jitter.
  current.xy -= dao_mv_current_ndc_jitter * current.w;
  previous.xy -= dao_mv_previous_ndc_jitter * previous.w;
  // Rasterize where the scene was: the current position plus the jitter, on
  // the full-frame motion vector target, with the draw's depth range.
  float2 screen = DaoMvToScreen(current.xy, current.w) + dao_mv_jitter;
  float2 ndc = float2(screen.x / dao_mv_render_size.x * 2.0 - 1.0,
                      1.0 - screen.y / dao_mv_render_size.y * 2.0);
  float depth = dao_mv_z_min + current.z / current.w * (dao_mv_z_max - dao_mv_z_min);
  output.position =
      float4(ndc, depth + dao_mv_depth_test_offset, 1.0) * current.w;
  output.previous = previous.xyw;
  output.depth = depth;
  return output;
}
