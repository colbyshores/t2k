#pragma once
// ============================================================================
// og_profile.h — the "pretend this is an Old 3DS" TEST KNOB (3DS build only).
//
// A New 3DS can be made to behave like an OG one closely enough to be worth
// measuring against, because the two differ in exactly three things this
// codebase touches:
//
//   1. CPU CLOCK + L2. osSetSpeedupEnable(true) unlocks 804 MHz AND the 2 MB
//      L2 cache in one call. Skipping it leaves the game thread at the OG's
//      268 MHz with L2 off — a ~3x swing, and the dominant difference. NB the
//      GPU is the SAME PICA200 at the same clock on both models, so every GPU
//      millisecond already transfers 1:1 and only the CPU half is emulated
//      here (DOCTRINE.md, the hardware-AA note).
//   2. SPARE-CORE LAYOUT. New has core 2 free for workers; OG has only
//      syscore 1, and using it requires an explicit APT_SetAppCpuTimeLimit
//      grant. Worker placement changes what contends with audio.
//   3. RAM. Best-effort only — see og_profile_squeeze_ram(). The CPU half is
//      exact; the RAM half is an approximation and is logged as such.
//
// This is a MEASUREMENT INSTRUMENT, not a feature: it exists so an OG-profile
// number can be taken on a New device without owning both. Per the project's
// scaffolding-hygiene rule, delete it once it has answered its question — or,
// if it earns a permanent place, say so explicitly in this comment.
// ============================================================================

namespace ts {

// Set once at boot from GameConfig::og_profile, before anything reads it.
void og_profile_set(bool on);
bool og_profile_active();

// "Is this a New 3DS?" — the ONE answer the whole 3DS build should use, so a
// forced OG profile cannot be half-applied. Wraps APT_CheckNew3DS and returns
// false whenever the override is on.
bool platform_is_new_3ds();

// Best-effort RAM squeeze toward an OG-sized budget: reserves the difference
// so later allocations run under OG-like pressure. Call EARLY (before the
// renderer and its texture masters allocate) or it proves nothing. Reserving
// less than requested is not an error — it logs what it actually took, and an
// allocation failing afterwards is a RESULT, not a bug.
void og_profile_squeeze_ram();

} // namespace ts
