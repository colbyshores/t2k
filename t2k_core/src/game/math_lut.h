#pragma once

// =============================================================================
// Fast sin/cos/atan2 via cache-resident lookup tables.
//
// ONE PATH, BOTH TARGETS. This header used to be `#if __3DS__` LUT / `#else`
// libm -- a deliberate split so the desktop could act as an exact-trig oracle
// to diff the table against. That split is GONE (user decision 2026-08-21,
// "lets get everything on LUTs so simplify the codebase"), and removing it is
// worth more than the oracle was:
//
//   * IT CLOSES A HOLE DOCTRINE.md ITSELF FLAGGED. The WYSIWYG section records
//     that the bit-identity regression harnesses are host g++ builds and
//     therefore took the DESKTOP branch, so their md5s proved the roster was
//     unchanged on the LIBM path and said NOTHING about the 3DS, which ran
//     different arithmetic. With one path, the harnesses now gate the
//     arithmetic the 3DS actually executes.
//   * IT IS THE WYSIWYG RULE APPLIED TO ITSELF. "No platform-specific fast
//     paths for shared math -- a SIMD sine on desktop and a LUT on 3DS is two
//     different games agreeing approximately." This header was the one
//     standing exception to that rule; it is no longer an exception.
//   * The desktop pays nothing it cannot afford. A table read is not slower
//     than a libm call, and the scene carries so little compute that it would
//     not matter if it were.
//
// Measured cost of the unification, field by field, over 1.15M records from 40
// level/roster combinations driven through the REAL game_tick and the REAL
// shared builders (tools/trig_harness.cpp): every INTEGER and BOOLEAN field
// identical -- no lane index, spawn count, animation phase, alive flag, score
// or draw/segment count moved anywhere. Floats drift at the ~1e-6 relative
// scale the table's own interpolation error predicts. See the commit for the
// full per-family table.
//
// The ARM11 (ARMv6K / VFPv2) has NO hardware transcendental instruction, so
// every std::sin/std::cos is a ~50-150-cycle libm call. The per-frame geometry
// BUILD is the 3DS bottleneck (build/issue perf split: build 12-26ms, issue
// ~1ms), and it issues ~22,000 sin/cos per frame over the grid alone
// (textureLevel: 4 per vertex x 4480 verts; the tremor wave; plus per-entity /
// per-segment rotations). Routing those through a table collapses each call to
// a ~few-cycle lookup.
//
// Table: 1024 entries over [0, 2*PI), linearly interpolated. 4KB -> fits the
// ARM11 16KB L1 D-cache on BOTH OG3DS and New3DS; on New3DS it also rides the
// 2MB L2 we enable via osSetSpeedupEnable, alongside the rest of the build's
// hot working set. Lifted from the Forsaken port (new3d.c fast_sinf) -- our
// cross-port rosetta stone.
//
// ACCURACY -- AND THE PART THIS HEADER USED TO STATE INCOMPLETELY. It claimed
// "< 4.7e-6 max error vs sinf" flat. That number is real but it is PER-PERIOD;
// the error GROWS WITH |rad|, and nothing said so. Measured 2026-08-21:
//
//     range              max |fastSin(x) - sin(x)|
//     [0, 2*PI)                    4.75e-06
//     |x| <   100 rad              8.55e-06
//     |x| <  1000 rad              8.84e-05
//     |x| < 10000 rad              7.82e-04
//
// THE TABLE IS NOT THE CAUSE -- `rad * TRIG_SCALE` is, because it is a FLOAT
// multiply and its result's own quantum grows with magnitude: at |rad|~1 the
// index ULP is 1.5e-05 (an angle quantum of 9.4e-08 rad), at |rad|~10000 it is
// 0.125 (7.7e-04 rad). Past that the interpolation fraction is quantised more
// coarsely than the table is spaced.
//
// PRACTICAL READ, since every phase here is time-derived: `engine.time` is
// MILLISECONDS, so a `time * 0.001f` phase reaches 1000 rad after ~17 minutes
// of play and a `time * 0.1f` phase (line_geometry.cpp's embryo pulse, among
// others) gets there in ~10 seconds. This is PRE-EXISTING, SHIPPED, PLAY-TESTED
// behaviour on the 3DS -- the reference target has always run these phases
// through this table -- so it is documented here, not "fixed". If a NEW effect
// ever needs a phase stable over hours, wrap its argument into [0, 2*PI) at the
// call site rather than widening the table; the fix belongs to the caller that
// needs it, not to every lookup in the frame.
//
// fastAtan2's error does not have this problem -- its argument is a ratio in
// [0,1], so it stays at 1.50e-06 rad everywhere (measured over the same pass).
//
// NOTE: sqrt/div are deliberately NOT tabled -- VFPv2 has hardware
// vsqrt.f32/vdiv, which are faster AND exact than any table or bit-hack
// (Forsaken learned this the hard way ripping out the Quake-III fast-invsqrt).
// We only need -fno-math-errno (in Makefile.3ds) so GCC emits vsqrt/vdiv
// instead of the errno-setting libm call.
//
// WHAT IS DELIBERATELY *NOT* ROUTED THROUGH HERE, so a later sweep does not
// "finish the job" and change the game:
//   * GAMEPLAY trig. engine.cpp's per-level spawn counts
//     (`round(sin(...)*12.5 + sin(...)*7.5 + cos(...)*8.0)`) and weapons.cpp's
//     tremor decay are ported the reference build math in DOUBLE precision. MECHANICS ARE
//     GATED; a ULP shift there can flip a round() boundary and change how many
//     enemies a level spawns.
//   * The procedural texture DSL (textures.cpp) and the init-time mesh
//     builders (primitives.cpp) -- they run once and define asset output.
//   * audio/fft.cpp, which already builds its own twiddle tables.
//   * The GLSL strings in shaders.cpp, which are shader source, not C++.
// =============================================================================

namespace ts {
namespace mathlut {

constexpr int   TRIG_TABLE_SIZE = 1024;
constexpr int   TRIG_TABLE_MASK = TRIG_TABLE_SIZE - 1;
constexpr float TWO_PI          = 6.28318530717958647692f;
constexpr float PI_F            = 3.14159265358979323846f;
constexpr float HALF_PI         = 1.57079632679489661923f;
constexpr float TRIG_SCALE      = (float)TRIG_TABLE_SIZE / TWO_PI;

// atan2 is the OTHER transcendental this port calls per-item (entity facing
// angles, the starfield's rim lookup), and libm's is dearer than sin/cos, not
// cheaper. Only atan(z) over z in [0,1] is tabulated -- 257 floats, 1KB -- and
// the other seven octants fold onto it by symmetry, so the whole of atan2 costs
// one divide plus one interpolated read. Linear-interp error is ~1.2e-6 rad.
constexpr int   ATAN_TABLE_SIZE = 256;

/// Build the tables. CALL ONCE AT BOOT, BEFORE ANY fastSin/fastCos/fastAtan2.
///
/// THIS IS NOW LOAD-BEARING ON BOTH TARGETS. It used to be a no-op on desktop,
/// so forgetting it there cost nothing; now an uncalled init leaves the tables
/// zero-filled and EVERY trig call returns 0. Deliberately an explicit call
/// rather than a global constructor (kept off the 3DS boot path) or a
/// function-local static (whose thread-safe guard would land in the hot loop).
///
/// Called from: src/main.cpp (desktop), src/platform_3ds/main_3ds.cpp (3DS),
/// tools/trig_harness.cpp. Any NEW entry point -- including a host-side test
/// harness -- must call it too.
void mathLutInit();

// +1 slot so the i+1 interpolation read at the last entry stays in-bounds.
extern float g_sinTable[TRIG_TABLE_SIZE + 1];
extern float g_atanTable[ATAN_TABLE_SIZE + 1];   // atan(i / ATAN_TABLE_SIZE)

} // namespace mathlut

// Inlined into the hot per-vertex loops (grid/entity/line build) so the lookup
// has no call overhead. Adapted from Forsaken new3d.c fast_sinf.
//
// THE RANGE REDUCTION MUST NOT USE __builtin_floorf. It looks like a builtin,
// but on the 3DS target (devkitARM g++ 15.2.0, ARCH = -march=armv6k
// -mfloat-abi=hard -mfpu=vfp, -O2 and no -ffast-math) VFPv2 has no float-floor
// instruction, so GCC emits a real `bl floorf` into newlib -- plus a `push
// {r4,lr}` / `vpush {d8}` frame, which also forces every CALLER to spill its
// VFP registers around what is supposed to be a leaf lookup. Measured on the
// shipped flags: 10 `bl floorf` in rendering/shatter.cpp's evalWorld alone, and
// newlib's floorf carries two VFP->ARM flag transfers (vmov + vcmpe/vmrs) that
// stall the ARM11 pipeline. At the ~22,000 calls/frame this header cites for
// the grid that is on the order of 2-3 ms EVERY frame, against a build region
// measured at 12-26 ms -- i.e. the "optimization" was costing more than the
// libm call it replaced.
//
// Integer truncation + a floor correction is exact and compiles to a LEAF
// function (verified on the same toolchain: no `bl`, ends in `bx lr`). (int)
// truncates toward zero, so one branchless-ish fixup turns it into floor; the
// mask then folds the index mod 1024 (two's complement wrap handles negatives:
// -1 & 1023 == 1023).
//
// DOMAIN: |rad| < ~1.3e7 radians. Past that rad*TRIG_SCALE overflows int32 and
// the (int) cast is UB (VFP saturates). Every caller here feeds either a bounded
// angle or a time-derived phase -- rotateMat (rendering/math_utils.h) folds its
// angle into [0,360) first, so its callers are bounded by construction. The
// fastest UNWRAPPED phase is line_geometry.cpp's enemy-shot shimmer
// (`time * 0.1f` into a `* PI * 0.05f`, ~0.0157 rad/ms), which reaches the limit
// after ~10 days of continuous runtime; grid_geometry.cpp's tremor wave
// (`time * 0.001f` under `* PI`) after ~48. So this is documented rather than
// paid for with a per-call branch -- but it is NOT the first thing a long uptime
// breaks: float(timeMs) stops being exact at 2^24 ms (4.7 h) and the int ms
// counter wraps at 24.9 days (game/phase.h). phase.h is the fix for a phase that
// must survive cabinet uptimes, and the `time * PI * 0.0005f` (~96 days) this
// paragraph used to cite as the fastest caller has already moved there -- it is
// the web base colour, now web_palette.h's WEB_BASE_ANIM_* integer turns, and is
// no longer a caller of this function at all.
inline float fastSin(float rad) {
    const float idx = rad * mathlut::TRIG_SCALE;
    int   i = (int)idx;                 // truncates toward zero
    float f = idx - (float)i;
    if (f < 0.0f) { --i; f += 1.0f; }   // -> floor(idx), fraction back in [0,1)
    i &= mathlut::TRIG_TABLE_MASK;
    return mathlut::g_sinTable[i] + (mathlut::g_sinTable[i + 1] - mathlut::g_sinTable[i]) * f;
}
inline float fastCos(float rad) { return fastSin(rad + mathlut::HALF_PI); }

// Octant-folded atan2. The table covers only atan(z), z in [0,1]; |y|<=|x| reads
// it directly and |y|>|x| reads the complement, which is why one 257-entry table
// serves the full circle. Matches std::atan2's branch cuts, including the
// axis cases (x==0 -> +-PI/2, y==0 && x<0 -> +PI) and the (0,0) -> 0 convention.
inline float fastAtan2(float y, float x) {
    const float ax = __builtin_fabsf(x), ay = __builtin_fabsf(y);
    if (ax == 0.0f && ay == 0.0f) return 0.0f;
    const bool  steep = (ay > ax);
    const float z = (steep ? (ax / ay) : (ay / ax)) * (float)mathlut::ATAN_TABLE_SIZE;
    int i = (int)z;
    if (i >= mathlut::ATAN_TABLE_SIZE) i = mathlut::ATAN_TABLE_SIZE - 1;
    const float f = z - (float)i;
    const float t = mathlut::g_atanTable[i]
                  + (mathlut::g_atanTable[i + 1] - mathlut::g_atanTable[i]) * f;
    float a = steep ? (mathlut::HALF_PI - t) : t;      // a = atan(|y|/|x|), in [0, PI/2]
    if (x < 0.0f) a = mathlut::PI_F - a;               // fold to the left half-plane
    return (y < 0.0f) ? -a : a;                        // mirror below the x axis
}

} // namespace ts
