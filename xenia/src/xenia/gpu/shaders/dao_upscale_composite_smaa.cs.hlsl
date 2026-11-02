// dao_upscale_composite.cs at the render size, its scene anti-aliased by
// SMAA's blending weights (see dao_smaa.hlsli): without temporal upscaling.
#define DAO_SMAA 1
#include "dao_upscale_composite.hlsli"
