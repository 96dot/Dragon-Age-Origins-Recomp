// DAO: depth of field (see dao_dof.hlsli), the blurred image over the sharp
// one at the display size: blurred where the pixel's circle of confusion, or
// a near blur's reach over it, is more than a pixel or so. The blurred image
// is taken through a tent (four bilinear samples a half size pixel apart),
// which fills the gaps between the blur's samples.
#include "dao_dof.hlsli"

Texture2D<float4> dao_dof_scene : register(t2);
Texture2D<float4> dao_dof_blurred : register(t3);
RWTexture2D<float4> dao_dof_out : register(u0);

// Display pixels of circle of confusion where the blend starts, and over
// which it reaches the blurred image.
static const float kDaoDofSharp = 1.0;
static const float kDaoDofBlend = 2.0;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= dao_dof_display_size)) {
    return;
  }
  float4 sharp = dao_dof_scene[id.xy];
  float coc = DaoDofCoc(id.xy);
  if ((dao_dof_flags & kDaoDofFlagShow) != 0u) {
    float shown = saturate(abs(coc) / max(dao_dof_max_radius, 1.0));
    float3 tint = coc > 0.0 ? float3(1.0, 0.25, 0.1) : float3(0.1, 0.4, 1.0);
    float3 gray = dot(DaoDofLinear(sharp.rgb), float3(0.2126, 0.7152, 0.0722));
    dao_dof_out[id.xy] =
        float4(DaoDofEncode(lerp(gray * 0.5, tint, shown)), 1.0);
    return;
  }
  float2 texel = 1.0 / float2(dao_dof_half_size);
  float2 uv = (float2(id.xy) + 0.5) / float2(dao_dof_display_size);
  float4 blurred =
      (dao_dof_blurred.SampleLevel(dao_dof_linear, uv + float2(-0.5, -0.5) * texel, 0.0) +
       dao_dof_blurred.SampleLevel(dao_dof_linear, uv + float2(0.5, -0.5) * texel, 0.0) +
       dao_dof_blurred.SampleLevel(dao_dof_linear, uv + float2(-0.5, 0.5) * texel, 0.0) +
       dao_dof_blurred.SampleLevel(dao_dof_linear, uv + float2(0.5, 0.5) * texel, 0.0)) *
      0.25;
  // (The reach is in half size pixels.)
  float size = max(abs(coc), blurred.a * 2.0);
  float amount = saturate((size - kDaoDofSharp) / kDaoDofBlend);
  if (amount <= 0.0) {
    // Untouched: not through the gamma and back.
    dao_dof_out[id.xy] = sharp;
    return;
  }
  float3 color = lerp(DaoDofLinear(sharp.rgb), blurred.rgb, amount);
  dao_dof_out[id.xy] = float4(DaoDofEncode(color), sharp.a);
}
