#pragma once

// ============================================================================
// input_frame.h — backend-neutral per-frame input, shared by desktop + 3DS.
//
// The game logic must never see SDL or libctru. Each platform's entry point
// fills an InputFrame (held level-state + pressed edges) and hands it to the
// shared game_step; the bit layout mirrors the reference build's keyb[]/joyp1 reads
//. No platform headers here.
// ============================================================================

#include <cstdint>

namespace ts {

// Action bits. Movement/shoot/jump/tremor/zapper are sampled as LEVEL state
// every tick (the reference build reads keyb[]/joy each tick); the rest are edge actions.
enum ActionBit : uint32_t {
    ACT_MOVE_LEFT  = 1u << 0,
    ACT_MOVE_RIGHT = 1u << 1,
    ACT_SHOOT      = 1u << 2,  // the reference build joy b1 / key A
    ACT_JUMP       = 1u << 3,  // the reference build joy b3 / key S
    ACT_TREMOR     = 1u << 4,  // the reference build joy b2 / key W
    ACT_ZAPPER     = 1u << 5,  // the reference build joy b4 / key Q
    ACT_NEXT_LEVEL = 1u << 6,  // key N, gated by engine.next_level_key (CHW easter egg)
    ACT_PAUSE      = 1u << 7,  // P or ESC (toggle)
    ACT_END_YES    = 1u << 8,  // Y — end game while paused
    ACT_CONFIRM    = 1u << 9,
    ACT_CANCEL     = 1u << 10,
    ACT_QUIT       = 1u << 11,
    ACT_ANY        = 1u << 12, // any key/button held — for "press any button" gates
};

struct InputFrame {
    uint32_t held    = 0;  // level state this frame
    uint32_t pressed = 0;  // edge: newly-down this frame (self-clearing per tick)
};

// ---------------------------------------------------------------------------
// THE ONE PLACE PHYSICAL LEFT/RIGHT BECOMES A MOVE ACTION.
//
// The raw mapping is CROSSED: a physical LEFT press raises ACT_MOVE_RIGHT. That
// is not a typo -- it is a correction found on hardware ("horizontal movement
// was inverted, right stick -> claw left") and it lived, for a long time, only
// inside platform_3ds/main_3ds.cpp. src/main.cpp did the straight identity
// mapping, so THE TWO TARGETS STEERED THE CLAW OPPOSITE WAYS from the same
// press -- caught by the GL/C3D parity audit 2026-08-21.
//
// It is shared rather than copied into the second frontend on purpose: a
// workaround duplicated across two files is precisely the drift pattern
// DOCTRINE.md warns about, and this one had already produced a gameplay-level
// divergence. Both entry points now call this and neither owns the convention.
//
// `invert` is engine.invert_move (GameConfig::controls.invert_move), which
// selects the RAW mapping for players who prefer it. It reaches the engine on
// both entry points via config_apply, but before this helper only the 3DS
// actually read it -- so the desktop's invert-movement setting was a dead knob.
inline void inputSetMove(InputFrame& f, bool physLeft, bool physRight, bool invert) {
    const bool moveLeft  = invert ? physLeft  : physRight;
    const bool moveRight = invert ? physRight : physLeft;
    if (moveLeft)  f.held |= ACT_MOVE_LEFT;
    if (moveRight) f.held |= ACT_MOVE_RIGHT;
}

} // namespace ts
