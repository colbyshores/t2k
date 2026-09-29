#include "player.h"
#include "engine.h"
#include "camera.h"     // camera_arrival_held -- the level-asset hold predicate
#include "constants.h"
#include "models.h"
#include "../rendering/gameover_geometry.h"   // gameoverRamp / gameoverWebT -- the web pull-away gate

#include <cmath>
#include <cstdlib>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace ts {

void player_shoot(GameEngine& engine) {
    PlayerInfo& p = engine.player;
    p.is_shooting = true;

    if (rand() % 20 < 10
        && p.init_animation == 0
        && p.gameover_animation == 0
        && p.z < GRID_ELEMENT_LENGTH) {
        engine.init_shot(p.grid_element_pos, p.z, p.shot_id);
        p.multiplier -= 0.001f;
    }
}

void player_jump(GameEngine& engine) {
    PlayerInfo& p = engine.player;
    if (p.has_jump
        && p.gameover_animation == 0
        && p.init_animation == 0
        && p.out_animation == 0
        && p.animation_jump == 0) {
        p.animation_jump = 1;
        p.multiplier -= 0.015f;
    }
}

void player_move_left(GameEngine& engine, bool pressed) {
    PlayerInfo& p = engine.player;

    if (p.gameover_animation > 0 || !pressed
        || p.init_animation > 50 || p.z >= GRID_ELEMENT_LENGTH) {
        p.leftdx = 0;
        return;
    }

    // Linear ramp to CLAW_MAX_SPEED over CLAW_RAMP_FRAMES held frames
    // (constants.h), instant zero on release -- the arcade model, mirrored
    // from an external reference implementation (see DOCTRINE.md). leftdx is the
    // held-frame counter: it scales the speed here and is still the "is the
    // player moving much" signal enemies.cpp reads for the shooter dodge test.
    // No renderer reads it. animation_phase is float so the SUB-LANE position
    // is continuous -- buildPlayer feeds it to the claw's LEAN shear
    // (entity_geometry.cpp), while the BODY stays pinned to the lane MIDPOINT
    // and steps a whole lane at a time. See "THE CLAW GRIPS THE LANE, IT DOES
    // NOT SLIDE" there: interpolating the body along the rim lifts the feet
    // off the borders, and was tried and reverted.
    if (p.leftdx < CLAW_RAMP_FRAMES) {
        p.leftdx += 1;
    }

    p.animation_phase += (static_cast<float>(p.leftdx) / CLAW_RAMP_FRAMES) * CLAW_MAX_SPEED;

    if (p.animation_phase >= 9.0f) {
        p.animation_phase -= 9.0f;
        p.grid_element_pos += 1;
        // No SFX on lane movement (user request) -- the the reference build ch13 crawl blip
        // is intentionally not ported.

        // EXE-FAITHFUL (FUN_00408088): round wraps at N (-> 0); OPEN clamps at
        // the last real face, N-1. grid_element_pos indexes a FACE (the
        // midpoint between stored point i and point i+1), not a vertex --
        // engine.lane_count is now ALREADY the true face count for whichever
        // dataset is loaded (tools/gen_webs.py bakes it into levels.json --
        // this engine's own open levels store 16 points but only 15 are real
        // faces, since point 15's own direction entry is closure-only there;
        // an imported transition-reference web's lane_count is already exact).
        // So this is the SAME formula as the round branch above,
        // unconditionally, for either dataset -- no per-set special case. The phase=8 pin (stops the held-into-wall
        // claw twitch) is itself exe-faithful (the exe sets it too).
        if (engine.grid_level_go_round) {
            if (p.grid_element_pos > engine.lane_count - 1) {
                p.grid_element_pos = 0;
            }
        } else if (p.grid_element_pos > engine.lane_count - 1) {
            p.grid_element_pos = engine.lane_count - 1;
            p.animation_phase = 8.0f;
        }
    }
}

void player_move_right(GameEngine& engine, bool pressed) {
    PlayerInfo& p = engine.player;

    if (p.gameover_animation > 0 || !pressed
        || p.init_animation > 50 || p.z >= GRID_ELEMENT_LENGTH) {
        p.rightdx = 0;
        return;
    }

    // See player_move_left for the ramp rationale/citation.
    if (p.rightdx < CLAW_RAMP_FRAMES) {
        p.rightdx += 1;
    }

    p.animation_phase -= (static_cast<float>(p.rightdx) / CLAW_RAMP_FRAMES) * CLAW_MAX_SPEED;

    // Threshold is "< 0", not the old "<= -1": the int model's integer speed
    // could only ever land EXACTLY on -1 the frame it first went negative (it
    // stepped down one whole phase-unit at a time from a 0..8 range), so
    // "<=-1" and "<0" were equivalent there. A fractional speed can overshoot
    // to anywhere in (-speed, 0) on the crossing frame -- checking "<=-1"
    // specifically would miss smaller overshoots, letting phase drift further
    // negative each frame until it eventually cleared -1 in one jump and
    // double-counted a lane change.
    if (p.animation_phase < 0.0f) {
        p.animation_phase += 9.0f;
        p.grid_element_pos -= 1;
        // No SFX on lane movement (user request) -- the the reference build ch13 crawl blip
        // is intentionally not ported.

        if (p.grid_element_pos < 0) {
            if (engine.grid_level_go_round) {
                p.grid_element_pos = engine.lane_count - 1;
            } else {
                p.grid_element_pos = 0;
                p.animation_phase = 0.0f;
            }
        }
    }
}

void player_tremor(GameEngine& engine, int time) {
    PlayerInfo& p = engine.player;
    if (p.has_tremor
        && p.gameover_animation == 0
        && p.init_animation == 0
        && p.z < GRID_ELEMENT_LENGTH
        && p.animation_tremor == 0
        && p.animation_zapp == 0) {
        p.animation_tremor = 1;
        engine.show_powerup_text(time, 7); // "tremor"
        p.multiplier *= 0.5f;
    }
}

void player_zapper(GameEngine& engine, int time) {
    PlayerInfo& p = engine.player;
    // Two uses per level (ZAPPER_STOCK_PER_LEVEL), restocked at every level
    // start and therefore at every death. A third press in the same level is
    // simply refused.
    if (p.zapp_stock > 0
        && p.gameover_animation == 0
        && p.init_animation == 0
        && p.z < GRID_ELEMENT_LENGTH
        && p.animation_zapp == 0
        && p.animation_tremor == 0) {
        p.animation_zapp = 1;
        // The use that empties the rack is the SINGLE-KILL zap: its beam ends
        // the instant it destroys its first enemy (move_zapper).
        p.zapp_single = (p.zapp_stock == 1);
        Vec3 pos = engine.grid_level_pos[p.grid_element_pos];
        engine.zapper_target = {pos.x, pos.y, -GRID_ELEMENT_LENGTH};
        engine.old_zapper_el_pos = -1;
        engine.old_zapper_num = -1;
        // "eat electric death!" shows when the superzapper is fired with a charge
        // left in the rack after use. The last charge fires silently: stock >= 2
        // is the multi-kill zap (stock == 1 is the single-kill zap), so the
        // screen-clear zap gets "eat electric death" and the last one keeps "zappo".
        engine.show_powerup_text(time, p.zapp_stock >= 2 ? 15 : 8);
        p.multiplier *= 0.5f;
        flash_raise(engine.flash, FLASH_SUPERZAP, time);
    }
}

// Faithful port of the original MoveJump. Control flow mirrors the reference build:
// the out-animation path does NOT early-return (damage + jump collisions still
// run during the level-exit slide), and the el-zapper/rect/spike damage checks
// fall through — for a non-jumping player the jump gate at the bottom is what
// ends the tick, exactly as the reference build's line 4127 exits.
void move_jump(GameEngine& engine, int time) {
    PlayerInfo& p = engine.player;

    // Decrement misc counters
    if (engine.control_ani > 0) {
        engine.control_ani -= 1;
    }
    if (engine.screen_flash > 0) {
        engine.screen_flash += 1;
        if (engine.screen_flash >= engine.max_screen_flash) {
            engine.screen_flash = 0;
        }
    }
    if (p.glow > 0) {
        p.glow -= 1;
    }
    if (p.warp_animation > 0) {
        p.warp_animation -= 1;
    }

    // --- Gameover Animation (ref build:4032-4042) ---
    if (p.gameover_animation > 0) {
        float val = (200 - p.gameover_animation) * (0.06f / GRID_ELEMENT_LENGTH);
        p.z -= val * val;
        p.gameover_animation += 1;

        // CLEAR THE LEVEL'S OBJECTS FROM MEMORY ONCE THE WEB HAS PULLED AWAY.
        // The render gate (goWebFadeNow) hides the entities as the web recedes,
        // but the engine's entity DATA otherwise survives the whole sequence.
        // The contract is that once the web is gone the objects are GONE, not
        // merely hidden. Fire the one-shot the tick the web finishes pulling
        // away (gameoverWebT reaches 1.0, ~0.5 s into the dive) -- strictly
        // after the render has already dropped the entities at its 0.996 gate,
        // and while the dive's own enemy/collision sim is early-returning on
        // gameover_animation > 0, so no live reference into a lane is held.
        if (!engine.gameover_entities_cleared &&
            ts::gameoverWebT(ts::gameoverRamp(engine)) >= 1.0f) {
            engine.clear_gameplay_entities();
            engine.gameover_entities_cleared = true;
        }

        if (p.gameover_animation == 200) {
            if (p.lives < 0) {
                p.gameover_animation = 199;
                if (engine.nolives_animation < 400) {
                    engine.nolives_animation += 1;
                    // THE GAME-OVER SEAM, and the LOUDEST of the three: at 400
                    // game_advance stops running (game_step.cpp's own gate) and
                    // -- unlike the warp case -- there is no return to gameplay
                    // to release anything. A voice live here rides the frozen
                    // game-over frame, the high-score table and the boot menu.
                    // Both loops are stranded, so tear both down on the exact
                    // tick the sim freezes.
                    if (engine.nolives_animation >= 400) engine.stop_looping_sfx();
                }
            } else {
                engine.init_level(time, engine.current_level);
            }
        }
        return;
    }

    // --- Level Exit Check (ref build:4044-4049) ---
    if (p.out_animation == 0 && engine.is_level_clear()) {
        engine.exit_level(time);
    }

    // Init animation countdown.
    //
    // FROZEN BY THE LEVEL-ASSET HOLD, on the same predicate the arrival glide
    // uses (camera.h ASSET_HOLD_MAX_TICKS). This counter and that glide are ONE
    // CLOCK -- the glide's deceleration is solved to reach z 0 with velocity 0
    // after exactly these 250 frames -- so letting the counter run while the
    // glide is held would hand the web fewer frames than its curve needs and
    // land it short, with a snap. Freeze both or neither.
    if (p.init_animation > 0 && !camera_arrival_held(engine)) {
        p.init_animation -= 1;
    }
    // Fire the deferred first-level-clear bonus once the new web has all but
    // landed, so the 1UP shatter (init_1up -> SHATTER_STYLE_ONEUP) starts after
    // the transition rather than mid-way (user request). init_animation<=50 is
    // the same gate player_move_left/right (above) and move_ai_droid
    // (weapons.cpp) use --
    // the last 50 of the 250-frame entry counter -- and the ARRIVAL GLIDE runs
    // on that same clock (camera.cpp's slide-in, solved over N=250 from
    // tstrans::IN_Z 270, frozen by the asset hold alongside this counter), so
    // at 50 it has ~9.9 of its 270 units left: the web is effectively in view.
    //
    // This used to be justified as "buildView's zoom-active threshold, where
    // the zoom formula evaluates to identity". THAT ZOOM IS GONE from both
    // backends -- c3d/04_setup.inc buildView and renderer_vk.cpp viewMatrix are
    // multiplier wobble, game-over spiral and the eye translate, nothing else,
    // and the transition rides world_trans.z now. Do not go looking for it.
    if (p.first_level_bonus_pending && p.init_animation <= 50) {
        p.first_level_bonus_pending = false;
        engine.init_1up(time);
    }
    // The ARMED companion (a capsule taken during the climb-out, the "yes yes
    // yes") is spent HERE, on the same gate as the bonus above, so the droid is
    // already hunting when lateral control returns. See its own comment for why
    // this is the arrival tick and not just a number.
    engine.summon_armed_droid(time);

    float dz = 0.0f;
    float nz = 0.0f;

    // --- Out Animation (exiting level, ref build:4052-4081) ---
    if (p.out_animation > 0) {
        float out_val = p.out_animation * (0.11f / GRID_ELEMENT_LENGTH);
        dz = out_val * out_val + GRID_ELEMENT_LENGTH / 500.0f;
        nz = p.z + dz;
        p.out_animation += 1;

        if (p.out_animation == 250) {
            // THE START-LEVEL BONUS IS PAID HERE, because HERE is where the
            // level the player picked is actually cleared. Before the branch
            // on purpose: taking a warp round is still a clear, and the
            // level-100 early return below is too.
            if (engine.pending_start_bonus > 0) {
                p.score += engine.pending_start_bonus;
                engine.pending_start_bonus = 0;
            }
            // THE WARP IS CASHED HERE, and it is never player-triggered: three
            // banked tokens (earned one per level from the ladder's warp slot)
            // are spent automatically at the transition out of the level the
            // third one landed in. WARP_BONUS_ROUND_ENABLED is the sole gate;
            // false forces the normal level-advance branch.
            if (p.warp_icons < WARP_ICONS_FOR_WARP || !WARP_BONUS_ROUND_ENABLED) {
                if (engine.current_level >= 99) {
                    p.out_animation = 249;
                    engine.current_level = 100; // main loop handles level-100 end
                    return;
                }
                // Guaranteed bonus life for clearing the FIRST level (current_level==0,
                // about to become 1). INTENTIONAL DEVIATION (user request), not exe-
                // faithful -- the original only grants 1UPs by crossing a 50000-point
                // score threshold (still fully intact below/elsewhere; this is additive,
                // not a replacement). DEFERRED via first_level_bonus_pending (consumed
                // in move_jump's init_animation countdown, above) so the swirl starts
                // once the new level's web is fully in view, not mid-transition.
                if (engine.current_level == 0) p.first_level_bonus_pending = true;
                engine.current_level += 1;
                engine.sfx.push(SfxId::SUPERZAP);   // Super Zapper Recharge, between levels (user request)
            flash_raise(engine.flash, FLASH_HANDOVER, time);   // the level advance flash
                engine.change_current_level(time, engine.current_level);
            } else {
                p.out_animation = 249;
                engine.warp_reached = true;
                // THE WARP SEAM. out_animation is PINNED here (set 249,
                // move_jump bumps it to 250, this branch pins 249 again), so
                // game_advance's `out_animation == 0` release can never fire --
                // and the frontend is about to leave GAMEPLAY, so game_advance
                // stops running at all. Measured before this line: a 604-tick
                // (9.66 s) unattended YES drone under the whole bonus round, at
                // frozen pitch. Tear the loops down at the seam that causes it.
                engine.stop_looping_sfx();
                // The SIGNS ride the same seam as the VOICE. A STYLE_YES chant
                // still inside its 4.2 s life at warp entry would keep
                // detonating in the bonus round: the world-space path
                // suppresses all non-empty events under GameState::WARP and
                // the warp burst mirrors the same engine events, so the
                // chant's dots arrive on the round's own popup path. The
                // user's contract: the YESes die at the flash, not in the
                // round. See engine.h clear_shatter_events.
                engine.clear_shatter_events();
            }
            return;
        }
        // NOTE: no early return here — the reference build falls through so damage and jump
        // collisions still run while the player slides out.
    }

    // --- Multiplier Tick (ref build:4084-4086) ---
    p.multiplier += 0.0001125f;
    if (p.multiplier < 0.1f) p.multiplier = 0.1f;
    if (p.multiplier > 2.0f) p.multiplier = 2.0f;

    // (The multiplier MILESTONE that used to sit here -- ref build:4088-4105, the
    // `missile_level * 0.3333 + 1.3333` threshold -- is gone with the automines
    // it existed to arm, and so is its second job of handing out a warp icon at
    // missile_level == 2. Warp tokens now come from exactly one place, the
    // ladder's warp slot in init_powerup. The multiplier itself is untouched:
    // it is this engine's scoring economy and it still ticks above. See
    // DOCTRINE.md "Intentional deviations".)

    // The following damage + jump collisions only ever consult the PLAYER's own
    // lane (the reference build binds v := grid_element_pos once, ref build:4114).
    int v = p.grid_element_pos;
    GridElement& pelem = engine.grid[v];

    // --- El-zapper / rectangle electrocution (ref build:4116-4120) ---
    if (p.z >= 0.0f && pelem.num_enemies > 0) {
        /* @vfp-exempt R3 — el-zapper/rect electrocution window (ref build:4116-4120): the float p.z >= 0 player-plane gate admits the per-enemy kill decision. measured n/a. Verified 2026-09-06. */
        for (int v3 = 0; v3 < pelem.num_enemies; v3++) {
            Enemy& e = pelem.enemies[v3];
            bool zap = (e.id == EL_ZAPPER1
                        && e.animation_phase > (e.max_animation * 3) / 4);
            bool rect = (e.id >= RECT1 && e.id <= RECT2
                         && e.animation_phase > e.max_animation / 4
                         && e.animation_phase < (e.max_animation * 3) / 4);
            if (zap || rect) {
                engine.init_explosion(time, e.life / 2, v, 0.0f, EXPLOSION_PLAYER, 2);
                engine.init_gameover(time, 14); // "fried you"
            }
        }
    }

    // --- Spike damage (ref build:4122-4125) ---
    if (pelem.spike - GRID_ELEMENT_LENGTH >= -p.z && p.z < GRID_ELEMENT_LENGTH) {
        engine.init_explosion(time, 150, v, p.z, EXPLOSION_PLAYER, 3);
        engine.init_gameover(time, 15); // "eat electric death"
    }

    // --- Jump gate (ref build:4127): only continue while jumping OR sliding out ---
    if ((!p.has_jump || p.animation_jump == 0) && p.out_animation == 0) return;

    // Jump parabola (ref build:4129-4132) — out-slide keeps its own dz/nz.
    if (p.out_animation == 0) {
        float t_val = (p.animation_jump - 40) * (2.5f / 40.0f);
        nz = t_val * t_val - 6.25f;
        dz = nz - p.z;
    }

    // Collisions during upward sweep (ref build:4134-4155).
    /* @vfp-exempt R3 — jump upward-sweep gate plus swept kill windows (ref build:4129-4155): parabola dz and shot/enemy intervals decide death/kill at sweep boundaries. measured n/a. Verified 2026-09-06. */
    if (dz > 0.0f) {
        // Shots on the player's lane (swept interval z <= shot.z <= z+dz).
        /* @vfp-exempt R3 — jump swept shot window (ref build:4134-4145): per-shot float interval vs p.z+dz. measured n/a. Verified 2026-09-06. */
        for (int v3 = 0; v3 < pelem.num_shots; v3++) {
            Shot& s = pelem.shots[v3];
            if ((s.id >= ENEMY_SHOT1 || s.id == POWERUP_SHOT)
                && p.z + dz >= s.z && p.z <= s.z) {
                engine.init_explosion(time, s.life, v, p.z + dz * 0.5f,
                                      EXPLOSION_SHOT, s.id);
                if (s.id != POWERUP_SHOT) {
                    engine.init_explosion(time, s.life, v, p.z + dz * 0.5f,
                                          EXPLOSION_PLAYER, 0);
                    engine.init_gameover(time, 13); // "shot you"
                } else {
                    engine.init_powerup(time);
                    s.life = 0; // MoveShots reaps it; not removed mid-scan
                }
            }
        }
        // Enemies on the player's lane -> jump-into-enemy kill (ref build:4146-4153).
        for (int v3 = 0; v3 < pelem.num_enemies; v3++) {
            Enemy& e = pelem.enemies[v3];
            if (p.z + dz >= e.z && p.z <= e.z) {
                engine.init_explosion(time, e.life, v, p.z + dz * 0.5f,
                                      EXPLOSION_PLAYER, 1);
                engine.init_explosion(time, e.life, v, p.z + dz * 0.5f,
                                      EXPLOSION_ENEMY, e.id);
                engine.init_gameover(time, 16); // "caught you"
            }
        }
    }

    p.z = nz;
    if (p.out_animation == 0) {
        p.animation_jump += 1;
        if (p.animation_jump > 80) {
            p.animation_jump = 0;
            p.z = 0.0f;
        }
    }
}

} // namespace ts
