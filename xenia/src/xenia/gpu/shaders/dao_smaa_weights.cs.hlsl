// DAO: SMAA's blending weights (see dao_smaa.hlsli): its own
// SMAABlendingWeightCalculationVS and PS for each pixel (no subsample indices:
// SMAA 1x).
#include "dao_smaa.hlsli"

// dao_smaa_edges.cs's edges, as UNORM R8G8: SMAA reads two pixels' edges with
// one bilinear fetch between them and tells them apart by the blended value.
// SMAA's area and search textures (AreaTex.h, SearchTex.h).
Texture2D<float4> dao_smaa_edges : register(t0);
Texture2D<float4> dao_smaa_area : register(t1);
Texture2D<float4> dao_smaa_search : register(t2);
RWTexture2D<float4> dao_smaa_weights : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= dao_smaa_size)) {
    return;
  }
  float2 texcoord = (float2(id.xy) + 0.5) * dao_smaa_metrics.xy;
  float2 pixcoord;
  float4 offset[3];
  SMAABlendingWeightCalculationVS(texcoord, pixcoord, offset);
  dao_smaa_weights[id.xy] = SMAABlendingWeightCalculationPS(
      texcoord, pixcoord, offset, dao_smaa_edges, dao_smaa_area,
      dao_smaa_search, float4(0.0, 0.0, 0.0, 0.0));
}
