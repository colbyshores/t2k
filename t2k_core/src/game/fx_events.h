#pragma once

// ============================================================================
// fx_events.h — the SCREEN FLASH envelope, shared by every target.
//
// The arcade reference punctuates its big moments with a full-field white
// flash. This port raises that flash from the SAME gameplay sites on every
// platform (the trigger points and their timing are invariants of the
// Aesthetic Contract in DOCTRINE.md); only how the flash is DRAWN differs:
//   * PC: the composite pass adds the flash through the bloom pipeline.
//   * 3DS: drawScreenFx (t2k_3ds/src/rendering/c3d/06_builders.inc) draws one
//     untextured single-TEV additive ortho fullscreen quad per eye at
//     FLASH_GAIN 0.85, only while env > 0.02 -- a quiet frame pays nothing, a
//     flash frame ~0.1-0.3 ms. A flat quad cannot be bloom-shaped: that is the
//     documented 3DS divergence (DOCTRINE.md, Phase 2.5); timing and envelope
//     are identical on both targets.
//   * VR: the PC backend attenuates and shortens it (comfort).
//
// ONE scalar envelope, decayed once per SIM TICK (game_tick / move_warp's
// step), never per rendered frame, so a 30 fps 3DS and a 144 Hz PC see the
// same curve. A raise never dims a brighter flash already in flight; the
// kind of the strongest recent event wins.
//
// "Hue is identity, intensity is event": the flash is white light. Death is
// the one event that is not a celebration, and it flashes dimmer, not redder.
// ============================================================================

#include <stdint.h>

namespace ts {

enum FlashKind : uint8_t {
    FLASH_NONE = 0,
    FLASH_POWERUP,       // capsule collected on the rim
    FLASH_WARP_TOKEN,    // powerup ladder slot 5: the token
    FLASH_ONE_UP,        // extra life
    FLASH_SUPERZAP,      // superzapper fired (stock or free)
    FLASH_LEVEL_CLEAR,   // exit_level: the dive begins
    FLASH_HANDOVER,      // the level advance at the end of the dive
    FLASH_DEATH,         // init_gameover
    FLASH_BONUS_BEGIN,   // bonus round starts (init_warp)
    FLASH_BONUS_END,     // bonus round won / failed
    FLASH_BONUS_CATCH,   // a gate caught
    FLASH_KIND_COUNT
};

// Per-kind intensity (0..1 at the raise) and per-tick decay multiplier at
// the 60 Hz sim tick. The tail length in ticks is ln(0.02)/ln(decay):
//   0.88 -> 31 ticks (0.5 s), 0.92 -> 47 (0.8 s), 0.95 -> 76 (1.3 s).
struct FlashParams { float intensity; float decay; };

inline FlashParams flashParams(FlashKind k) {
    switch (k) {
        case FLASH_POWERUP:     return {0.35f, 0.93f};
        case FLASH_WARP_TOKEN:  return {0.60f, 0.94f};
        case FLASH_ONE_UP:      return {0.80f, 0.95f};
        case FLASH_SUPERZAP:    return {0.70f, 0.90f};
        case FLASH_LEVEL_CLEAR: return {0.40f, 0.97f};
        case FLASH_HANDOVER:    return {0.90f, 0.93f};
        case FLASH_DEATH:       return {0.45f, 0.88f};
        case FLASH_BONUS_BEGIN: return {0.60f, 0.95f};
        case FLASH_BONUS_END:   return {1.00f, 0.965f};
        case FLASH_BONUS_CATCH: return {0.30f, 0.90f};
        default:                return {0.00f, 0.90f};
    }
}

struct FlashState {
    float   env      = 0.0f;   // 0..1, the drawn intensity
    float   decay    = 0.90f;  // per-tick multiplier of the flash in flight
    uint8_t kind     = FLASH_NONE;
    int     start_ms = 0;      // engine.time at the raise (renderers may ramp on it)
};

// Raise a flash. `scale` lets a site modulate the table value (a 750-point
// catch flashes harder than a 250). Never dims an in-flight flash.
inline void flash_raise(FlashState& f, FlashKind k, int time_ms, float scale = 1.0f) {
    const FlashParams p = flashParams(k);
    const float v = p.intensity * scale;
    if (v > f.env) {
        f.env = v > 1.0f ? 1.0f : v;
        f.decay = p.decay;
        f.kind = k;
        f.start_ms = time_ms;
    }
}

// One sim tick of decay.
inline void flash_tick(FlashState& f) {
    if (f.env <= 0.0f) return;
    f.env *= f.decay;
    if (f.env < 0.002f) { f.env = 0.0f; f.kind = FLASH_NONE; }
}

// Presentation-side helpers, so every backend maps the envelope the same way.
// `safe` = photosensitive_safe (depth clamp; the rate is already gentle).
inline float flash_display(const FlashState& f, bool safe) {
    return safe ? f.env * 0.4f : f.env;
}
// VR comfort: attenuated and shortened (a squared envelope decays faster).
inline float flash_display_vr(const FlashState& f, bool safe) {
    const float e = flash_display(f, safe);
    return e * e * 0.35f;
}

} // namespace ts
