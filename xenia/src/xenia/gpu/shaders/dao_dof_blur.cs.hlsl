// DAO: depth of field (see dao_dof.hlsli), the blur at half size: samples on a
// disc (a golden angle spiral, outwards), each one counted where its own
// circle of confusion reaches this pixel, else as the average so far (after
// Dennis Gustafsson's single pass bokeh). A sample behind this pixel reaches
// it by at most twice this pixel's circle: a sharp face doesn't take the
// blurred wall's color, while a blurred near shoulder spreads over it. Alpha:
// how far the samples counted reach, for the combine (a near blur over
// something in focus).
#include "dao_dof.hlsli"

Texture2D<float4> dao_dof_half : register(t2);
RWTexture2D<float4> dao_dof_blur_out : register(u0);

static const float kDaoDofGoldenAngle = 2.39996323;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= dao_dof_half_size)) {
    return;
  }
  float2 texel = 1.0 / float2(dao_dof_half_size);
  float2 uv = (float2(id.xy) + 0.5) * texel;
  float4 center = dao_dof_half[id.xy];
  float center_size = abs(center.a);
  float3 color = center.rgb;
  float reach = center_size;
  float total = 1.0;
  // In half size pixels.
  float radius_max = dao_dof_max_radius * 0.5 * dao_dof_strength;
  float samples = float(dao_dof_samples);
  for (uint i = 0u; i < dao_dof_samples; ++i) {
    float radius = radius_max * sqrt((float(i) + 0.5) / samples);
    float angle = float(i) * kDaoDofGoldenAngle;
    float2 offset = float2(cos(angle), sin(angle)) * radius;
    float4 tap = dao_dof_half.SampleLevel(dao_dof_linear, uv + offset * texel,
                                          0.0);
    float size = abs(tap.a);
    if (tap.a > center.a) {
      size = min(size, center_size * 2.0);
    }
    float counted = saturate(size - radius + 0.5);
    color += lerp(color / total, tap.rgb, counted);
    reach += lerp(reach / total, size, counted);
    total += 1.0;
  }
  dao_dof_blur_out[id.xy] = float4(color / total, reach / total);
}
