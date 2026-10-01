#include "collision.h"
#include "engine.h"
#include "camera.h"
#include "constants.h"
#include "models.h"
#include "enemies/enemies_shared.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include "math_lut.h"

namespace ts {

// Internal helper
static void _remove_shot(GameEngine& engine, GridElement& elem, int idx, Shot& shot) {
    engine.shots_nums[shot.id] -= 1;
    elem.num_shots -= 1;
    if (idx < elem.num_shots) {
        elem.shots[idx] = elem.shots[elem.num_shots];
    }
    if (!elem.shots.empty()) {
        elem.shots.pop_back();
    }
}

// =============================================================================
// move_shots
// =============================================================================

void move_shots(GameEngine& engine, int time) {
    PlayerInfo& p = engine.player;
    if (p.gameover_animation > 0) return;

    /* @vfp-exempt R3 — move_shots swept/bounds removal (pins the removal branch below): per-shot float bounds and life drain over live rosters. measured n/a. Verified 2026-09-06. */
    for (int v = 0; v < engine.lane_count; v++) {
        GridElement& elem = engine.grid[v];
        int s_idx = 0;
        /* @vfp-exempt R3 — shot scan loop (pins the removal branch below; split_loops segments start at the loop head and ref-bind). measured n/a. Verified 2026-09-06. */
        while (s_idx < elem.num_shots) {
            /* @vfp-exempt R3 — shot ref-bind island (pins the removal branch below; segment starts at the ref-bind). measured n/a. Verified 2026-09-06. */
            Shot& shot = elem.shots[s_idx];

            // ---- THE ARCADE REFLECTED SHOT OWNS ITS WHOLE PIPELINE --------
            // Your own bullet, taken over in place by a Mirror or a Beast and
            // sent back down your lane. It steps its own speed (SHOT_DZ[] is
            // 0 for it, exactly like ENEMY_DZ[] for every arcade enemy), runs
            // its own lethality test and its own expiry plane, and returns
            // the player's shot slot when it unlinks. None of the shared
            // block below applies to it.
            if (shot_is_arcade_reflected(shot.id)) {
                if (enemyfam::ArcadeReflectedShot::tick(engine, time, v, shot)) {
                    _remove_shot(engine, elem, s_idx, elem.shots[s_idx]);
                    continue;
                }
                s_idx++;
                continue;
            }

            // dz for this shot: signed. Player shots dz>0 (fly toward far),
            // enemy shots dz<0 (fly toward player). The single the reference build window
            // uses signed dz directly (no *2, no separate branch); for enemy
            // shots (dz<0) it is naturally empty vs enemies, so enemy shots
            // never damage enemies.
            float dz = SHOT_DZ[shot.id];
            bool is_player_shot = shot.id < ENEMY_SHOT1;

            // =================================================================
            // Shot vs Enemy Collision.
            // Ascending iteration. Mutual damage on ORIGINAL values: the shot
            // loses the enemy's original life; the enemy loses the shot's
            // original life (ol). No enemy removal and no early break here —
            // MoveEnemies removes dead enemies; the shot keeps scanning the lane
            // and is removed by the bounds/life check below.
            // =================================================================
            for (int e_idx = 0; e_idx < elem.num_enemies; e_idx++) {
                /* @vfp-exempt R3 — swept-window hit test on per-iteration mutable float state (shot.z/enemy.z/dz); integerizing changes the hit set. measured n/a. Verified 2026-09-06. */
                Enemy& enemy = elem.enemies[e_idx];
                if (!(shot.z <= enemy.z && shot.z + dz >= enemy.z))
                    continue;

                // THE VULNERABILITY WINDOW (audit C9; enemies_shared.h).
                // Always true for every classic id; the arcade set says no for
                // the arrival dots, the mid-flip flipper, a fuseball that is
                // not mid-crossing and the Mirror at all times (see
                // enemy_shootable() in enemies_shared.h).
                if (!enemy_shootable(enemy))
                    continue;

                // ...and the Mirror/Beast own their bullet test entirely: they
                // do not take damage from a shot, they TAKE THE SHOT.
                if (!enemy_uses_shared_shot_collision(enemy))
                    continue;

                // Space zapper dodges 90% of shots.
                if (enemy.id == SP_ZAPPER1 && rand() % 10 != 0)
                    continue;

                // The classic shot-kill pays its kill score HERE (at the sweep)
                // AND at `_handle_death` -- the faithful 2x score the design
                // keeps. But it DRAWS only ONE ball: this sweep's record is
                // suppressed with draw_visual=false, so the single visible ball
                // comes from `_handle_death` (enemies.cpp:848), the same point
                // an arcade kill uses. shed_bonus=false keeps the bonus capsule
                // batch single (shed at the death handler too). Net: a classic
                // shot-kill = 1 ball + 2x kill score + 1 capsule shed -- half
                // the explosion particles of the old 2-ball sweep, same score.
                // Arcade is untouched (gated off by !isArcadeEnemy). The shot's
                // OWN explosion (EXPLOSION_SHOT below) is unaffected.
                if (is_player_shot && shot.life >= enemy.life
                    && !isArcadeEnemy(enemy.id)) {
                    engine.init_explosion(
                        time, enemy.life, v, shot.z + dz * 0.5f,
                        EXPLOSION_ENEMY, enemy.id,
                        /*shed_bonus=*/false, /*draw_visual=*/false
                    );
                }
                // Shot destroyed -> shot explosion (real hits only, not the
                // reflector/rect pushbacks); else a reflect sound (audio TODO).
                if (shot.life <= enemy.life) {
                    if (enemy.id != RECT1 && enemy.id != RECT2 && (enemy.mush_flag & 2)) {
                        engine.init_explosion(
                            time, shot.life, v, shot.z + dz * 0.5f,
                            EXPLOSION_SHOT, shot.id
                        );
                    }
                    // else: playsound(reflect) — audio not yet ported.
                }

                int ol = shot.life;                 // original shot life
                shot.life -= enemy.life;            // loses original enemy.life
                if (shot.life < 0) shot.life = 0;

                // Player, non-powerup shots: reflector/rect pushback + damage.
                if (is_player_shot && shot.id != POWERUP_SHOT) {
                    if (enemy.id == REFLECTOR1) {
                        float rnd = rand() / (float)RAND_MAX;
                        enemy.z += dz * (1.0f + rnd);
                        engine.init_shot(v, enemy.z, REFLECT_SHOT1);
                    }
                    /* @vfp-exempt R3 — rect pushback window on per-enemy float z; integerizing changes pushback vs the original damage ordering. measured n/a. Verified 2026-09-06. */
                    /* @vfp-exempt R3 — rect window island (pins the if below; split_loops emits a bare-if segment here). measured n/a. Verified 2026-09-06. */
                    if (enemy.id == RECT1
                        || (enemy.id == RECT2 && (enemy.z > -2.0f || enemy.z < -2.5f))) {
                        float rnd = rand() / (float)RAND_MAX;
                        enemy.z += dz * (1.0f + rnd) * 0.42f;
                    }
                    // Enemy takes the shot's ORIGINAL life, gated: mushroom-half,
                    // player-primary shot, or 1-in-11 for other player shots.
                    if ((enemy.mush_flag & 2) || p.shot_id == PLAYER_SHOT1 || rand() % 11 == 0) {
                        enemy.life -= ol;
                        if (enemy.life < 0) enemy.life = 0;
                    }
                    // ARCADE ONLY (arcade_enemies.md sec 3.5): a confirmed kill
                    // CONSUMES an ordinary claw shot; only the particle laser
                    // punches through and can open several tankers in one
                    // pass. Against the arcade life of 1 a PLAYER_SHOT1 (life
                    // 100) would otherwise fly on at 99.
                    if (isArcadeEnemy(enemy.id) && enemy.life <= 0
                        && arcade_kill_consumes_shot(shot.id))
                        shot.life = 0;
                }
            }

            // =================================================================
            // Shot vs Shot Collision. Scan only shots after
            // this one (avoid double resolution); reflect shots are inert. A
            // player shot (dz>0) sweeps forward onto an enemy shot; an enemy
            // shot (dz<0) sweeps back onto a player shot. Mutual on original life.
            // =================================================================
            if (shot.life > 0 && shot.id != REFLECT_SHOT1) {
                for (int s2_idx = s_idx + 1; s2_idx < elem.num_shots; s2_idx++) {
                    Shot& other = elem.shots[s2_idx];
                    if (other.id == REFLECT_SHOT1) continue;
                    float odz = SHOT_DZ[other.id];
                    /* @vfp-exempt R3 — shot-vs-shot swept test on per-iteration float state (z/dz/odz); integerizing changes the mutual-kill set. measured n/a. Verified 2026-09-06. */
                    bool hit =
                        (is_player_shot && other.id >= ENEMY_SHOT1
                         && shot.z <= other.z && shot.z + dz >= other.z + odz)
                     || (!is_player_shot && other.id < ENEMY_SHOT1
                         && shot.z >= other.z && shot.z + dz <= other.z + odz);
                    if (!hit) continue;
                    if (shot.life >= other.life)
                        engine.init_explosion(time, other.life, v, shot.z + dz * 0.5f,
                                              EXPLOSION_SHOT, other.id);
                    if (shot.life <= other.life)
                        engine.init_explosion(time, shot.life, v, shot.z + dz * 0.5f,
                                              EXPLOSION_SHOT, shot.id);
                    int ol = shot.life;
                    shot.life -= other.life;
                    if (shot.life < 0) shot.life = 0;
                    other.life -= ol;
                    if (other.life < 0) other.life = 0;
                }
            }

            // =================================================================
            // Enemy/Powerup Shot vs Player Collision.
            // Enemy shot: swept interval z+dz <= player.z <= z. Powerup: widened
            // ±dz*10 window, gated on animation_phase < 42 -- the phase starts
            // at 110 (init_shot) and only counts DOWN, so the window is CLOSED
            // for the first 69 ticks of the capsule's approach animation and
            // open for the rest of it and forever after at phase 0, which is
            // where the catch actually lands (the capsule covers ~5.4 units in
            // the whole animation, then flies at constant speed -- see
            // docs/design/pickup_burst.md). NO jump guard — jumping does not
            // dodge shots. the reference build does not zero enemy-shot life here; it flies on
            // and is removed at the bounds check below.
            // =================================================================
            {
                /* @vfp-exempt R3 — player-crossing test: swept enemy interval plus widened powerup window vs player.z; float positions define the hit. measured n/a. Verified 2026-09-06. */
                bool enemy_hit = shot.id >= ENEMY_SHOT1
                    && shot.z >= p.z && shot.z + dz <= p.z;
                bool powerup_hit = shot.id == POWERUP_SHOT && shot.animation_phase < 42
                    && shot.z - dz * 10.0f >= p.z && shot.z + dz * 10.0f <= p.z;
                if ((enemy_hit || powerup_hit) && v == p.grid_element_pos && shot.life > 0) {
                    engine.init_explosion(time, shot.life, v, shot.z + dz * 0.5f,
                                          EXPLOSION_SHOT, shot.id);
                    if (shot.id != POWERUP_SHOT) {
                        engine.init_explosion(time, shot.life, v, shot.z + dz * 0.5f,
                                              EXPLOSION_PLAYER, 0);
                        engine.init_gameover(time, 13); // "shot you"
                    } else {
                        engine.init_powerup(time);
                        shot.life = 0;
                    }
                }
            }

            // =================================================================
            // Shot Movement
            // =================================================================
            if (shot.id == POWERUP_SHOT && shot.animation_phase != 0) {
                float factor = (110.0f - shot.animation_phase) / 110.0f;
                shot.z += SHOT_DZ[shot.id] * factor * factor;
                shot.animation_phase -= 1;
            } else {
                shot.z += SHOT_DZ[shot.id];
            }

            // =================================================================
            // Shot vs Spike Collision
            // =================================================================
            /* @vfp-exempt R3 — spike gate on simulated float hazard state (pins the comparisons below); visibility equals lethality. measured n/a. Verified 2026-09-06. */
            /* @vfp-exempt R3 — spike gate island (pins the if below; split_loops emits a bare-if segment here). measured n/a. Verified 2026-09-06. */
            if (shot.id < ENEMY_SHOT1
                && shot.id != POWERUP_SHOT
                && elem.spike > 0.0f) {
                float spike_z = GRID_ELEMENT_LENGTH - elem.spike;
                /* @vfp-exempt R3 — spike hit test on simulated float hazard state; integerizing moves the kill boundary vs lethality. measured n/a. Verified 2026-09-06. */
                if (shot.z >= spike_z) {
                    // The arcade reference (run_spike/decspike, see DOCTRINE.md): a hit
                    // removes a FIXED chunk of the spike -- 2 units off a
                    // 30-unit spike, i.e. 1/15th, independent of shot type.
                    // (Its `laserspike` test loads the same value on BOTH
                    // branches, so the intended laser bonus never applied.)
                    //
                    // The ported rule in this engine used to scale erosion by shot.life
                    // (life * L / 18750), which at our lane length of 25 works
                    // out to 0.133 per hit -- 159 shots to clear one SPIKER1
                    // spike and 216 for a SPIKER2. Spikes were effectively
                    // unshootable. See DOCTRINE.md "Intentional deviations".
                    //
                    // Capture the spike's height BEFORE this hit erodes it: the
                    // tink's period is computed from the length as it stood at
                    // the hit, so the ramp reflects the pre-hit height. Passed
                    // to init_explosion, which turns it into the pitch (see
                    // engine.h).
                    const float spike_height = elem.spike;
                    elem.spike -= SPIKE_EROSION_PER_HIT;

                    /* @vfp-exempt R3 — spike erosion break: float erosion remainder decides detonate-vs-chip; per-hit simulated state. measured n/a. Verified 2026-09-06. */
                    /* @vfp-exempt R3 — erosion break island (pins the if below; split_loops emits a bare-if segment here). measured n/a. Verified 2026-09-06. */
                    if (elem.spike <= 0.0f) {
                        elem.spike = 0.0f;
                        engine.init_explosion(
                            time, shot.life * 3,
                            v, spike_z, EXPLOSION_SPIKE, 0, true, true, spike_height
                        );
                        if (engine.note_powerup_kill()) {
                            engine.init_shot(v, spike_z, POWERUP_SHOT);
                            engine.show_powerup_text(time, -1);
                        }
                    } else {
                        engine.init_explosion(
                            time, shot.life / 2, v, spike_z,
                            EXPLOSION_SPIKE, 0, true, true, spike_height
                        );
                    }
                    shot.life = 0;
                }
            }

            // =================================================================
            // Shot Removal (out of bounds or dead)
            // =================================================================
            /* @vfp-exempt R3 — shot removal on float bounds/life: swept-interval bounds and life drain are the removal law. measured n/a. Verified 2026-09-06. */
            const bool outOfBounds = (shot.z >= GRID_ELEMENT_LENGTH)
                || (shot.z <= -GRID_ELEMENT_LENGTH * 0.25f);
            const bool dead = (shot.life <= 0);
            if (outOfBounds || dead) {
                // Track killed enemy shots for powerup spawning
                if (shot.life <= 0 && shot.id >= ENEMY_SHOT1) {
                    if (engine.note_powerup_kill()) {
                        engine.init_shot(v, shot.z, POWERUP_SHOT);
                        engine.show_powerup_text(time, -1);
                    }
                }

                _remove_shot(engine, elem, s_idx, elem.shots[s_idx]);
                continue;
            }

            s_idx++;
        }
    }
}

// =============================================================================
// move_bonus
// =============================================================================

void move_bonus(GameEngine& engine, int time) {
    PlayerInfo& p = engine.player;
    if (p.gameover_animation > 0) return;

    int i = 0;
    while (i < (int)engine.bonuses.size()) {
        Bonus& bonus = engine.bonuses[i];
        // the reference build floors the phase at -max_animation, it does not
        // decrement past it — the phase feeds the drift/z-speed formulas below.
        if (bonus.animation_phase > -bonus.max_animation) {
            bonus.animation_phase -= 1;
        }
        bool removed = false;

        // Lateral attraction toward player's lane
        if (!p.is_shooting && bonus.grid_element_pos != p.grid_element_pos) {
            float d2 = (float)(p.grid_element_pos - bonus.grid_element_pos) - bonus.d;

            // Wrap around for closed grids
            /* @vfp-exempt R3 — bonus lane-wrap on evolving float drift d2; wrap + force integrator define lane-slip behavior vs the original. measured n/a. Verified 2026-09-06. */
            if (engine.grid_level_go_round) {
                if (d2 > engine.lane_count / 2.0f) {
                    d2 -= engine.lane_count;
                } else if (d2 < -engine.lane_count / 2.0f) {
                    d2 += engine.lane_count;
                }
            }

            d2 *= (p.z - bonus.z) * (p.z - bonus.z);

            /* @vfp-exempt R3 — bonus force integrator: d2!=0 guard plus force clamp feed bonus.d; lane-transition thresholds define pickup behavior. measured n/a. Verified 2026-09-06. */
            if (d2 != 0.0f) {
                float force = (100 - bonus.animation_phase) * 0.0175f / d2 + d2 * 0.0000175f;
                force = std::max(-0.05f, std::min(0.05f, force));
                bonus.d += force;
            }

            // Lane transitions
            if (bonus.d >= 0.5f) {
                bonus.grid_element_pos = (bonus.grid_element_pos + 1) % engine.lane_count;
                bonus.d -= 1.0f;
            } else if (bonus.d <= -0.5f) {
                // R11: one-period bound HOLDS (grid_element_pos-1 is at most
                // one period below 0), so `if(v<0)v+=n;` would be an exact
                // zero-divide identity. Mechanics-gated and not
                // profile-justified; left as the modulo positive-wrap.
                bonus.grid_element_pos = ((bonus.grid_element_pos - 1) % engine.lane_count + engine.lane_count) % engine.lane_count;
                bonus.d += 1.0f;
            }
        } else {
            bonus.d *= 0.975f;
        }

        // Z movement
        bonus.z -= GRID_ELEMENT_LENGTH / 150.0f - bonus.animation_phase * 0.0015f;

        // Collection or expiry check
        bool fell_off = bonus.z < -GRID_ELEMENT_LENGTH * 0.275f;
        bool in_range = (bonus.grid_element_pos == p.grid_element_pos
                         && fabsf(bonus.z - p.z) < 1.0f);

        // the reference build removes a bonus only when it falls off the back or is collected
        // there is no separate lifetime expiry.
        if (fell_off || in_range) {
            if (in_range && !fell_off) {
                // Collected! Award score
                engine.sfx.push(SfxId::BONUS_PICKUP);
                // NOTE: multiplier*40 is exactly representable; roundf keeps
                // single-precision throughout (was double round via cast).
                /* @vfp-exempt R2 — score domain crossing: roundf-to-int is the required float-to-score conversion; feeds award_score and 1UP thresholds. measured n/a. Verified 2026-09-06. */
                /* @vfp-exempt R2 — score cast island (pins the cast below; split_loops emits a bare segment here). measured n/a. Verified 2026-09-06. */
                int score_val = (int)roundf(p.multiplier * (float)BONUS_PICKUP_POINTS);
                engine.award_score(time, score_val);
                p.multiplier += 0.001f;

                // Pickup sparkle: a small "+N" popup near the lower-left HUD --
                // N is score_val above (multiplier * BONUS_PICKUP_POINTS), not
                // the base -- gated so a fast pickup streak doesn't flood the
                // screen.
                // The BONUS_PICKUP sfx above plays unconditionally on every pickup, but
                // the the reference build only fires it inside this same gate
                // -- worth checking whether the SFX should be gated identically to
                // the popup rather than firing every time.
                if (p.glow < 25 || rand() % 10 == 0) {
                    auto rnd = [&]() { return rand() / (float)RAND_MAX; };
                    engine.init_score(
                        score_val,   // what was ACTUALLY awarded, multiplier and
                                     // all -- printing the un-multiplied base
                                     // understated every pickup above 1.0x
                        (0.025f + (rnd() - 0.5f) * 0.1f) * 1.3333f,
                        0.975f + (rnd() - 0.5f) * 0.1f,
                        0.4f + (rnd() - 0.5f) * 0.5f,
                        0.4f + (rnd() - 0.5f) * 0.5f,
                        0.7f + (rnd() - 0.5f) * 0.6f
                    );
                }
                p.glow = 30 - rand() % 3;
            }

            // Remove bonus (swap with last)
            engine.bonuses[i] = engine.bonuses.back();
            engine.bonuses.pop_back();
            removed = true;
        }

        if (!removed) {
            i++;
        }
    }

    p.is_shooting = false;
}

// =============================================================================
// move_explosions
// =============================================================================

void move_explosions(GameEngine& engine) {
    if (engine.player.gameover_animation > 0) return;

    for (int type_id = 0; type_id < 5; type_id++) {
        int i = 0;
        while (i < engine.explosions_nums[type_id]) {
            Explosion& exp = engine.explosions[type_id][i];
            exp.animation_phase += 1;
            if (exp.animation_phase >= exp.max_animation) {
                engine.explosions_nums[type_id] -= 1;
                if (i < engine.explosions_nums[type_id]) {
                    engine.explosions[type_id][i] = engine.explosions[type_id][engine.explosions_nums[type_id]];
                }
                engine.explosions[type_id].pop_back();
            } else {
                i++;
            }
        }
    }
}

// =============================================================================
// move_scores
// =============================================================================

void move_scores(GameEngine& engine) {
    int i = 0;
    while (i < (int)engine.scores.size()) {
        engine.scores[i].animation_phase += 1;
        if (engine.scores[i].animation_phase >= engine.scores[i].max_animation) {
            engine.scores[i] = engine.scores.back();
            engine.scores.pop_back();
        } else {
            i++;
        }
    }
}

// =============================================================================
// move_cam
// =============================================================================

// The legacy the reference build camera that used to live here (an exponential 0.975/0.025 lerp
// toward the player's lane, with the shake folded into the accumulated state and
// z chasing -player.z) has been REPLACED by the arcade reference's camera law --
// see game/camera.{h,cpp}. move_cam is now just the reference's `moveclaw` ordering:
// ease toward last frame's target, then recompute the target. The shake moved to
// camera_eye(), because accumulating random jitter into the eased position kept
// kicking the eye out of its deadzone and destroyed the characteristic hold.
void move_cam(GameEngine& engine) {
    camera_xform(engine);   // vp_xform -- top of moveclaw, see DOCTRINE.md
    camera_set(engine);     // vp_set   -- bottom of moveclaw, see DOCTRINE.md
}

} // namespace ts
