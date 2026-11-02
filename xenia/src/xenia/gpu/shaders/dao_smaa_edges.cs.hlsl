// DAO: SMAA's edges (see dao_smaa.hlsli): its SMAALumaEdgeDetectionPS with
// loads (SMAA's point samples, clamped). Every pixel is written, 0 where SMAA
// discards (SMAA clears its target first; this needs no clear).
#include "dao_smaa.hlsli"

// The image before the UI, as the game encodes it.
Texture2D<float4> dao_smaa_color : register(t0);
// Red: an edge on the left, green: on top.
RWTexture2D<float2> dao_smaa_edges : register(u0);

float Luma(int2 p) {
  p = clamp(p, int2(0, 0), int2(dao_smaa_size) - 1);
  return dot(dao_smaa_color.Load(int3(p, 0)).rgb,
             float3(0.2126, 0.7152, 0.0722));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= dao_smaa_size)) {
    return;
  }
  int2 p = int2(id.xy);
  float l = Luma(p);
  float l_left = Luma(p + int2(-1, 0));
  float l_top = Luma(p + int2(0, -1));
  float4 delta;
  delta.xy = abs(l - float2(l_left, l_top));
  float2 edges = step(SMAA_THRESHOLD, delta.xy);
  // Most pixels.
  if (dot(edges, float2(1.0, 1.0)) == 0.0) {
    dao_smaa_edges[id.xy] = float2(0.0, 0.0);
    return;
  }
  // Local contrast adaptation: an edge much weaker than one next to it isn't
  // one (it would blur that one's anti-aliasing).
  delta.zw = abs(l - float2(Luma(p + int2(1, 0)), Luma(p + int2(0, 1))));
  float2 max_delta = max(delta.xy, delta.zw);
  delta.zw = abs(float2(l_left, l_top) -
                 float2(Luma(p + int2(-2, 0)), Luma(p + int2(0, -2))));
  max_delta = max(max_delta, delta.zw);
  float final_delta = max(max_delta.x, max_delta.y);
  edges *= step(final_delta, SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR * delta.xy);
  dao_smaa_edges[id.xy] = edges;
}
