#pragma once

#include <string>
#include <vector>

#include "constants.h"
#include "models.h"
#include "fx_events.h"
#include "sfx.h"
#include "warp.h"
#include "../audio/audio_features.h"

namespace ts {

enum class GameState {
    MENU,
    LEVEL_SELECT,
    GAMEPLAY,
    HIGHSCORES,
    ENDING,
    WARP,
};

struct GameEngine {
    // Core State
    GameState state = GameState::MENU;
    int current_level = 0;
    // Start-level bonus banked by the level select. init_gameplay STAGES it
    // into pending_start_bonus and zeroes this field -- it is NEVER added to
    // the score there; see engine.cpp init_gameplay and the next field.
    // Zero for a level-0 start (LevelSelect::startBonus returns 0 for level
    // <= 0), which is what keeps the ported scoring path byte-identical
    // unless you use a feature the original never had.
    int start_bonus = 0;
    // EARNED BY CLEARING THE LEVEL, NOT BY PICKING IT. Staged out of
    // start_bonus at init_gameplay and paid in move_jump when the level is
    // actually cleared -- see the comment there.
    int pending_start_bonus = 0;
    int time = 0;

    // Fixed-timestep accumulator clock (game_step.cpp; the reference build dif_time/new_time).
    // time is the wall-clock ms render/HUD clock; dif_time carries un-stepped ms.
    int dif_time = 0;
    int last_ms = 0;
    // CHW easter-egg gate for the N-key level skip (the reference build next_level_key).
    // Default false -> N does nothing, matching a normal build.
    bool next_level_key = false;

    // Grid / Level Geometry
    std::vector<GridElement> grid;
    bool grid_level_go_round = false;
    // Lane count for the CURRENTLY LOADED level. This engine's own 50 levels are
    // always GRID_NUM_ELEMENTS (16); an imported level (e.g. a transition-reference
    // imported web) can set this to anything up to
    // GRID_MAX_ELEMENTS. change_current_level() sets this and resizes
    // grid/grid_level_pos/grid_level_normal/vertex_*/face_indices to match
    // BEFORE rebuilding lane vectors -- every loop/wrap/divisor elsewhere in
    // the engine that used to read GRID_NUM_ELEMENTS now reads this instead.
    int lane_count = GRID_NUM_ELEMENTS;

    // Vertex arrays for rendering
    std::vector<std::vector<GridVertexPos>> vertex_pos;
    std::vector<std::vector<GridVertexCol>> vertex_col;
    // NB there is no vertex_tex here. Two `vector<vector<GridVertexTex>>`
    // members used to sit in this block; nothing in any of the three trees ever
    // read or wrote them, and _resize_grid re-allocated both on EVERY level
    // load — ~81 KB and ~184 separate heap blocks of pure churn, in the worst
    // possible layout, on the target with the tightest memory. Removed
    // 2026-09-07. The tube's UVs are built where they are used
    // (grid_geometry.cpp textureLevel), not cached on the engine.
    std::vector<std::vector<int>> face_indices;

    // Grid lane positions and normals
    std::vector<Vec3> grid_level_pos;
    std::vector<Vec3> grid_level_normal;

    // Entity Counts. These are indexed by EnemyId and MUST be sized from
    // ENEMIES_NUM_IDS, not from a literal: they were `[14]` while the id space
    // was 14 wide, and the two are only coincidentally equal. Any id past the
    // literal writes off the end of three arrays at once, and the corruption
    // looks like anything but what it is.
    int shots_nums[SHOTS_NUM_IDS] = {};
    int enemies_nums[ENEMIES_NUM_IDS] = {};
    int embrios_nums[ENEMIES_NUM_IDS] = {};
    int enemies_todo[ENEMIES_NUM_IDS] = {};

    // DEPLOY-ON-JUMP latch (test directive 2026-09-26). Set when the player
    // picks up the jump powerup; consumed by the next init_embrio() that can
    // actually release the saucer, forcing that one spawn to be the UFO so it
    // is on the web while the player can still answer it. Cleared on release,
    // and cleared by init_level so it never leaks across a level or a death
    // re-entry. After the one forced release the pool is ordinary random again,
    // saucer included, subject to the live jump gate in init_embrio.
    bool spawn_ufo_next = false;

    // ---- Which enemy ROSTER is in play (docs/design/arcade_enemies.md §4) -----
    // `enemy_set` is what THIS LEVEL is running; `enemy_set_pref` is what the
    // config/menu asked for. init_level latches pref -> active, so a change
    // applies at the next level start: swapping mid-level would strand live
    // enemies carrying the other roster's state and leave enemies_todo[]
    // counting types the new set cannot spawn, so is_level_clear() would never
    // fire and the level would hang.
    //
    // config_apply additionally copies pref -> active immediately when no game
    // is in progress, so a fresh game always starts on the chosen set.
    int enemy_set      = ENEMY_SET_CLASSIC;
    int enemy_set_pref = ENEMY_SET_CLASSIC;

    // THE ONE GLOBAL ALIEN-FIRE TIMER (docs/design/arcade_enemies.md §2.5).
    // One countdown for the WHOLE arcade fleet, not one per enemy: every
    // railing enemy decrements it once per tick, so the fire rate scales with
    // the number of descenders (period / N). It lives here because it is
    // per-SET state that outlives any individual enemy, and it is cleared in
    // init_level. The policy (decrement-and-reload BEFORE the gates, so a
    // blocked attempt is LOST rather than deferred; the depth gate; the
    // concurrent-bullet cap) is ArcadeFlipper::alienFireTick, reached through
    // enemies_shared.h's arcade_alien_fire_tick from its three call sites.
    // Untouched by the classic roster, which has no fleet-wide cadence.
    int arcade_fire_timer = 0;

    // Explosions
    int explosions_nums[5] = {};
    std::vector<Explosion> explosions[5];

    // Floating Scores. MAINTAINED BY THE SIM, DRAWN BY NEITHER BACKEND.
    // init_score fills this, init_level clears it and move_scores
    // (collision.cpp) ages it, but the popups have gone unrendered on BOTH
    // targets since the GL twin (renderScores) was stripped under the
    // target-parity policy in 9eb9a36 -- the 3DS never drew them, so the PC's
    // copy was cruft by that rule.
    // DO NOT DELETE. The call site draws three rand()s unconditionally to
    // colour the popup (init_explosion) and init_score a fourth (rand() % 25),
    // so removing the system shifts the RNG stream that spawn and animation
    // rolls share, and would move the fixed-seed bit-identity harnesses
    // (tools/trig_harness.cpp, tools/demo_audit.cpp) off their baselines.
    // move_scores also holds numbered slot 330 in the ported per-frame update
    // order (game_step.cpp:85).
    std::vector<FloatingScore> scores;

    // Bonuses
    std::vector<Bonus> bonuses;

    // SFX events raised this frame; drained by the platform frontend
    // (main.cpp / main_3ds.cpp) into its audio backend. See sfx.h.
    SfxQueue sfx;

    // Player
    PlayerInfo player;

    // Explosion Light Mask (22x22 radial falloff kernel)
    float explosion_lightmask[22][22] = {};

    // Camera / World Transform
    //
    // world_trans IS the viewpoint now -- the arcade reference's `vp_x/vp_y/vp_z`
    // (game/camera.h). It is an absolute eye position in world units, resting at
    // (0, 0, STANDOFF) with the near rim on z=0, and the renderers turn it into a
    // view matrix by translating by exactly -world_trans. It is NO LONGER the old
    // 0.975/0.025 lerp target that got blended 0.6/0.4 against center_trans and
    // offset by a hardcoded -1.25 / -7.5 in each backend; that camera is gone.
    //
    // center_trans still holds the web's UN-subtracted vertex average (written by
    // grid_geometry.cpp) but the camera no longer reads it -- the arcade
    // reference's camera has no web-centre term, and the C3D backend had
    // already zeroed its weight.
    Vec3 center_trans;
    Vec3 world_trans;

    // The rest of the arcade-reference camera state (game/camera.h).
    Vec3  cam_target;              // vp_xtarg / vp_ytarg / vp_ztarg
    Vec3  cam_vel;                 // spring velocity (camera.h OMEGA)
    int   cam_view = 1;            // `view`, indexes tscam::VIEWS (1 == the
                                   // 7.125 follow view, the default framing)
    bool  cam_z_settled = false;   // `z_target` -- eye z is inside its deadzone
    // The transition reference's level-transition pair (camera.h `tstrans`): the web's z
    // offset from the play position and its velocity. DAT_0868c670/DAT_0868c674.
    float cam_web_z   = 0.0f;
    float cam_web_vel = 0.0f;
    // The scripted transition envelope for the starfield (camera.h
    // tstrans::EXIT_ENV_MAX block): 0 at rest, 1 at the level handover.
    float cam_star_env = 0.0f;
    // ---- THE LEVEL-ASSET HOLD (camera.h ASSET_HOLD_MAX_TICKS) --------------
    // Are the NEW level's procedural grid textures finished? Written once per
    // frame by the frontend from the render seam (ts_render_level_ready); the
    // arrival glide freezes while it is false so the web never slides into view
    // untextured and then pops.
    //
    // DEFAULTS TRUE, and that is load-bearing: everything that drives this
    // engine WITHOUT a renderer -- the regression harnesses, any headless
    // tool -- must take byte-for-byte the path it took before this existed.
    // "No renderer" means "nothing to wait for", never "wait forever".
    bool  level_assets_ready = true;
    // ---- THE PAUSE PRESENTATION RAMP (ui/pause_fx.h) ----------------------
    // 0 = live game, 1 = fully paused. Written once per frame by the frontend
    // (the shared pauseFxStep); the render seam reads it to lerp the pause
    // melt-o-vision and the frost menu box in and out. DEFAULTS 0, so any
    // engine driven without a frontend renders exactly as it always has --
    // the same "no renderer" law as level_assets_ready above.
    float pause_fx = 0.0f;
    // The frost box's half-width for THIS pause, measured from the widest row
    // the open menu screen can draw (ui/pause_fx.h pauseBoxHalfWidth). Written
    // by Menu::renderOverlay, read by both backends when they build the box --
    // a fixed width cannot fit "soundtrack  13_flossiesfrolic". Defaults to the
    // minimum, so an engine driven without a menu draws the box it always did.
    float pause_box_hw = 0.0f;
    // Sim ticks the arrival has been frozen for, cleared at every handover
    // (camera_snap) and ticked by camera_xform. Bounds the hold -- see
    // ASSET_HOLD_MAX_TICKS; a hold that cannot end is a hang.
    int   level_hold_ticks = 0;
    // Per-web lift off the plane of a NEAR-PLANAR web, recomputed once per level
    // by camera_snap. ZERO for anything round enough, so reference-exact webs keep the
    // reference camera exactly; non-zero only for the flat shapes the reference web set never
    // contained. See camera.cpp -- without it "bowling alley" is a single line.
    float cam_plane_bias_x = 0.0f;
    float cam_plane_bias_y = 0.0f;

    // Storage for the world modelview/projection/viewport, mirroring the reference build's
    // world_model / world_proj / world_view globals.
    // Column-major 4x4 (GL/glm layout). Read by project_point, which
    // init_explosion uses to place the floating score at the kill point.
    //
    // NOTHING CAPTURES THEM TODAY. set_world_matrices is their only writer and
    // it has no call site in any tree; no translate_world exists here either,
    // and both backends build their view matrix locally (c3d/04_setup.inc
    // buildView, renderer_vk.cpp viewMatrix) and never hand it back. So these
    // hold the identity / 1x1-viewport initialisers below for the whole run,
    // and project_point does not really project: with an identity MVP it
    // collapses to winx = (ox+1)*0.5, winy = (oy+1)*0.5 -- no depth term -- and
    // it cannot fail, since clip.w is always 1. That flat remap IS the shipped
    // score placement, so wiring the capture up would move every popup; it is
    // the Phase F work game_step.cpp's reference-slot-329 note describes.
    double world_model[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    double world_proj[16]  = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    int    world_view[4]   = {0, 0, 1, 1};

    // Tremor Effect
    float base_tremor_strength = 0.02f;
    float tremor_strength = 0.02f;

    // Screen Flash (the the reference build's web-wave phase term on pickup -- despite the
    // name it is NOT a screen flash; see grid_geometry.cpp Phase 2).
    int screen_flash = 0;
    int max_screen_flash = 300;

    // THE screen flash (fx_events.h): raised at the shared trigger points,
    // decayed per sim tick, drawn by each backend in its own way.
    FlashState flash;

    // Zapper Targeting
    Vec3 old_zapper_target;
    Vec3 zapper_target;
    int old_zapper_el_pos = -1;
    int old_zapper_num = -1;
    bool zapper_target_found = false;

    // ---- SUPER ZAPPER CHAIN (user request 2026-09-16) ---------------------
    // The super zapper clears the WHOLE SCREEN, but serially -- one bolt per
    // beat, never all at once. It fires at the nearest zappable enemy and KILLS
    // it outright, then re-scans for the next nearest and arcs a fresh bolt to
    // it, repeating until nothing zappable remains. This POD carries the live
    // state: a sliding window of struck points the renderer strokes as a comet
    // tail, plus the current target reference. Fixed-size arrays, no allocation,
    // and one add + one compare per tick -- it fits the existing game_step tick.
    //
    // The target is a (lane, id, captured-z) reference, NOT an index: a target
    // killed by another source between aim and impact resolves to "nothing" and
    // is skipped rather than stranding the bolt on a swap-down-shifted slot. The
    // kill itself is applied by weapons.cpp move_zapper through the SAME
    // init_explosion / arcade_enemy_die / counter plumbing the continuous beam
    // used, so scoring, corpses and the capsule-cadence exclusion are unchanged.
    struct ZapperChain {
        // Sliding window of struck points kept for the comet tail. The tail only
        // shows links younger than the renderer's BOLT_FADE (~8 ticks) and a beat
        // is ~9 ticks, so at most ~2 links are ever on screen at once; WINDOW=8
        // is generous headroom and bounds the struct with no allocation.
        static constexpr int WINDOW = 8;

        // Sliding window of struck points in DRAW space (z negated): index 0 is
        // the oldest kept point (the claw at fire, until the cascade outruns it),
        // index pt_count-1 the newest. The renderer strokes consecutive links and
        // fades each by the age of its newer end.
        float pts[WINDOW][3] = {};
        int   pt_tick[WINDOW] = {};
        int   pt_count = 0;

        // The in-flight leading edge (draw space). TRAVEL homes it to the current
        // target; DWELL/FADE park it on the last struck point.
        float front[3] = {};

        // The current bolt target, as a live (lane, id, captured-z) reference so
        // a swap-down from an unrelated kill cannot strand the bolt on the wrong
        // enemy. Re-scanned to the nearest remaining zappable enemy after each
        // kill, so the cascade continues until the screen is clear.
        int   cur_lane = -1;
        int   cur_id   = 0;
        float cur_z    = 0.0f;
        bool  have_cur = false;

        // State machine: 1 = TRAVEL (front homing to the current target),
        // 2 = IMPACT dwell (the bolt sits on the struck target), 3 = FADE tail.
        int   phase = 0;
        int   timer = 0;   // ticks spent in the current phase
        bool  active = false;
    };
    ZapperChain zapper_chain;

    // ---- AI Droid companion (the arcade reference: makedroid / rundroid) ---
    // ONE tumbling cube that walks the rim hunting the nearest enemy's lane and
    // fires on a fixed cadence. Position is carried in world x/y and integrated
    // during a glide, exactly as the arcade reference does ([si+20]/[si+24] added to [si+04]/
    // [si+08]) -- the lane index alone cannot express a droid mid-step.
    bool  ai_droid = false;
    // The arcade reference's `dnt`, a THREE-state flag, not a bool:
    //    0 = idle, the droid's powerup slot is live
    //    1 = ARMED -- a powerup was taken during the climb-out (:5334); the
    //        next pickup grants the droid outright, skipping the slot ladder
    //   -1 = already granted, so the ladder's droid slot is skipped and the
    //        pickup that WOULD have landed there lands on the warp slot instead
    //        (init_powerup). init_level clears -1 back to 0 every level, so the
    //        droid is re-earned per level exactly as the reference does at its
    //        own web setup -- state 1 (ARMED) deliberately survives both a level
    //        change and a death, and is the only powerup state that does.
    // This is what makes "the first pickup on some levels summons the droid"
    // true, and it is the mechanism this engine ported as ai_next_level.
    int   ai_droid_state = 0;
    int   ai_lane = 0;          // committed lane ([si+16])
    int   ai_next_lane = 0;     // destination during a glide ([si+48])
    int   ai_move = 0;          // glide countdown, 16..0 ([si+44]); 0 = parked
    int   ai_fire = 0;          // shot cadence countdown ([droidel])
    float ai_x = 0.0f, ai_y = 0.0f;            // current, interpolated
    float ai_x0 = 0.0f, ai_y0 = 0.0f;          // glide start
    float ai_x1 = 0.0f, ai_y1 = 0.0f;          // glide destination

    // Powerup Text Display
    std::string powerup_text[2];
    int powerup_text_starttime[2] = {};

    // Pixel-shatter celebration text (presentation-only; the renderers own the
    // particle simulation — see rendering/shatter.h). Celebratory popup ids
    // (constants.h isShatterCelebration) and the 1-up route here instead of the
    // wobble popup above. Events are never cleared: the renderers treat an
    // event older than its duration as idle, so there is no renderer-writes-
    // engine-state coupling like powerup_text's clear-after-4s.
    struct ShatterEvent {
        char text[20] = {};   // empty = never triggered
        int  starttime = 0;
        int  kind      = 0;   // shatter::STYLE_* (constants.h shatterStyleFor)
    };
    ShatterEvent shatter_events[2];

    // Warp. The round's own end flag lives on WarpState (warp.warp_level_end) --
    // a duplicate `warp_level_end` sat here for a long time with no reader and
    // no writer, shadowing the live one; it is gone.
    bool warp_reached = false;
    WarpState warp;
    // Per-round-type play counters for the bonus rounds (warp.h WARP_ROUND_*):
    // count & 7 picks one of the 8 deterministic courses, the raw count scales
    // base speed each recurrence. Lives here (not on WarpState) because it
    // must PERSIST across rounds within a run; reset with the run in
    // init_gameplay — deliberately NOT in reinit_gameplay, which runs after
    // every warp and would zero the difficulty ramp it exists to carry.
    int warp_plays[WARP_ROUND_COUNT] = {};

    // Level Transition
    int nolives_animation = 0;
    int control_ani = 0;

    // ---- "Yes!" climb-out glissando (level exit) ---------------------------
    // The arcade reference does NOT schedule a sequence of retriggers here. It plays the
    // LOOPING 'yes' sample (sample-table idx 23: repeat length 23146 of 23148,
    // and it contains several utterances) and then bends that live voice's pitch
    // upward once per frame -- the arcade reference `yeson` stores the two voice handles, and
    // `czoom1`/`czoom2` call `zoomup` every frame, which CHANGEFXs pitch +1.
    //
    // Raising the pitch SHORTENS the loop, so the utterances speed up on their
    // own. The acceleration is emergent, not authored. An earlier attempt here
    // scheduled 5 discrete pushes with halving gaps; that could never sound
    // right because it modelled the wrong mechanism.
    bool  yes_loop_active = false;
    float yes_period = 512.0f;   // the arcade reference `yespitch`, seeded to the sample's own $200
    int   yes_release = 0;       // >0 = tapering out; ticks remaining

    // ---- LOOPING-VOICE OWNERSHIP (defect fix, user report 2026-08-20) ------
    // "when the announcer says YES YES YES ... it continues to play that
    // buffer", and the same for the superzapper beam.
    //
    // A looping voice can only be ended by an explicit event, and every emitter
    // of that event lived inside a per-tick system that runs ONLY in
    // GameState::GAMEPLAY. Cross a state seam and the voice is left ringing.
    //
    // These are TWO mechanisms wearing one pattern, and the distinction decides
    // the fix -- a single teardown site closes one and not the other:
    //   * YES  -- its stop predicate is PINNED FALSE. The warp branch sets
    //     out_animation = 249, move_jump bumps it to 250, the branch pins 249
    //     again: it oscillates forever, so `out_animation == 0` never holds.
    //     Engine state SURVIVES re-init, which is why it self-heals on return
    //     from the bonus round -- measured as a bounded 604-tick (9.66 s)
    //     unattended drone, not a permanent orphan.
    //   * THUNDER -- its trigger variable is DESTROYED. init_level writes
    //     animation_zapp = 0 and move_zapper's own guard then makes the whole
    //     function a no-op, so nothing can ever emit its stop. The engine kept
    //     NO record that the voice was live; that state existed only inside the
    //     audio backend, which is the actual hole this field closes.
    // Their seam sets are DISJOINT, so both are handled, at their own seams.
    //
    // NB the desktop oracle UNDERSTATES the YES case: LOOP_PITCH is a no-op on
    // SDL_mixer, so desktop drones at 1.0x while hardware drones at ~2.5x the
    // base rate (the glissando leaves the period at ~202 of 512). Do not judge
    // this defect fixed from a desktop capture alone.
    bool  zapper_loop_active = false;   // THUNDER beam voice is live

    // Push whatever stops the currently-live looping voices need, and forget
    // them. Call at every seam that leaves GAMEPLAY without returning to it on
    // the next tick -- warp entry, game over, quit to menu. Safe to call when
    // nothing is looping.
    //
    // YES is RELEASED rather than hard-stopped: LOOP_RELEASE is self-
    // terminating on both backends (3DS re-queues the remainder non-looping,
    // desktop fades), so it cannot orphan, and it keeps the deliberate
    // finish-the-current-pass behaviour instead of chopping mid-word.
    void stop_looping_sfx();

    // ---- Kill live shatter signs at a seam that leaves GAMEPLAY ----------
    // The YES chant's signs (rendering/shatter.h STYLE_YES) live up to
    // YES_MS = 4200 ms and are keyed off shatter_events[] + yes_beat_ms[].
    // During GameState::WARP the world-space path suppresses every non-empty
    // event (warpOwnsEvent is all-non-empty by the seventh verdict), and the
    // warp's own burst mirrors the SAME engine events -- so a chant still
    // live at warp entry keeps detonating inside the bonus round. The audio
    // seam (stop_looping_sfx) tears the VOICE down; this tears the SIGNS
    // down on the same tick, so the YESes die at the flash that takes the
    // player to the round. Idempotent.
    void clear_shatter_events();

    // ---- Drop the gameplay entities once the web has pulled away ----------
    // The game-over sequence fades the web and everything standing on it out
    // through the render gate (goWebFadeNow), but the engine's entity DATA --
    // the per-lane enemy/shot/embryo vectors, the explosion/bonus/score lists
    // and their counts -- otherwise survives the whole sequence and the
    // high-score screen. The user's contract is that once the web is gone, the
    // level's objects are gone from MEMORY, not merely hidden. This clears the
    // gameplay entities and their counts WITHOUT touching the web/grid surface
    // geometry (the recede+fade owns that) or the player state (the dive is
    // still running). Idempotent: clearing an already-empty lane is a no-op,
    // and clear() keeps the reserved capacity so a later init_level's reserve
    // is a no-op rather than a realloc.
    void clear_gameplay_entities();

    // One-shot latch so the game-over step clears the entities exactly once,
    // on the tick the web finishes pulling away. Reset by init_level.
    bool gameover_entities_cleared = false;

    // ---- Chant sync: when the voice actually SAYS "yes" -------------------
    // The sample is ONE sustained utterance per loop pass (23148 samples @
    // 19226.1 Hz = 1.204 s, attack measured at loop position 0.218), so every
    // loop pass is one audible "YES!" -- and because the glissando above keeps
    // shortening the loop, they arrive faster and faster. game_step integrates
    // the playback phase and stamps each attack here; the YES! shatter signs
    // (rendering/shatter.h STYLE_YES) appear on these timestamps, so a sign
    // pops exactly when you hear the word. RENDER-ONLY consumers.
    static constexpr int YES_BEAT_MAX = 6;
    int   yes_beat_ms[YES_BEAT_MAX] = {};
    int   yes_beat_count = 0;
    float yes_play_phase = 0.0f;   // loop passes elapsed since LOOP_START

    // Background Color
    float bg_color[4] = {0.225f, 0.225f, 0.3375f, 1.0f};

    // Rendering options, copied from GameConfig by applyConfigToEngine -- at
    // boot from each frontend, and again on every menu change via
    // Menu::applyLive. READ BY BOTH BACKENDS (c3d/08_frame.inc, vk_scene.cpp).
    // Translucent web so enemies behind the tube are visible — see DOCTRINE.md
    // "Intentional deviations". web_alpha only applies when web_transparent.
    bool  web_transparent = true;
    float web_alpha       = 0.5f;
    float web_brightness  = 1.0f;   // overall web brightness scale (0..2)
    float web_tex_bright  = 1.0f;   // extra scale on the additive texture glow
    // Per-colour-band brightness multiplier for the CURRENT level, latched at
    // change_current_level from levels().band_additive[webColorBandIndex(level)].
    // Multiplies BOTH the web surface and the texture glow so the high-luminance
    // sky/jade bands stop washing out the additive enemies on them. 1.0 = no
    // change. See WebSet::band_additive (data/webs.h) for the why.
    float web_band_additive = 1.0f;
    bool  invert_move     = false;  // false = corrected horizontal movement
    // 0 = off, non-zero = on (the additive vector-glow wire). The old
    // 1=cheap-TEV / 2=bloom split is retired; both backends test `!= 0`.
    int   web_glow        = 1;
    float web_glow_k      = 1.0f;   // Glow Strength (web_glow_pct/100); 100% = tuned vector-glow look
    float fov_deg         = 53.13f; // View: vertical FOV in degrees. The arcade reference
                                    // frustum (tscam::FOV_Y_DEG). Was 60. reference binary is 45, but the
                                    // reference-exact camY bias (1.25, needed so FLAT levels are
                                    // not seen edge-on) then clips round levels; 60 frames
                                    // both. Runtime/configurable, transition-reference style.
    // Line-entity brightness scale (shots/explosions), 0..1; also drives distance
    // dimming so bright white shots recede like the fogged web instead of floating.
    float shot_intensity  = 0.65f;
    // Pickup burst style (constants.h PICKUP_BURST_*, rendering/burst_styles.h):
    // which drawing the powerup-collected explosion gets. Render-only; read by
    // the shared line builder and, for the one style that ripples the web, by
    // the shared grid wave. From GameConfig::pickup_burst via config_apply.
    int   pickup_burst    = PICKUP_BURST_ARCADE;
    // A/B toggle for the pickup LOD (constants.h PICKUP_DETAIL_*), from
    // GameConfig::pickup_detail via config_apply. Read by line_geometry's
    // capsule emit; FULL pins the authored two passes.
    int   pickup_detail   = PICKUP_DETAIL_AUTO;
    // Write the 3DS perf log (GameConfig::perf_log). A diagnostic, mirrored
    // here only because it is how every other config value reaches the
    // renderer -- no new seam symbol for a debug switch.
    bool  perf_log        = false;
    // Which processor expands line segments (GameConfig::seg_expand;
    // constants.h SEG_EXPAND_*).
    int   seg_expand      = 0;   // SEG_EXPAND_AUTO
    // Diagnostic logging (phase heartbeat + seg boundary recorder). Separate
    // from perf_log on purpose -- see GameConfig::diag_log.
    bool  diag_log        = false;
    bool  gameover_test   = false;   // GameConfig::gameover_test
    // ATTRACT MODE. True while a demo game is being played by the synthetic
    // pilot (game/demo_ai.h). The SIM does not read this -- a demo is an
    // ordinary game, which is the whole point -- it exists so the frontends can
    // route input from the pilot and the backends can draw the DEMO overlay.
    bool  demo_mode       = false;
    // 0..1 envelope for the DEMO word, owned by the frontends' attract state
    // (ui/attract.h wordFade). It lives on the engine only so BOTH backends can
    // read it from the same place -- the alternative is each backend keeping
    // its own copy of the timing, which is the drift this codebase warns about.
    float demo_word_fade  = 0.0f;
    // TEST INSTRUMENT (config burst_test / desktop T2K_BURST_TEST): re-fire the
    // pickup burst at the claw on a clock so a style can be looked at without
    // farming a capsule (game_step.cpp). Never on in a real run.
    bool  burst_test      = false;
    int   burst_test_tick = 0;

    // ---- Audio-reactive visuals (see docs/AUDIO_REACTIVE_SPEC.md) -----------
    // Refreshed once per frame by BOTH frontends -- t2k_pc/src/main.cpp and
    // t2k_3ds/src/platform_3ds/main_3ds.cpp each call audiofx::analysis_sample
    // -- sampled at the PLAYHEAD (not at analysis time — the wave queue is
    // seconds deep). All-zero is the legal, expected state whenever music is
    // off or unavailable, and every consumer must degrade to "calm" rather
    // than freeze.
    //
    // NOTE these are RENDER-ONLY. Nothing here may feed game logic, collision or
    // the ported tremor ability (weapons.cpp move_tremor / ACT_TREMOR).
    AudioFeatures audio;
    // Depth for the web colour-duck (the shared law, rendering/web_palette.h
    // webAudioBrightness -- the 3DS reaches it through the audioWebBrightness
    // adapter in c3d/07_perf.inc, the PC calls it directly from vk_scene.cpp)
    // AND the starfield's beat response (3DS c3d/05_stars.inc build_Stars, PC
    // vk_scene.cpp) — one dial for both, since both are "how hard does the
    // audio hit" rather than independently tuned.
    float audio_pulse_k   = 1.0f;   // (audio_pulse_pct/100)

    // ---- SPIKE: geometry-indexed web colour cycling -------------------------
    // Evaluating whether an arcade-style cycling palette, indexed by a GEOMETRIC
    // term rather than by texel, reads well on the tube. Toggle so it can be
    // A/B'd live on hardware. If it looks good the real version moves to the
    // PICA fragment-lighting unit's RR/RG/RB reflection LUTs (per-pixel, and it
    // costs no TEV stages -- only stage 5 is free). This CPU version is the
    // cheap look-test: at 80x56 = 4480 grid verts a smooth gradient is nearly
    // indistinguishable from per-pixel, so it answers the aesthetic question
    // without the fragment-lighting unit's documented silent-failure chain.
    bool  web_color_cycle = false;
    float web_cycle_k     = 1.0f;   // 0..2 blend strength
    bool  audio_safe_mode = false;  // photosensitivity clamp — safety, not taste
    // Bonus-round VLM feedback chamber kill-switch (WARP rounds ONLY — never
    // normal gameplay rendering; docs/design/bonus_rounds.md). Surfaced as the
    // SD-config key "warp_feedback" (save_load.h, routed via config_apply) —
    // rail_c3d_twin.md D9: on C3D the chamber is a render-to-texture loop, the
    // one PICA200 configuration class known to be able to wedge the GPU, so the
    // shipped build MUST be recoverable by editing a JSON file rather than by a
    // reflash. false => both backends draw the round's full geometry with zero
    // chamber passes.
    bool  warp_feedback   = true;

    // Game time
    int game_time = 0;

    // Constructor
    GameEngine();

    // Initialization methods
    void _init_grid();
    void _fill_explosion_lightmask();
    void _fill_grid_level_face();
    // Resizes grid/grid_level_pos/grid_level_normal/vertex_*/face_indices for
    // an n-lane level and sets lane_count = n. Called once from _init_grid()
    // (n = GRID_NUM_ELEMENTS) and again from change_current_level() whenever
    // the incoming level's lane count differs from the current one. A no-op
    // cost-wise when n is unchanged (std::vector::resize to the same size
    // does not reallocate) -- this engine's own 50 levels never trigger the
    // reallocation path.
    void _resize_grid(int n);

    // Level management
    void init_gameplay(int time);
    void reinit_gameplay(int time);
    void change_current_level(int time, int nr);
    // Sets bg_color. Always pure black: the background is the starfield, and a
    // flat grey backdrop only ever made sense as a canvas for the mandelbrot to
    // tint. Kept as a function (rather than a constant) because it is called at
    // level start and is the natural seam if a level-tinted backdrop returns.
    void apply_background_color();
    void _compute_grid_positions();
    void init_level(int time, int nr);
    void exit_level(int time);

    // Entity spawning
    void init_enemy(int lane, int enemy_id);
    void init_shot(int lane, float z, int shot_id);
    void init_embrio();
    // shed_bonus=false suppresses the bonus-particle shed for an EXPLOSION_ENEMY
    // record. Used by the classic shot-kill SWEEP sites so a classic kill sheds
    // its single bonus batch at _handle_death (the same single point an arcade
    // kill uses) instead of shedding twice. See init_explosion.
    //
    // draw_visual=false pays the kill score (and its sfx) but creates NO
    // explosion record -- the ball is suppressed, the score is kept. Used by
    // the classic shot-kill SWEEP sites together with shed_bonus=false so a
    // classic kill draws ONE ball (at _handle_death) while still paying the
    // faithful 2x kill score (sweep pay + death pay): half the explosion
    // particles, same score. See init_explosion.
    // spike_height: the EXPLOSION_SPIKE's spike height (engine z-units) BEFORE
    // this hit eroded it. Drives the "tink" pitch to mirror the reference's
    // decspike period override (sfx_pitch = (length & 0x1FF) + 524), which the
    // bank's table period (254 -> 14092.7 Hz) does NOT encode: the tink actually
    // plays at ~6.5-6.8 kHz and ramps UP as the spike is shot down. The pitch
    // is computed here (per-explosion, not in a hot loop) as
    // 254 / ((round(spike_height / (GRID_ELEMENT_LENGTH/160)) & 0x1FF) + 524).
    // Ignored for every other ex_id. Default 0.0 -> pitch 1.0 for non-spike sites.
    void init_explosion(int time, int energy, int lane, float z, int ex_id, int ex_id2 = 0,
                       bool shed_bonus = true, bool draw_visual = true,
                       float spike_height = 0.0f);
    // Builds and pushes the visible explosion record (the ball) only -- no score,
    // no sfx, no bonus shed. Extracted from init_explosion for readability; the
    // only caller is init_explosion, with the 300 energy clamp already applied.
    // Do NOT call directly for a killed enemy -- route through _handle_death so
    // the score, shed, and cadence run too. See init_explosion.
    void emit_explosion_record(int energy, int lane, float z, int ex_id, int ex_id2);
    void init_bonus(int lane, float z);
    void init_1up(int time);
    // Add `points` to the player's score and cross the 50000-point 1-up
    // threshold if this crossed it (init_1up). The same
    // prev=score/50000; score+=points; if(score/50000>prev) init_1up(time)
    // shape repeated at every scoring site in the game -- see engine.cpp,
    // enemies.cpp, collision.cpp, warp.cpp. `points` must be side-effect-free
    // (evaluated exactly once, before the threshold check).
    void award_score(int time, int points);
    void init_score(int points, float x, float y, float r, float g, float b);
    void init_gameover(int time, int text_id);

    // Powerup system
    void init_powerup(int time);

    // One qualifying kill against the capsule schedule (constants.h
    // POWERUP_KILL_BANDS). Returns true when this kill drops a capsule.
    // ONE rule, one place: EVERY qualifying kill site routes through here --
    // classic and arcade alike -- so the cadence cannot drift between them.
    // Superzapper kills must NOT call it, and neither does the pulsar's rim
    // split, which is not a death (arcade_pulsar.cpp).
    bool note_powerup_kill();
    void _award_powerup_score(int time, int base_points, int num_popups);
    void _trigger_fake_zapper();

    // Store the world modelview/projection/viewport for project_point below.
    // glm/GL types never cross the seam — plain double[16]/int[4], matching
    // the reference build's globals.
    //
    // NO CALLER TODAY, on either target: the reference build's translate_world (tick slot
    // 329) was never ported, so nothing ever captures the MVP, and --gc-sections
    // discards this from the 3DS binary. Kept as the seam Phase F item 25 needs
    // (docs/port-audit/gameplay-fidelity-plan.md, C-12); see game_step.cpp's
    // slot-329 note. WIRING IT UP MOVES EVERY FLOATING SCORE POPUP -- read the
    // note on world_model above first.
    void set_world_matrices(const double* model, const double* proj, const int* view);

    // gluProject-equivalent (the original InitExplosion). Projects an object-space
    // point through the captured world_model/proj/view into window coords.
    // Returns false if the point is behind the eye (w==0). Mirrors GLU's math.
    bool project_point(double ox, double oy, double oz,
                       double& winx, double& winy, double& winz) const;

    // Utility
    void show_powerup_text(int time, int text_id);
    // Raise a pixel-shatter celebration (kind = shatter::KIND_*). Stamps the
    // engine's own clock, deliberately ignoring callers' popup backdating —
    // the shatter has a real converge phase instead of a slow fade-in.
    void trigger_shatter(int kind, const char* text);
    bool is_level_clear();
};

} // namespace ts
