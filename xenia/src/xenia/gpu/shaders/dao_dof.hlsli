// DAO: cinematic depth of field (dao_upscale_dof.cc), in conversations and
// cutscenes, on the upscaled scene before the UI goes over it:
//   the focus, from the scene depth around the middle of the screen
//   (dao_dof_focus.cs), eased over frames, at once on a cut ->
//   the scene at half size, linear, with each pixel's circle of confusion
//   (dao_dof_prefilter.cs) ->
//   a disc of samples gathered around each half size pixel
//   (dao_dof_blur.cs) ->
//   the blurred image blended over the sharp one by the circle of confusion
//   (dao_dof_combine.cs).
// The circle of confusion comes from the scene's device depth alone: with the
// far plane far beyond the near one, a pixel's camera distance is the near
// distance over its depth's distance from the far end, so the ratio of two
// distances, all the lens needs, doesn't depend on either plane.

// t0 the scene depth (render size, device depth), t1 the focus (2x1: the
// distance in near-plane units, and the focus area's median distance, for
// finding cuts), t2 and t3 the pass's own.
Texture2D<float> dao_dof_depth : register(t0);
Texture2D<float> dao_dof_focus : register(t1);
SamplerState dao_dof_linear : register(s0);

// Mirrored by DofConstants and kDofFlag* in d3d12/dao_upscale_impl.h: keep them
// in step.
cbuffer DaoDofConstants : register(b0) {
  uint2 dao_dof_display_size;
  // dao_dof_display_size / 2, rounded up.
  uint2 dao_dof_half_size;
  uint2 dao_dof_render_size;
  // The circle of confusion's radius, in display pixels, of a surface at
  // infinity with the lens focused at 1 (it grows with the lens: narrower
  // fields of view blur more), and the largest radius applied.
  float dao_dof_aperture;
  float dao_dof_max_radius;
  // How much of the effect is applied, 0 to 1 (eased in and out with the
  // game's shots).
  float dao_dof_strength;
  // How far the focus moves towards the new one each frame, 0 to 1.
  float dao_dof_focus_rate;
  // The gamma the scene is encoded with, 0 if it's linear.
  float dao_dof_gamma;
  // The blur's samples.
  uint dao_dof_samples;
  // kDaoDofFlag*.
  uint dao_dof_flags;
};

// The focus is taken at once (a cut, or the effect starting).
static const uint kDaoDofFlagSnap = 1u;
// Shows the circle of confusion (red far, blue near) instead of the scene.
static const uint kDaoDofFlagShow = 2u;
// The depth is reversed (1 near), as most of the scene's draws tested it.
static const uint kDaoDofFlagDepthReversed = 4u;
// Linear color the blur takes at most (a highlight's bokeh, not infinity).
static const float kDaoDofMaxColor = 64.0;

// A pixel's camera distance in near-plane units (the sky, at the far end,
// very far).
float DaoDofDistance(float depth) {
  float from_far = (dao_dof_flags & kDaoDofFlagDepthReversed) != 0u
                       ? depth
                       : 1.0 - depth;
  return 1.0 / max(from_far, 1.0e-7);
}

// The scene depth at a display pixel (the render pixel it's in).
float DaoDofDepthAt(uint2 display_pixel) {
  uint2 render_pixel = min(
      uint2((float2(display_pixel) + 0.5) * float2(dao_dof_render_size) /
            float2(dao_dof_display_size)),
      dao_dof_render_size - 1u);
  return dao_dof_depth.Load(int3(render_pixel, 0));
}

// The sharp zone around the focus, as multiples of its distance: from a
// little in front of it to well behind, so that in a shot of two people (the
// nearer one the focus) both are sharp, and only the scene around them blurs.
static const float kDaoDofSharpNear = 0.9;
static const float kDaoDofSharpFar = 1.75;

// The signed circle of confusion at a display pixel, in display pixels:
// positive behind the sharp zone, negative in front of it.
float DaoDofCoc(uint2 display_pixel) {
  float focus = dao_dof_focus.Load(int3(0, 0, 0));
  float distance = DaoDofDistance(DaoDofDepthAt(display_pixel));
  float edge = clamp(distance, focus * kDaoDofSharpNear,
                     focus * kDaoDofSharpFar);
  float coc = dao_dof_aperture * (1.0 - edge / distance);
  return clamp(coc, -dao_dof_max_radius, dao_dof_max_radius) *
         dao_dof_strength;
}

float3 DaoDofLinear(float3 color) {
  return dao_dof_gamma > 0.0 ? pow(max(color, 0.0), dao_dof_gamma)
                             : max(color, 0.0);
}

float3 DaoDofEncode(float3 color) {
  return dao_dof_gamma > 0.0 ? pow(max(color, 0.0), 1.0 / dao_dof_gamma)
                             : color;
}
