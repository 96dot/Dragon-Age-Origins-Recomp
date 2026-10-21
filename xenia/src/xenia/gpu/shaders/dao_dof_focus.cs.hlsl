// DAO: depth of field's focus (see dao_dof.hlsli), one group: the scene's
// distances on a 16x16 grid over most of the screen's upper two thirds
// (where the game's conversation shots frame the faces: one in the middle, or
// two either side of it), sorted; the focus is the nearer quarter's, so the
// people in front of a far wall are the ones in focus.
// It's eased towards that over frames (a slow pull within a shot), or taken at
// once on a cut: the area's median distance changing by more than half.
#include "dao_dof.hlsli"

// (x 0: the focus, x 1: the median; R32_FLOAT, which any GPU loads from as
// unordered access.)
RWTexture2D<float> dao_dof_focus_out : register(u0);

// The grid's area, as fractions of the screen.
static const float2 kDaoDofAreaMin = float2(0.15, 0.18);
static const float2 kDaoDofAreaMax = float2(0.85, 0.6);
// Beyond this (near-plane units) a sample is the sky: not a focus.
static const float kDaoDofSky = 1.0e5;
// The median's change (as a ratio) that is a cut.
static const float kDaoDofCut = 1.5;

groupshared float dao_dof_samples_shared[256];

[numthreads(16, 16, 1)]
void main(uint3 id : SV_GroupThreadID, uint index : SV_GroupIndex) {
  float2 position = lerp(kDaoDofAreaMin, kDaoDofAreaMax,
                         (float2(id.xy) + 0.5) / 16.0);
  uint2 render_pixel = min(uint2(position * float2(dao_dof_render_size)),
                           dao_dof_render_size - 1u);
  dao_dof_samples_shared[index] = min(
      DaoDofDistance(dao_dof_depth.Load(int3(render_pixel, 0))), 1.0e7);
  GroupMemoryBarrierWithGroupSync();
  // Bitonic sort, ascending.
  [unroll] for (uint size = 2u; size <= 256u; size <<= 1u) {
    [unroll] for (uint stride = size >> 1u; stride > 0u; stride >>= 1u) {
      uint partner = index ^ stride;
      if (partner > index) {
        float a = dao_dof_samples_shared[index];
        float b = dao_dof_samples_shared[partner];
        bool ascending = (index & size) == 0u;
        if ((a > b) == ascending) {
          dao_dof_samples_shared[index] = b;
          dao_dof_samples_shared[partner] = a;
        }
      }
      GroupMemoryBarrierWithGroupSync();
    }
  }
  if (index != 0u) {
    return;
  }
  float target = dao_dof_samples_shared[64];
  float median = dao_dof_samples_shared[128];
  float2 previous =
      float2(dao_dof_focus_out[uint2(0, 0)], dao_dof_focus_out[uint2(1, 0)]);
  bool snap = (dao_dof_flags & kDaoDofFlagSnap) != 0u ||
              !(previous.x > 0.0 && previous.x < 1.0e8) ||
              !(previous.y > 0.0 && previous.y < 1.0e8) ||
              max(median / previous.y, previous.y / median) > kDaoDofCut;
  if (target > kDaoDofSky) {
    // Sky in the middle: keep the focus (or, with none yet, far).
    target = snap ? kDaoDofSky : previous.x;
  }
  // Eased in log distance: a pull from 2 to 4 m as smooth as 20 to 40 m.
  float focus = snap ? target
                     : exp(lerp(log(previous.x), log(target),
                                dao_dof_focus_rate));
  dao_dof_focus_out[uint2(0, 0)] = focus;
  dao_dof_focus_out[uint2(1, 0)] = median;
}
