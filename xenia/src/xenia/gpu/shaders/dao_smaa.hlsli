// DAO: SMAA 1x, Enhanced Subpixel Morphological Antialiasing (Jimenez et al.,
// thirdparty/smaa), on 3D scenes without temporal upscaling (dao_smaa.cc), in
// place of the game's 2xMSAA. Three compute passes at the render size:
//   edges, from the luma of the image before the UI (dao_smaa_edges.cs) ->
//   each edge pixel's blending weights, from the line it's on and its ends
//   (dao_smaa_weights.cs, SMAA's own) ->
//   each pixel blended with the neighbours its weights point to, and the UI
//   put back over it (dao_upscale_composite_smaa.cs).
// The edges are found in the image as the game encodes it (luma differences as
// they're seen); the blending is linear (true HDR's scene, or the SDR image
// linearized).

// Mirrored by SmaaConstants in d3d12/dao_upscale_impl.h: keep them in step.
cbuffer DaoSmaaConstants : register(b0) {
  // 1 / width, 1 / height, width, height: SMAA's SMAA_RT_METRICS.
  float4 dao_smaa_metrics;
  uint2 dao_smaa_size;
};

SamplerState dao_smaa_linear : register(s0);
SamplerState dao_smaa_point : register(s1);

// SMAA's HLSL 4 porting macros, with level 0 for its implicit-level samples (a
// compute shader has no derivatives; SMAA's textures have one level).
#define SMAA_CUSTOM_SL 1
#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) \
  tex.SampleLevel(dao_smaa_linear, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) \
  tex.SampleLevel(dao_smaa_point, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) \
  tex.SampleLevel(dao_smaa_linear, coord, 0, offset)
#define SMAASample(tex, coord) SMAASampleLevelZero(tex, coord)
#define SMAASamplePoint(tex, coord) SMAASampleLevelZeroPoint(tex, coord)
#define SMAASampleOffset(tex, coord, offset) \
  SMAASampleLevelZeroOffset(tex, coord, offset)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
#define SMAA_RT_METRICS dao_smaa_metrics
// "95% of the quality" by SMAA's own account, with its diagonal and corner
// handling (ULTRA's: 99%, with twice the search steps and a lower threshold,
// which finds more texture detail as edges). The edges pass reads its
// threshold too.
#define SMAA_PRESET_HIGH 1
#include "../../../../../thirdparty/smaa/SMAA.hlsl"
