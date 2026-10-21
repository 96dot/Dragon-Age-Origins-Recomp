// DAO: depth of field (see dao_dof.hlsli), the scene at half size: each half
// size pixel the average of its 2x2 display pixels, linear, and in alpha the
// smallest of their signed circles of confusion (the most in front, or the
// least behind), in half size pixels: an edge of something in focus doesn't
// take the blur behind it.
#include "dao_dof.hlsli"

Texture2D<float4> dao_dof_scene : register(t2);
RWTexture2D<float4> dao_dof_half_out : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= dao_dof_half_size)) {
    return;
  }
  float3 color = 0.0;
  float coc = dao_dof_max_radius;
  [unroll] for (uint i = 0u; i < 4u; ++i) {
    uint2 pixel = min(id.xy * 2u + uint2(i & 1u, i >> 1u),
                      dao_dof_display_size - 1u);
    color += min(DaoDofLinear(dao_dof_scene[pixel].rgb), kDaoDofMaxColor);
    coc = min(coc, DaoDofCoc(pixel));
  }
  dao_dof_half_out[id.xy] = float4(color * 0.25, coc * 0.5);
}
