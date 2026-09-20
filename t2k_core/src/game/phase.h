#pragma once

// ============================================================================
// phase.h — EXACT, ZERO-DRIFT PERIODIC PHASE, for a machine that never reboots.
//
// WHY THIS EXISTS: THE ARCADE CABINET.
//
// Every animated presentation effect in this engine takes its phase from
// `engine.time`, a millisecond count that starts at 0 when the process starts
// and then only grows — `now_ms() - startMs` on the 3DS, `SDL_GetTicks()` on
// the desktop. On a handheld that is fine; a session is minutes. A CABINET is
// switched on and left running, in attract mode, for weeks. At that timescale
// the old formulation has two separate failures, both MEASURED:
//
//   A. `(float)timeMs` STOPS BEING EXACT AT 2^24 ms = 4.7 HOURS. Past that the
//      millisecond count no longer round-trips through float, so the phase
//      quantises — and the quantum doubles with every doubling of uptime. As a
//      fraction of ONE FRAME's phase advance (grid_geometry.cpp:214's colour
//      cycle, 60 fps):
//
//          uptime    ULP(time)    angle quantum    vs one frame's motion
//           1 day       8 ms       0.0126 rad             48 %
//           1.6 days   16 ms       0.0251 rad             96 %
//           7 days     64 ms       0.1005 rad            383 %
//          14 days    128 ms       0.2011 rad            767 %
//
//      Past ~1.6 days every time-driven effect in the game — the web colour
//      sweep, the embryo pulse, the enemy wobbles, the pause melt — advances
//      in visible steps instead of smoothly. It looks like frame pacing has
//      broken. It has not; the phase has.
//
//   B. `int` MILLISECONDS OVERFLOW AT 2^31 ms = 24.9 DAYS, on BOTH targets
//      (`(int)(svcGetSystemTick() / (SYSCLOCK_ARM11/1000))` and
//      `(int)SDL_GetTicks()`). The count goes negative and every phase jumps.
//
// THE FIX IS TO STOP EXPRESSING A PHASE AS A FLOAT ANGLE AT ALL.
//
// A phase is a POINT ON A CIRCLE. The natural machine representation of a
// point on a circle is an unsigned integer where overflow IS the wrap — a
// Q0.32 fraction of a turn. Then:
//
//   * the wrap is EXACT and FREE. No fmodf, no floorf, no `k - (int)k`, no
//     range-reduction error, and nothing to get wrong at a period boundary.
//   * the precision is CONSTANT FOREVER: one part in 2^32 of a turn
//     (8.4e-8 degrees), on second one and on day one thousand. There is no
//     "quantum grows with uptime" term because there is no float exponent.
//   * it is IMMUNE TO DEFECT B. Modular arithmetic does not care that the
//     millisecond counter wrapped: (T mod 2^32)*K mod 2^32 == T*K mod 2^32.
//     A negative `engine.time` cast to uint32_t is still the correct residue,
//     so the phase keeps running exactly through the 24.9-day rollover.
//
// AND IT IS ABSOLUTE, NOT INTEGRATED. `phaseAt` is a pure function of the
// clock, not a per-frame accumulator. That matters more than it looks:
// an accumulator would make every effect FRAME-RATE DEPENDENT — a 3DS running
// at 20 fps and a cabinet at 60 fps would diverge, and a dropped frame would
// permanently shift the phase. DOCTRINE.md's Aesthetic Contract makes effect
// timing an invariant shared by both targets, so the phase has to be a
// function of time, and this one is.
//
// COST ON ARM11: a 32x64 integer multiply (`umull` + `mla`, a few cycles, no
// libcall — note it is a MULTIPLY, not a divide; ARMv6 has no integer divide
// at all, see AGENTS.md R11) plus a shift, then one `vcvt` for the
// interpolation fraction. The old form was a float multiply, a `vcvt`, a float
// subtract, a float compare feeding a BRANCH, and a mask. So this is about a
// wash on cycles and strictly better on accuracy — cycles are not why it is
// here.
//
// WHAT THIS IS NOT FOR:
//   * Anything MECHANICS-GATED. Spawn counts, tremor decay and the the reference build
//     world matrices stay in double, off the table, untouched (DOCTRINE.md,
//     AGENTS.md R4's standing exemption). This header is presentation only.
//   * Bounded angles. A lane angle, a facing direction, a `t` in [0,1] has no
//     drift problem — it never grows. Leave those alone; converting them buys
//     nothing and costs a reader's attention.
//   * A phase that must match a recorded baseline bit-for-bit. A converted
//     site lands within the shipped table's own interpolation error of where
//     it used to (both paths read the same 1024-entry table, whose measured
//     bound is 4.75e-6), but it is not the identical float. The trig harness
//     WILL see the move. That is expected and must be reported, not hidden.
//
// It deliberately does NOT modify math_lut.h — AGENTS.md section 7 puts that
// header off limits to perf work, and nothing here needs to change it. This
// reads the same shipped table through its public `extern`.
// ============================================================================

#include "math_lut.h"

#include <cstdint>

namespace ts {
namespace phase {

// A POINT on the circle: Q0.32 turns. 0 = 0 turns, 0x80000000 = half a turn.
// Overflow is the wrap and is the whole point, so this must stay UNSIGNED —
// signed overflow is UB and would hand the optimiser a licence it must not
// have here.
using Turns = uint32_t;

// A RATE: turns per millisecond, as Q0.48, evaluated at COMPILE TIME.
//
// WHY Q0.48 AND NOT Q0.32 — this is the difference between "no stutter" and
// "zero drift", and the cabinet asked for the second one. A rate rounded to
// Q0.32 is off by up to 2^-33 relative, which never quantises anything but
// does integrate into a slowly growing PHASE OFFSET: measured at ~18 degrees
// after two weeks of uptime for the grid colour sweep. Invisible — there is no
// reference to compare a colour sweep against — but it is not zero, and it is
// avoidable for one wider multiply. At Q0.48 the rate error is ~1.4e-11
// relative, i.e. under 0.04 degrees of accumulated phase after a YEAR of
// continuous running. That is zero for any purpose a cabinet has.
//
// `double` on purpose: this runs in a constant expression on the host
// compiler, never on the ARM11, so the standing "no double in hot paths" rule
// (R4) does not apply — and float would throw away 45 of the 48 bits we are
// building. Authoring a rate in Hz is the same number the chamber effects
// already use (FB_WOBBLE_HZ, MELT_WOB_HZ are turns per SECOND).
using Rate = uint64_t;

constexpr Rate perMs(double turnsPerMs) {
    return (Rate)(turnsPerMs * 281474976710656.0 + 0.5);   // 2^48
}
constexpr Rate fromHz(double hz)          { return perMs(hz * 0.001); }
constexpr Rate fromPeriodMs(double ms)    { return perMs(1.0 / ms); }

// The rate a legacy call site was ALREADY running at, so a conversion is a
// transcription rather than a retune. Both spellings appear in the tree:
//   fastSin(time * K)            -> fromRadPerMs(K)
//   fastSin(time * PI * K)       -> fromRadPerMs(PI * K), or fromPiPerMs(K)
constexpr Rate fromRadPerMs(double radPerMs) {
    return perMs(radPerMs * (1.0 / 6.28318530717958647692));
}
constexpr Rate fromPiPerMs(double piPerMs)  { return perMs(piPerMs * 0.5); }

// The phase of a `k`-rate oscillator at wall-clock `timeMs`.
//
// EXACT: bits 16..47 of the product ARE frac(timeMs * k) in Q0.32. Nothing is
// rounded here — the bits above 47 are the integer turn count, and throwing
// them away IS the modulo. Bits above 63 fall off the end of the uint64, which
// is the same modulo applied one step earlier and equally harmless.
//
// `timeMs` is taken UNSIGNED and callers pass `(uint32_t)engine.time` — see
// defect B above: after the signed counter rolls over at 24.9 days, the
// unsigned cast is still the correct residue, so the phase runs straight
// through the rollover without a hiccup.
inline Turns at(uint32_t timeMs, Rate k) {
    return (Turns)(((uint64_t)timeMs * k) >> 16);
}

// Add a per-object offset expressed in turns, e.g. a lane index or a depth
// stagger. Wraps exactly, like everything else here.
inline Turns offset(Turns p, Turns d) { return p + d; }

// A fixed fraction of a turn, for those offsets. `turns(0.25)` is a quarter.
constexpr Turns turns(double t) {
    return (Turns)(int64_t)((t - (double)(int64_t)t) * 4294967296.0 + 0.5);
}

// ONE RADIAN in Q0.32 turns. `i * TURNS_PER_RAD` converts an INTEGER-indexed
// radian offset (a per-gate or per-lane stagger, the commonest bounded offset
// in this tree) with no float in the chain at all: the multiply wraps
// modularly, so any i is in range. Residual 0.42/2^32 turns per radian, i.e.
// under 2e-9 turns for the indices this is used with.
constexpr Turns TURNS_PER_RAD =
    (Turns)(4294967296.0 / 6.28318530717958647692 + 0.5);

// `i` radians as turns, for an integer-indexed stagger. A free int-to-unsigned
// widening and one multiply -- there is no float in this conversion at all,
// which is the point. (It lives here rather than as a cast at the call site so
// the per-element loops that use it stay free of anything a reader -- or the
// VFP gate's textual rule -- could mistake for a float->int conversion.)
inline Turns radIndex(int i) { return (Turns)i * TURNS_PER_RAD; }

// ---- the table read ---------------------------------------------------------
// The shipped 1024-entry float sin table, indexed by the TOP 10 BITS and
// interpolated with the next 22. Same table, same interpolation, same accuracy
// as ts::fastSin within one period (its measured 4.75e-6 bound) — what changes
// is only that the ARGUMENT can no longer degrade, however long the machine
// has been on.
//
// Note what is absent compared with fastSin's range reduction: no multiply by
// TRIG_SCALE, no `(int)` truncation, no negative-fraction fixup branch, no
// mask. The bit layout has already done all of it.
inline float sinTurns(Turns p) {
    const uint32_t i = p >> 22;                       // top 10 bits: 0..1023
    const float    f = (float)(p & 0x3FFFFFu) * (1.0f / 4194304.0f);
    return mathlut::g_sinTable[i]
         + (mathlut::g_sinTable[i + 1] - mathlut::g_sinTable[i]) * f;
}

// A quarter turn is 0x40000000 and adding it wraps for free, so cos costs the
// same as sin — unlike the float path, where fastCos pays an extra add of
// HALF_PI onto an already-large argument.
inline float cosTurns(Turns p) { return sinTurns(p + 0x40000000u); }

// Back out to radians, for the handful of consumers that take an ANGLE rather
// than sampling a table (the melt mesh's wobble and lobe phases). The result is
// already inside one period, so this cannot reproduce the magnitude loss the
// float path had — that loss came from the size of the argument, and by here
// the argument is bounded by construction.
inline float radOf(Turns p) {
    return (float)p * (6.28318530717958647692f / 4294967296.0f);
}

} // namespace phase
} // namespace ts
