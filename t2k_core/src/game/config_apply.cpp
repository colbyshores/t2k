#include "config_apply.h"

namespace ts {

void applyConfigToEngine(const GameConfig& config, GameEngine& engine) {
    engine.web_transparent = config.web_transparent;
    engine.web_alpha       = config.web_alpha_pct * 0.01f;
    engine.shot_intensity  = config.shot_intensity * 0.01f;
    engine.web_glow        = config.web_glow;
    engine.web_glow_k      = config.web_glow_pct * 0.01f;
    engine.fov_deg         = static_cast<float>(config.fov_deg);
    engine.web_brightness  = config.web_brightness_pct * 0.01f;
    engine.web_tex_bright  = config.web_texture_brightness_pct * 0.01f;
    engine.invert_move     = config.controls.invert_move;
    engine.audio_pulse_k   = config.audio_pulse_pct * 0.01f;
    engine.audio_safe_mode = config.photosensitive_safe;
    engine.web_color_cycle = config.web_color_cycle;
    engine.web_cycle_k     = config.web_cycle_pct * 0.01f;
    // The bonus-round feedback chamber's runtime kill-switch (save_load.h
    // warp_feedback / rail_c3d_twin.md D9). Routed here rather than read at the
    // renderer so BOTH backends and BOTH platforms pick it up through the one
    // boot copy every other rendering knob uses.
    engine.warp_feedback   = config.warp_feedback;
    // Pickup burst style (constants.h PICKUP_BURST_*). Live from the options
    // menu like every other knob here; the builder reads it per frame.
    engine.pickup_burst    = (config.pickup_burst >= 0 && config.pickup_burst < PICKUP_BURST_COUNT)
                           ? config.pickup_burst : PICKUP_BURST_CLASSIC;
    engine.pickup_detail   = (config.pickup_detail >= 0 && config.pickup_detail < PICKUP_DETAIL_COUNT)
                           ? config.pickup_detail : PICKUP_DETAIL_AUTO;
    engine.perf_log        = config.perf_log;
    engine.seg_expand      = config.seg_expand;
    engine.diag_log        = config.diag_log;
    engine.gameover_test   = config.gameover_test;
    engine.burst_test      = config.burst_test;

    // ---- Enemy roster (docs/design/arcade_enemies.md §4.2/§4.4) ---------------
    // The PREFERENCE always lands; init_level latches it into engine.enemy_set
    // at the next level start, because swapping mid-level would strand live
    // enemies and hang is_level_clear().
    engine.enemy_set_pref = config.enemy_set;
    // ...but with no game in progress there is nothing to strand, so apply it
    // NOW. That is what keeps this from reading as a dead knob on the boot
    // menu: pick Arcade, start a game, and level 1 is already the chosen set
    // rather than the one before it.
    if (engine.state != GameState::GAMEPLAY && engine.state != GameState::WARP)
        engine.enemy_set = config.enemy_set;
}

} // namespace ts
