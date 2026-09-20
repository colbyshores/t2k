#pragma once

// ============================================================================
// attract.h — WHEN the attract demo runs. (demo_ai.h is HOW it plays.)
//
// The policy lives here, shared, rather than in each frontend: two copies of
// "how long before the demo starts" is exactly the drift this codebase has
// paid for before, and the two targets must attract identically.
//
// The reference's shape, kept:
//   * the demo is an ORDINARY GAME with a synthetic pilot -- not a replay, not
//     a special mode with its own rules. Its autopilot writes the same control
//     byte a joystick would, so nothing downstream knows the difference.
//   * ANY INPUT ENDS IT IMMEDIATELY (its check_demo_end tests the key table and
//     both controllers every frame). A player who reaches for the stick is
//     playing, not watching.
//   * pause and quit are suppressed while it runs -- there is nobody to pause.
//
// What is OURS: the durations, and that the demo ends by itself. The reference
// runs its attract loop as part of a larger cabinet cycle; this is a handheld,
// so the demo shows a slice and hands the menu back.
// ============================================================================

#include <cstdint>
#include <cstdlib>

namespace ts {
namespace attract {

// ---- WHICH LEVEL THE DEMO PLAYS --------------------------------------------
// OURS, not the reference's: its setauto pins cweb/cwave to 0, so its attract
// always demos the first web. User request 2026-09-03 -- a demo that is always
// level 1 advertises one web out of a hundred, and the shapes and the colour
// bands ARE the thing worth advertising.
//
// Capped rather than uniform over all 100. The pilot targets the nearest enemy
// and nothing else (demo_ai.h), which is fine early and hopeless late; an
// attract that dies in three seconds and bounces back to the menu looks broken
// rather than hard. 50 covers FOUR of the five colour bands -- blue, red, pink
// and sky (web_palette.h, 16 levels each) -- so the variety still shows.
// One number to retune if the pilot turns out to survive deeper than expected.
constexpr int LEVEL_MAX = 50;

// Never twice in a row: with a 20 s idle between demos, a repeat reads as a bug.
// `prev` is the last level demoed, or -1 for none.
inline int pickLevel(int prev) {
    if (LEVEL_MAX <= 1) return 0;
    int lv = std::rand() % LEVEL_MAX;
    if (lv == prev) lv = (lv + 1) % LEVEL_MAX;
    return lv;
}

// Idle on the menu before the demo takes over.
constexpr int IDLE_MS = 20000;
// The "set period of time" it plays for before returning to the menu.
constexpr int RUN_MS  = 45000;
// The word DEMO eases in and out rather than snapping, so the hand-off in
// either direction reads as intentional.
constexpr int FADE_MS = 600;

struct State {
    bool active  = false;   // a demo game is running
    int  idleMs  = 0;       // time on the menu with no input
    int  runMs   = 0;       // time this demo has been running (slice clock)
    // DEMO-word fade-in clock. Unlike runMs it is NOT parked during the asset
    // hold, so the word eases in over the streaming starfield (the loading
    // screen) instead of only once the web lands -- the cue that the attract
    // mode is live. The fade-OUT stays on runMs so it still tracks real play.
    int  wordMs  = 0;

    int  lastLevel  = -1;   // so pickLevel can avoid an immediate repeat
    // The level the game was on BEFORE the demo borrowed it. A demo picks a
    // random web, and `current_level` is read by more than the web: the
    // frontier bookkeeping, and the level-synced soundtrack. Restoring it on
    // the way out means nothing downstream ever sees the demo's level.
    int  savedLevel = 0;

    void reset() { active = false; idleMs = 0; runMs = 0; wordMs = 0; }

    // Call every frame while the MENU is up. Returns true on the frame the
    // demo should start.
    bool tickMenu(int dtMs, bool anyInput) {
        if (anyInput) { idleMs = 0; return false; }
        idleMs += dtMs;
        if (idleMs < IDLE_MS) return false;
        idleMs = 0;
        active = true;
        runMs  = 0;
        wordMs = 0;
        return true;
    }

    // Call every frame while a demo game is running. Returns true on the frame
    // it should end -- either because someone touched a control or because its
    // slice is up.
    //
    // `assetsReady` is the level-asset hold's answer for the demo's level.
    // The SLICE clock (runMs) does NOT run while it is false: the run timer
    // starts when the level has fully loaded in, so a slow OG texture
    // generation does not eat the demo's play time. The DEMO-word clock
    // (wordMs) DOES run while held -- the word eases in over the streaming
    // starfield as a cue that the attract mode is live, rather than waiting
    // for the web. Input still ends the demo immediately, held or not.
    bool tickRun(int dtMs, bool anyInput, bool assetsReady = true) {
        if (!active) return false;
        if (anyInput) { reset(); return true; }
        if (dtMs < 0) dtMs = 0;       // signed wall-clock deltas: never rewind a clock
        if (dtMs > RUN_MS) dtMs = RUN_MS;     // bound the delta so the adds below can't overflow
        if (wordMs < RUN_MS) wordMs += dtMs;  // fade-in clock, bounded (no overflow)
        if (!assetsReady) return false;
        runMs += dtMs;
        if (runMs >= RUN_MS) { reset(); return true; }
        return false;
    }

    // 0..1 envelope for the DEMO word: in at the start (on wordMs, so it
    // eases in over the starfield even while the asset hold parks the slice
    // clock), out at the end (on runMs, so it tracks real play time).
    float wordFade() const {
        if (!active) return 0.0f;
        const float in  = (float)wordMs / (float)FADE_MS;
        const float out = (float)(RUN_MS - runMs) / (float)FADE_MS;
        float f = in < out ? in : out;
        return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
    }
};

} // namespace attract
} // namespace ts
