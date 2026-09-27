#include "enemies.h"
#include "engine.h"
#include "constants.h"
#include "models.h"
#include "enemies/reflector.h"
#include "enemies/arcade_flipper.h"
#include "enemies/arcade_tanker.h"
#include "enemies/arcade_spiker.h"
#include "enemies/arcade_mirror.h"
#include "enemies/arcade_fuseball.h"
#include "enemies/arcade_pulsar.h"
#include "enemies/enemies_shared.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace ts {

// Forward declarations of internal helpers
static bool _ai_shooter(GameEngine& engine, int time, int lane, int idx, Enemy& enemy);
static void _ai_container(GameEngine& engine, int time, int lane, int idx, Enemy& enemy);
static bool _ai_el_zapper(GameEngine& engine, int time, int lane, int idx, Enemy& enemy);
static bool _ai_mushroom(GameEngine& engine, int time, int lane, int idx, Enemy& enemy);
static bool _ai_rectangle(GameEngine& engine, int time, int lane, int idx, Enemy& enemy);
static bool _ai_spiker(GameEngine& engine, int time, int lane, int idx, Enemy& enemy);
static bool _ai_sp_zapper(GameEngine& engine, int time, int lane, int idx, Enemy& enemy);
static void _apply_movement(GameEngine& engine, int lane, int idx, Enemy& enemy);
static bool _handle_death(GameEngine& engine, int time, int lane, int idx, Enemy& enemy);
static int _hatch_child_tweak(GameEngine& engine, int child_lane, float z, bool is_zapper, bool second, int parent_lane, int forced_max);
static bool _handle_mushroom_split(GameEngine& engine, int lane, int idx, Enemy& enemy);
static bool _transfer_enemy(GameEngine& engine, int from_lane, int idx, int to_lane);
static void _remove_enemy(GameEngine& engine, int lane, int idx, Enemy& enemy);

// =============================================================================
// move_enemies
// =============================================================================

void move_enemies(GameEngine& engine, int time) {
    /* @vfp-exempt R3 — move_enemies swept windows (pins the shot/enemy/player branches below): per-iteration float z/edz/dz over live rosters. measured n/a. Verified 2026-09-06. */
    PlayerInfo& p = engine.player;
    // Accepted gap: during the gameover transition we skip enemy motion entirely,
    // so a classic enemy killed on the exact frame gameover starts is stranded
    // (life<=0) -- its sweep-suppressed ball is never drawn and its death score /
    // bonus shed / powerup cadence never run. The score is moot once the run is
    // over and the transition is chaotic, so the missing ball is a minor cosmetic
    // gap, not worth a death pass here. (The stranded enemy itself still renders
    // during the opening gameover ramp -- a PRE-EXISTING condition independent of
    // the explosion change, and out of scope.) The RECT2 fade-out removal (see
    // _ai_rectangle) is the one normal-play removal bypass that gets a net.
    if (p.gameover_animation > 0) return;

    // Per-FRAME scalars, hoisted once (enemies/enemy_family.h EnemyCtx). The
    // families read them from here instead of reaching into `engine` per
    // enemy.
    const enemyfam::EnemyCtx ctx{ time, engine.lane_count,
                                  engine.grid_level_go_round };

    /* @vfp-exempt R3 — lane loop (pins the swept branches below; split_loops segments start at the loop head). measured n/a. Verified 2026-09-06. */
    for (int v = 0; v < engine.lane_count; v++) {
        GridElement& elem = engine.grid[v];
        int v2 = 0;
        /* @vfp-exempt R3 — enemy loop (pins the swept branches below; segments start at the loop head). measured n/a. Verified 2026-09-06. */
        while (v2 < elem.num_enemies) {
            Enemy& enemy = elem.enemies[v2];
            bool deleted = false;

            // THE ONE DISPATCH. A `switch` on a small dense enum -- a
            // compiler-chosen branch (jump table or if-chain, its call), NOT
            // dynamic dispatch: no virtual, no vtable, no per-enemy function
            // pointer, no `static void(*table[N])(...)` indexed in the loop.
            // See enemies/enemy_family.h.
            //
            // `default:` is the LANDING PAD for a declared-but-unimplemented
            // id: it does not act, so such an id is inert rather than
            // undefined. All twelve arcade ids used to land there; the seven
            // family structs below now claim every one of them, and the
            // fourteen classic ids above are all claimed too, so NO DECLARED
            // ID reaches it today. `Enemy::id` is a plain `int` (models.h), so
            // the pad still catches an out-of-range value -- and it catches an
            // id added WITHOUT a case, which is what the audit
            // (docs/design/enemy_pipeline_audit.md) exists to make loud.
            switch (enemy.id) {
            case SHOOTER1:
            case SHOOTER2:
                deleted = _ai_shooter(engine, time, v, v2, enemy);
                break;
            case CONTAINER1:
            case CONTAINER2:
            case CONTAINER3:
            case CONTAINER4:
                _ai_container(engine, time, v, v2, enemy);
                break;
            case EL_ZAPPER1:
                deleted = _ai_el_zapper(engine, time, v, v2, enemy);
                break;
            case MUSHROOM:
                deleted = _ai_mushroom(engine, time, v, v2, enemy);
                break;
            case REFLECTOR1:
                deleted = enemyfam::Reflector::update(engine, ctx, v, v2, enemy);
                break;
            case RECT1:
            case RECT2:
                deleted = _ai_rectangle(engine, time, v, v2, enemy);
                break;
            case SPIKER1:
            case SPIKER2:
                deleted = _ai_spiker(engine, time, v, v2, enemy);
                break;
            case SP_ZAPPER1:
                deleted = _ai_sp_zapper(engine, time, v, v2, enemy);
                break;

            // ---- the ARCADE roster (docs/design/arcade_enemies.md) ----------
            // Each family claims exactly the ids its own header's `Ids[]`
            // lists; the two lists must agree, and this is the single
            // dispatch. ARCADE_ROSTER_READY is now true and every mask row is
            // non-empty (enemy_spawns.h static_asserts it), so
            // enemy_spawn_mask() resolves Arcade to the arcade table and these
            // run in normal play. The gate was held shut until the wave that
            // gave them GEOMETRY: an id with no geometry row is fully
            // simulated, fully lethal and COMPLETELY INVISIBLE (audit
            // sec 10/G1).
            case ARCADE_FLIPPER:
            case ARCADE_SFLIPPER2:
            case ARCADE_SFLIPPER3:
            case ARCADE_BEAST:
                deleted = enemyfam::ArcadeFlipper::update(engine, ctx, v, v2, enemy);
                break;
            case ARCADE_MIRROR:
                deleted = enemyfam::ArcadeMirror::update(engine, ctx, v, v2, enemy);
                break;
            case ARCADE_TANKER:
            case ARCADE_FUSE_TANKER:
            case ARCADE_PULSAR_TANKER:
                deleted = enemyfam::ArcadeTanker::update(engine, ctx, v, v2, enemy);
                break;
            case ARCADE_SPIKER:
                deleted = enemyfam::ArcadeSpiker::update(engine, ctx, v, v2, enemy);
                break;
            case ARCADE_FUSEBALL:
                deleted = enemyfam::ArcadeFuseball::update(engine, ctx, v, v2, enemy);
                break;
            case ARCADE_PULSAR:
                deleted = enemyfam::ArcadePulsar::update(engine, ctx, v, v2, enemy);
                break;
            case ARCADE_PULSAR_SPARK:
                deleted = enemyfam::ArcadePulsarSpark::update(engine, ctx, v, v2, enemy);
                break;
            case ARCADE_ADROID:
                deleted = enemyfam::ArcadeAdroid::update(engine, ctx, v, v2, enemy);
                break;

            default:
                break;
            }

            if (deleted) continue;

            // =================================================================
            // Enemy vs Shot Collision. The complementary
            // sweep to MoveShots: here the ENEMY's forward window catches a shot
            // it would otherwise overtake between frames. Evaluated at
            // pre-increment z. Mutual damage on ORIGINAL values (ol = enemy life
            // captured before it is decremented). No early break — the enemy
            // keeps scanning the lane's shots.
            // =================================================================
            {
                float edz = ENEMY_DZ[enemy.id];
                for (int sh = 0; sh < elem.num_shots; sh++) {
                    Shot& shot = elem.shots[sh];
                    // Window: z >= shot.z >= z+enemy_dz (edz<0; enemy flies in).
                    /* @vfp-exempt R3 — enemy-vs-shot swept window: per-iteration float z/edz; integerizing changes hit-vs-miss at window edges. measured n/a. Verified 2026-09-06. */
                    const bool inWindow = (enemy.z >= shot.z) && (enemy.z + edz <= shot.z);
                    if (!inWindow)
                        continue;
                    // THE VULNERABILITY WINDOW (audit C9; enemies_shared.h).
                    // Always true for every classic id. The arcade set says no
                    // for the flipper/tanker/pulsar arrival dots, the mid-flip
                    // flipper, a fuseball that is not mid-crossing (arrival,
                    // climb, and the outer 7 of its 16 crossing steps) and the
                    // Mirror at all times -- enemy_shootable() in
                    // enemies_shared.h is the full list. Placed before the
                    // SP_ZAPPER1 roll so an invulnerable enemy consumes
                    // nothing at all, which is what "no collision test of any
                    // kind" means.
                    if (!enemy_shootable(enemy))
                        continue;
                    // The Mirror/Beast own their bullet test; a reflected shot
                    // damages nothing at all.
                    if (!enemy_uses_shared_shot_collision(enemy))
                        continue;
                    if (shot_is_arcade_reflected(shot.id))
                        continue;
                    // Space zapper enemy dodges 90% of shots.
                    if (enemy.id == SP_ZAPPER1 && rand() % 10 != 0)
                        continue;

                    bool is_player_shot = shot.id < ENEMY_SHOT1;
                    // The complementary sweep to collision.cpp move_shots.
                    // Same contract as there: pay the faithful 2x kill score
                    // (this pay + the _handle_death pay) but draw NO ball --
                    // draw_visual=false suppresses the record so the single
                    // visible explosion comes from _handle_death. shed_bonus=
                    // false keeps the bonus capsule batch single. Arcade is
                    // gated off (!isArcadeEnemy): an arcade kill is paid ONCE,
                    // by _handle_death through arcade_kill_score(); firing here
                    // would pay the arcade life of 1 on top and roll the
                    // fuseball's / Mirror's random 250/500/750 a second time.
                    if (is_player_shot && enemy.life <= shot.life
                        && !isArcadeEnemy(enemy.id))
                        engine.init_explosion(time, enemy.life, v, enemy.z + edz * 0.5f,
                                            EXPLOSION_ENEMY, enemy.id,
                                            /*shed_bonus=*/false, /*draw_visual=*/false);
                    // Shot destroyed -> shot explosion (real hits only), else reflect.
                    if (enemy.life >= shot.life) {
                        if (enemy.id != RECT1 && enemy.id != RECT2 && (enemy.mush_flag & 2))
                            engine.init_explosion(time, shot.life, v, enemy.z + edz * 0.5f,
                                                  EXPLOSION_SHOT, shot.id);
                        // else: playsound(reflect) — audio not yet ported.
                    }

                    int ol = enemy.life;                // original enemy life
                    // Player, non-powerup shots: reflector/rect pushback + damage
                    // to the enemy (pushback uses the SHOT's dz).
                    if (is_player_shot && shot.id != POWERUP_SHOT) {
                        if (enemy.id == REFLECTOR1) {
                            float rnd = rand() / (float)RAND_MAX;
                            enemy.z += SHOT_DZ[shot.id] * (1.0f + rnd);
                            engine.init_shot(v, enemy.z, REFLECT_SHOT1);
                        }
                        /* @vfp-exempt R3 — rect pushback window on per-enemy float z with rand; feeds next frame window (C-1). measured n/a. Verified 2026-09-06. */
                        const bool inRectBand = (enemy.z > -2.0f) || (enemy.z < -2.5f);
                        if (enemy.id == RECT1
                            || (enemy.id == RECT2 && inRectBand)) {
                            float rnd = rand() / (float)RAND_MAX;
                            enemy.z += SHOT_DZ[shot.id] * (1.0f + rnd) * 0.42f;
                        }
                        enemy.life -= shot.life;
                        if (enemy.life < 0) enemy.life = 0;
                    }
                    // Shot loses the enemy's ORIGINAL life, gated (all shot types).
                    if ((enemy.mush_flag & 2) || p.shot_id == PLAYER_SHOT1 || rand() % 11 == 0)
                        shot.life -= ol;
                    if (shot.life < 0) shot.life = 0;
                    // ARCADE ONLY (arcade_enemies.md sec 3.5): a confirmed kill
                    // CONSUMES an ordinary claw shot; only the particle laser
                    // punches through. Against the arcade life of 1 a
                    // PLAYER_SHOT1 would otherwise survive at 99 and go on to
                    // kill the next thing in the lane.
                    if (isArcadeEnemy(enemy.id) && enemy.life <= 0 && is_player_shot
                        && arcade_kill_consumes_shot(shot.id))
                        shot.life = 0;
                }
            }

            // =================================================================
            // Enemy-Player Collision (BEFORE Z-movement)
            // =================================================================
            // Check if enemy will cross player position during this frame
            // No animation_jump guard: the reference build (ref build:3287) runs enemy-side player
            // collision even mid-jump — the jump-into-enemy kill lives in move_jump.
            //
            // THE TEST IS PER-SET (audit sec 4/P2, P4). The arcade families own
            // theirs inside update() and are skipped here; the predicate is
            // always true for every classic id. See enemies_shared.h sec 2
            // for why each arcade family answers the way it does (it works the
            // flipper, tanker and spiker cases; the later families follow the
            // same rule) — in particular, with the arcade descriptor dz at 0
            // the window below
            // degenerates to exact float equality, which is ALWAYS TRUE for a
            // flipper parked at the rim, so leaving it shared would fire on
            // the DESTINATION lane mid-flip and silently undo the flipper's
            // verified "it grabs the lane it is flipping OUT OF" correction.
            if (enemy_uses_shared_player_collision(enemy.id)
                && enemy.life > 0 && v == p.grid_element_pos) {
                float dz = ENEMY_DZ[enemy.id];
                // Collision occurs if: z >= player.z AND z+dz <= player.z
                /* @vfp-exempt R3 — enemy-vs-player swept window (ref build:3287): decides death triggers; preserve current (C-10/C-11). measured n/a. Verified 2026-09-06. */
                const bool hitsPlayer = (enemy.z >= p.z) && (enemy.z + dz <= p.z);
                if (hitsPlayer) {
                    // Shooter dodge. the reference build collides when:
                    //   ((not shooter) OR shooter-clause OR leftdx+rightdx>4)
                    // shooter-clause = phase<max*5/6 AND (!shooting OR primary OR coin).
                    // De Morgan of the shooter no-collide: the shooter DODGES when
                    //   (leftdx+rightdx<=4) AND (phase>=max*5/6
                    //                             OR (shooting AND !primary AND coin)).
                    // The old port dropped the phase>=max*5/6 alternative entirely.
                    bool can_collide = true;
                    if (enemy.id == SHOOTER1 || enemy.id == SHOOTER2) {
                        int max_anim = enemy.max_animation;
                        bool shooter_dodges =
                            abs(enemy.animation_phase) >= (max_anim * 5) / 6
                            || (p.is_shooting && p.shot_id != PLAYER_SHOT1 && rand() % 2 != 0);
                        if (p.leftdx + p.rightdx <= 4 && shooter_dodges)
                            can_collide = false;
                    }

                    if (can_collide) {
                        // Enemy hits the player!
                        engine.init_explosion(time, enemy.life, v, enemy.z + dz * 0.5f,
                                            EXPLOSION_PLAYER, 1);
                        engine.init_explosion(time, enemy.life, v, enemy.z + dz * 0.5f,
                                            EXPLOSION_ENEMY, enemy.id);
                        engine.init_gameover(time, 16);
                        return;
                    }
                }
            }

            _apply_movement(engine, v, v2, enemy);

            // Re-check: _apply_movement may have removed the enemy (mushroom fell off)
            if (v2 >= elem.num_enemies) break;

            if (elem.enemies[v2].life <= 0) {
                // A multi-part mushroom splits and SURVIVES: advance past it
                // (the reference build `inc(v2); continue`). Every other death removes the
                // enemy (last swapped into v2), so we reprocess v2 in place.
                bool survived = _handle_death(engine, time, v, v2, elem.enemies[v2]);
                if (survived) v2++;
                continue;
            }

            v2++;
        }
    }
}

// =============================================================================
// AI: Shooter
// =============================================================================

static bool _ai_shooter(GameEngine& engine, int time, int lane, int idx, Enemy& enemy) {
    PlayerInfo& p = engine.player;

    if (enemy.animation_phase == 0) {
        // At the near edge: fast sidestep toward player
        if (enemy.z <= 0.0f && rand() % 50 < enemy.sidestep_freq) {
            // Three-way: lane>pos -> -, lane<pos -> +, EQUAL -> hold (no sidestep).
            // a lane-aligned enemy does not step. The old
            // two-way ternary yielded -1 on equality, forcing a spurious step.
            int dir = (lane > p.grid_element_pos) ? -1 : (lane < p.grid_element_pos) ? 1 : 0;
            if (engine.grid_level_go_round && abs(lane - p.grid_element_pos) > engine.lane_count / 2)
                dir = -dir;
            if (dir != 0) {
                enemy.animation_vec = dir * 2;
                enemy.animation_phase = dir * 2;
            }
        } else {
            // Normal idle behavior
            int max_roll = std::max(1, (26 - enemy.sidestep_freq) * 128);
            int roll = rand() % (max_roll + 1);
            if (roll <= 2) {
                if (engine.grid_level_go_round || lane > 0) {
                    enemy.animation_vec = -1;
                    enemy.animation_phase = -1;
                }
            } else if (roll <= 5) {
                if (engine.grid_level_go_round || lane < engine.lane_count - 2) {
                    enemy.animation_vec = 1;
                    enemy.animation_phase = 1;
                }
            } else if (roll <= 11) {
                // Three-way hold on equality.
                int dir = (lane > p.grid_element_pos) ? -1 : (lane < p.grid_element_pos) ? 1 : 0;
                if (engine.grid_level_go_round && abs(lane - p.grid_element_pos) > engine.lane_count / 2)
                    dir = -dir;
                if (dir != 0) {
                    enemy.animation_vec = dir;
                    enemy.animation_phase = dir;
                }
            }
        }

        // Shooting behavior
        if (rand() % 10000 < enemy.shoot_freq * 5 + (int)enemy.z) {
            float shot_z = (enemy.z > 1.0f) ? enemy.z + ENEMY_DZ[ENEMY_SHOT1] : enemy.z;
            engine.init_shot(lane, shot_z, ENEMY_SHOT1);
        }
    } else {
        // Moving animation in progress
        enemy.animation_phase += enemy.animation_vec;
        if (abs(enemy.animation_phase) >= enemy.max_animation) {
            int step;
            if (abs(enemy.animation_vec) > 1) {
                step = enemy.animation_vec / 2;
            } else {
                step = enemy.animation_vec;
            }
            int target = lane + step;
            if (engine.grid_level_go_round) {
                // R11: one-period bound does NOT hold -- step is animation_vec/2
                // or animation_vec, and animation_vec carries fade-out markers
                // (+100 / -200, see :566/:576), so target can be many periods
                // out and the modulo wrap is required. Mechanics-gated; left as
                // the double-modulo positive-wrap (no profile to justify the
                // 2->1 divide reduction here).
                target = ((target % engine.lane_count) + engine.lane_count) % engine.lane_count;
            } else if (target < 0 || target >= engine.lane_count) {
                enemy.animation_vec = -enemy.animation_vec;
                return false;
            }
            return _transfer_enemy(engine, lane, idx, target);
        }
    }
    return false;
}

// =============================================================================
// AI: Container
// =============================================================================

static void _ai_container(GameEngine& engine, int time, int lane, int idx, Enemy& enemy) {
    if (enemy.z <= 0.0f) {
        // shed_bonus=false: this sets life=0, so the container's single bonus
        // batch is shed by _handle_death. Shedding here too would double the
        // batch on a rim kill (self-destruct or a shot that lands at the rim),
        // breaking the one-shed-per-classic-kill rule the shot-kill sweeps use.
        engine.init_explosion(time, enemy.life / 4, lane, enemy.z,
                              EXPLOSION_ENEMY, enemy.id, false);
        enemy.life = 0;
    }
}

// =============================================================================
// AI: Element Zapper
// =============================================================================

static bool _ai_el_zapper(GameEngine& engine, int time, int lane, int idx, Enemy& enemy) {
    // Initialize oscillation on first tick
    if (enemy.animation_vec == 0) {
        enemy.animation_vec = 1;
        enemy.max_animation = 20 + (25 - enemy.shoot_freq) * 4;
    }

    // Oscillate animation phase
    enemy.animation_phase += enemy.animation_vec;
    if (enemy.animation_phase >= enemy.max_animation) {
        enemy.animation_vec = -1;
    } else if (enemy.animation_phase <= 0) {
        enemy.animation_vec = 1;
    }

    if (enemy.z <= 0.0f) {
        if (enemy.sidestep_freq >= 0) {
            // First arrival: split into left + right copies
            // shed_bonus=false: the split is a visual, not a kill (the zapper
            // survives as the left-mover), so it sheds no bonus here. A
            // rim-timed kill of this zapper sheds its single batch at
            // _handle_death, matching the one-shed-per-classic-kill rule.
            engine.init_explosion(time, enemy.life / 2, lane, enemy.z,
                                  EXPLOSION_ENEMY, enemy.id, false);
            enemy.sidestep_freq = -1; // Mark as left-mover

            // Spawn a right-mover copy
            if (engine.grid[lane].num_enemies < MAX_ENEMIES) {
                Enemy right_copy;
                right_copy.z = enemy.z;
                right_copy.id = enemy.id;
                right_copy.life = enemy.life;
                right_copy.animation_phase = enemy.animation_phase;
                right_copy.animation_vec = enemy.animation_vec;
                right_copy.max_animation = enemy.max_animation;
                right_copy.oo_max_animation = enemy.oo_max_animation;
                right_copy.sidestep_freq = -3; // Right-mover
                right_copy.shoot_freq = enemy.shoot_freq;

                engine.grid[lane].enemies.push_back(right_copy);
                engine.grid[lane].num_enemies += 1;
                engine.enemies_nums[enemy.id] += 1;
            }
        } else {
            // Already split: move sideways every 4th animation frame
            if (enemy.animation_phase % 4 == 0) {
                int direction = enemy.sidestep_freq + 2; // -1 or +1
                int target = lane + direction;
                if (engine.grid_level_go_round) {
                    // R11: one-period bound HOLDS here (direction is +/-1, so
                    // target = lane +/-1 is at most one period out of [0,n));
                    // `if(v<0)v+=n; else if(v>=n)v-=n;` would be an exact
                    // zero-divide identity. Mechanics-gated and not
                    // profile-justified, so left as the modulo form.
                    target = ((target % engine.lane_count) + engine.lane_count) % engine.lane_count;
                } else if (target < 0 || target >= engine.lane_count) {
                    // Bounce off edge
                    enemy.sidestep_freq = -enemy.sidestep_freq - 4;
                    return false;
                }
                return _transfer_enemy(engine, lane, idx, target);
            }
        }
    }
    return false;
}

// =============================================================================
// AI: Mushroom
// =============================================================================

static bool _ai_mushroom(GameEngine& engine, int time, int lane, int idx, Enemy& enemy) {
    PlayerInfo& p = engine.player;

    if (enemy.animation_phase != 0) {
        enemy.animation_phase += enemy.animation_vec;
        if (abs(enemy.animation_phase) >= enemy.max_animation) {
            int target = lane + enemy.animation_vec;
            if (engine.grid_level_go_round) {
                // R11: one-period bound does NOT hold -- the step is
                // animation_vec, which carries fade-out markers (+100/-200),
                // so target can be many periods out; the modulo wrap is
                // required. Mechanics-gated; left as-is.
                target = ((target % engine.lane_count) + engine.lane_count) % engine.lane_count;
            } else if (target < 0 || target >= engine.lane_count) {
                enemy.animation_phase = 0;
                return false;
            }
            return _transfer_enemy(engine, lane, idx, target);
        }
    } else {
        if (enemy.z <= 2.0f
            && rand() % 50 < enemy.sidestep_freq
            && (enemy.mush_flag & 2)) {
            // Three-way hold on equality.
            int dir = (lane > p.grid_element_pos) ? -1 : (lane < p.grid_element_pos) ? 1 : 0;
            if (engine.grid_level_go_round && abs(lane - p.grid_element_pos) > engine.lane_count / 2)
                dir = -dir;
            if (dir != 0) {
                enemy.animation_vec = dir;
                enemy.animation_phase = dir;
            }
        }
    }
    return false;
}

// (AI: Reflector has MOVED to enemies/reflector.cpp -- the worked example for
// the per-family layout described in enemies/enemy_family.h. Nothing about its
// behaviour changed; only where it lives.)

// =============================================================================
// AI: Rectangle
// =============================================================================

static bool _ai_rectangle(GameEngine& engine, int time, int lane, int idx, Enemy& enemy) {
    // Decision-making when settled at near edge
    if (enemy.z < -2.0f && enemy.animation_phase == 0 && enemy.animation_vec == 0) {
        int max_roll = std::max(1, (26 - enemy.sidestep_freq) * 2 + 11);
        int roll = rand() % (max_roll + 1);
        if (roll <= 2) {
            if (enemy.id == RECT2) {
                enemy.animation_vec = -1;
            }
        } else if (roll <= 5) {
            if (enemy.id == RECT2) {
                enemy.animation_vec = 1;
            }
        } else if (roll <= 11) {
            // toward player - not implemented for rects
        } else {
            enemy.animation_phase = 1;
            enemy.max_animation = 33 + (25 - enemy.shoot_freq) * 4;
        }
    }

    // Rect2 duplication movement
    if (enemy.animation_vec != 0 && enemy.id == RECT2) {
        if (enemy.animation_vec > 0 && enemy.animation_vec < 100) {
            // Fading out
            enemy.animation_vec -= 1;
            enemy.z -= 0.085f + ENEMY_DZ[enemy.id];
            if (enemy.animation_vec <= 0) {
                // A shot may have killed this RECT2 (life<=0) on the exact frame
                // its fade-out completes. This dispatch removes it BEFORE the
                // life<=0 death check (enemies.cpp:311), so route a killed rect
                // through _handle_death -- the SAME full treatment the normal
                // death path gives: the 2nd score, the clamped ball, the bonus
                // shed, the powerup cadence, and removal. (Calling the raw
                // record helper here would draw the ball but skip the score,
                // shed, and cadence, AND bypass init_explosion's 300 clamp --
                // ENEMY_LIFE[RECT2]=7500 would render a grossly oversized ball.)
                // A natural fade-out (life>0) is a plain despawn, no death fx.
                if (enemy.life <= 0)
                    _handle_death(engine, time, lane, idx, enemy);
                else
                    _remove_enemy(engine, lane, idx, enemy);
                return true;
            }
        } else if (enemy.animation_vec < 0 && enemy.animation_vec > -200) {
            // Arriving
            enemy.animation_vec += 1;
            enemy.z -= ENEMY_DZ[enemy.id];
            if (enemy.z < -2.75f) {
                enemy.z = -2.75f;
            }
            if (enemy.animation_vec >= 0) {
                enemy.animation_vec = 0;
            }
        } else {
            // Initiate duplication
            int dir = (enemy.animation_vec > 0) ? 1 : -1;
            int target = lane + dir;
            if (engine.grid_level_go_round) {
                // R11: one-period bound HOLDS (dir is +/-1); a compare-wrap
                // would be an exact zero-divide identity. Mechanics-gated and
                // not profile-justified; left as the modulo form.
                target = ((target % engine.lane_count) + engine.lane_count) % engine.lane_count;
            } else if (target < 0 || target >= engine.lane_count) {
                enemy.animation_vec = 0;
                return false;
            }
            // Create duplicate in target lane
            GridElement& target_elem = engine.grid[target];
            if (target_elem.num_enemies < MAX_ENEMIES) {
                Enemy dup;
                dup.z = -7.5f;
                dup.id = enemy.id;
                dup.life = enemy.life;
                dup.animation_vec = -200;
                dup.max_animation = enemy.max_animation;
                dup.oo_max_animation = enemy.oo_max_animation;
                dup.sidestep_freq = enemy.sidestep_freq;
                dup.shoot_freq = enemy.shoot_freq;

                target_elem.enemies.push_back(dup);
                target_elem.num_enemies += 1;
                engine.enemies_nums[enemy.id] += 1;
            }
            enemy.animation_vec = 100; // Start fade-out
        }
    }

    // Attack animation cycle
    if (enemy.animation_phase > 0) {
        enemy.animation_phase += 1;
        if (enemy.animation_phase >= enemy.max_animation * 3) {
            enemy.animation_phase = 0;
        }
    }

    return false;
}

// =============================================================================
// AI: Spiker
// =============================================================================

static bool _ai_spiker(GameEngine& engine, int time, int lane, int idx, Enemy& enemy) {
    GridElement& elem = engine.grid[lane];

    if (enemy.animation_phase == 0) {
        // Approaching: EXTEND the spike, one climb step at a time.
        //
        // The arcade reference (climbspike, see DOCTRINE.md) adds one `spiker_zspeed`
        // increment per frame, and only while the spiker has out-climbed the
        // spike's current tip (`sub bx,cx / jle rrts`). It never assigns an
        // absolute height.
        //
        // This block used to SET elem.spike from the spiker's z, which meant
        // any damage a shot did was restored in full on the very next frame
        // while the spiker was alive -- so a spike could not be shot down at
        // all, whatever the erosion rate. Growing incrementally makes damage
        // stick and forces the spiker to re-climb what it lost.
        float max_spike = GRID_ELEMENT_LENGTH * ((enemy.id == SPIKER1) ? 0.85f : 1.15f);
        float reach = GRID_ELEMENT_LENGTH - enemy.z - ENEMY_DZ[enemy.id] * 20.0f;
        if (reach > elem.spike && elem.spike < max_spike) {
            elem.spike += std::fabs(ENEMY_DZ[enemy.id]);   // one climb step
            if (elem.spike > max_spike) elem.spike = max_spike;
            if (elem.spike > reach)     elem.spike = reach;   // never pass the spiker
        }
        // Retreat trigger
        if (enemy.z < GRID_ELEMENT_LENGTH * 0.666f) {
            enemy.animation_phase = -1;
        }
    } else if (enemy.animation_phase == -1) {
        // Retreating at double speed
        enemy.z -= ENEMY_DZ[enemy.id] * 2.0f;
        if (enemy.z >= GRID_ELEMENT_LENGTH) {
            enemy.z = GRID_ELEMENT_LENGTH;
            enemy.animation_phase = 0;
            // Spiker2 changes lanes on retreat. PROPAGATE the transfer: every
            // other AI does `return _transfer_enemy(...)`, and this one alone
            // discarded it -- after a successful transfer, slot `idx` holds a
            // DIFFERENT enemy (swap-down removal), so `enemy` is stale. Falling
            // through then fired a shot from the wrong lane using the wrong
            // enemy's data, and the caller advanced past the swapped-in enemy,
            // skipping its update for that frame.
            if (enemy.id == SPIKER2) {
                int target = rand() % engine.lane_count;
                if (target != lane) {
                    if (_transfer_enemy(engine, lane, idx, target)) return true;
                }
            }
        }
    }

    // Spikers can also shoot
    if (rand() % 20000 < enemy.shoot_freq * 5 + (int)enemy.z) {
        engine.init_shot(lane, enemy.z, ENEMY_SHOT1);
    }
    return false;
}

// =============================================================================
// AI: Space Zapper
// =============================================================================

static bool _ai_sp_zapper(GameEngine& engine, int time, int lane, int idx, Enemy& enemy) {
    PlayerInfo& p = engine.player;

    if (enemy.animation_phase == 0) {
        int max_roll = std::max(1, (26 - enemy.sidestep_freq) * 32);
        int roll = rand() % (max_roll + 1);
        if (roll <= 4) {
            if (engine.grid_level_go_round || lane > 0) {
                enemy.animation_vec = -1;
                enemy.animation_phase = -1;
            }
        } else if (roll <= 9) {
            if (engine.grid_level_go_round || lane < engine.lane_count - 2) {
                enemy.animation_vec = 1;
                enemy.animation_phase = 1;
            }
        } else if (roll <= 13) {
            // Three-way hold on equality.
            int dir = (lane > p.grid_element_pos) ? -1 : (lane < p.grid_element_pos) ? 1 : 0;
            if (engine.grid_level_go_round && abs(lane - p.grid_element_pos) > engine.lane_count / 2)
                dir = -dir;
            if (dir != 0) {
                enemy.animation_vec = dir;
                enemy.animation_phase = dir;
            }
        }
    } else {
        enemy.animation_phase += enemy.animation_vec;
        if (abs(enemy.animation_phase) >= enemy.max_animation) {
            int target = lane + enemy.animation_vec;
            if (engine.grid_level_go_round) {
                // R11: one-period bound does NOT hold -- the step is
                // animation_vec, which carries fade-out markers (+100/-200),
                // so target can be many periods out; the modulo wrap is
                // required. Mechanics-gated; left as-is.
                target = ((target % engine.lane_count) + engine.lane_count) % engine.lane_count;
            } else if (target < 0 || target >= engine.lane_count) {
                enemy.animation_vec = -enemy.animation_vec;
                return false;
            }
            return _transfer_enemy(engine, lane, idx, target);
        }
    }
    return false;
}

// =============================================================================
// Movement and Death
// =============================================================================

static void _apply_movement(GameEngine& engine, int lane, int idx, Enemy& enemy) {
    // THE ARCADE FAMILIES OWN THEIR OWN Z (audit sec 5/M1, M5). Their
    // descriptor dz is 0 so the step below would be a no-op anyway, but the
    // shared FLOOR would not be: `z < -ENEMY_DZ[id]` is exactly `z < 0` at a
    // zero row, and super-flipper-3's rim promotion deliberately overshoots
    // the rim plane by up to one frame of z -- a real, visible tell that this
    // clamp would erase. One compare, and it is `false` for every classic id.
    if (isArcadeEnemy(enemy.id)) return;

    float dz = ENEMY_DZ[enemy.id];

    // Mushrooms with body part 2 intact move slower. The original does
    // `z += dz; z -= dz*0.6666` => NET dz*0.3334. The port had dz*0.6666 (~2x
    // too fast).
    if (enemy.id == MUSHROOM && (enemy.mush_flag & 2)) {
        dz *= 0.3334f;
    }

    // Skip z movement for rects in special states
    if ((enemy.id == RECT1 || enemy.id == RECT2) && enemy.animation_vec != 0) {
        return;
    }

    float new_z = enemy.z + dz;

    // Clamp to valid range
    if (enemy.id == RECT1 || enemy.id == RECT2) {
        if (new_z < -2.75f && enemy.animation_vec == 0) {
            new_z = -2.75f;
        }
    } else if (enemy.id == MUSHROOM) {
        if (new_z < -7.5f) {
            _remove_enemy(engine, lane, idx, enemy);
            return;
        }
    } else {
        if (new_z < -ENEMY_DZ[enemy.id]) {
            new_z = -ENEMY_DZ[enemy.id];
        }
    }

    enemy.z = new_z;
}

// Returns true iff the enemy SURVIVED (a multi-part mushroom that split off one
// part and lives on). All other outcomes remove the enemy and return false.
static bool _handle_death(GameEngine& engine, int time, int lane, int idx, Enemy& enemy) {
    PlayerInfo& p = engine.player;

    // A multi-part mushroom splits and survives — skip the
    // explosion / score / powerup / removal entirely.
    if (enemy.id == MUSHROOM && _handle_mushroom_split(engine, lane, idx, enemy))
        return true;

    // (MUSHROOM survival handled above.) Snapshot the dying enemy BEFORE any
    // spawn: hatching container children can push_back into this lane's enemy
    // vector and invalidate the `enemy` reference.
    const int   eid   = enemy.id;
    const float ez    = enemy.z;
    const int   elife = enemy.life;

    // Container hatch: two children via init_enemy (FRESH
    // randoms), then z overridden, max_animation halved, sidestep doubled, and
    // animation_phase = +-(max-1) so the pair spreads apart immediately. C1-C3
    // hatch shooters into the same lane; C4 hatches el-zappers into lane+-1.
    if (eid >= CONTAINER1 && eid <= CONTAINER4) {
        bool r0 = (rand() % 2 == 0);
        bool is_c4 = (eid == CONTAINER4);

        // ---- First child (spreads toward +1) ----
        int lane1;
        if (is_c4) {
            lane1 = (!engine.grid_level_go_round && lane + 1 > engine.lane_count - 2)
                    ? engine.lane_count - 2 : (lane + 1) % engine.lane_count;
            engine.init_enemy(lane1, EL_ZAPPER1);
        } else {
            lane1 = lane;
            engine.init_enemy(lane1,
                (eid == CONTAINER3 || (eid == CONTAINER2 && r0)) ? SHOOTER2 : SHOOTER1);
        }
        int ol = _hatch_child_tweak(engine, lane1, ez, is_c4, /*second=*/false, lane, -1);

        // ---- Second child (spreads toward -1) ----
        int lane2;
        if (is_c4) {
            lane2 = (!engine.grid_level_go_round && lane - 1 < 0) ? 0
                  : (engine.grid_level_go_round && lane - 1 < 0) ? engine.lane_count - 1
                  : lane - 1;
            engine.init_enemy(lane2, EL_ZAPPER1);
        } else {
            lane2 = lane;
            engine.init_enemy(lane2,
                (eid == CONTAINER3 || (eid == CONTAINER2 && !r0)) ? SHOOTER2 : SHOOTER1);
        }
        // The second C4 el-zapper inherits the first child's halved max (ol).
        _hatch_child_tweak(engine, lane2, ez, is_c4, /*second=*/true, lane, is_c4 ? ol : -1);
    }

    // Space zapper kill bonus
    if (eid == SP_ZAPPER1) {
        p.killed_zappers += 1;
        int bonus_score = (int)round(250.0 * p.killed_zappers * p.multiplier);
        engine.award_score(time, bonus_score);
        engine.init_score(bonus_score, 0.5f * 1.3333f, 0.5f, 0.5f, 1.0f, 0.5f);
    }

    // Explosion (snapshot values — `enemy` may be stale after a hatch).
    //
    // SCORE (audit sec 6/D4, arcade_enemies.md sec 3.6): `energy` IS the score
    // init_explosion pays, and the arcade roster's life is 1 by design, so an
    // arcade kill would be worth ONE POINT. The arcade ids pay their own
    // score instead (flipper 150, tanker 100, spiker 150 — enemies_shared.h);
    // every classic id keeps `enemy.life` exactly as before.
    int ex_energy = isArcadeEnemy(eid) ? arcade_kill_score(eid)
                                    : ((elife > 0) ? elife : ENEMY_LIFE[eid]);
    engine.init_explosion(time, ex_energy, lane, ez, EXPLOSION_ENEMY, eid);

    // Powerup drop on the reference's banded schedule (constants.h).
    if (engine.note_powerup_kill()) {
        engine.init_shot(lane, ez, POWERUP_SHOT);
        engine.show_powerup_text(time, -1); // "collect powerup"
    }

    // THE ARCADE CUSTOM CORPSE (arcade_tanker.h registration item 2) — a shot
    // tanker SPLITS. Placed here, after the explosion and the capsule
    // cadence and immediately before removal, so that this path performs
    // exactly what ArcadeTanker's own landing path performs in exactly the same
    // order: "shot and landed are identical — letting a tanker land releases
    // exactly what shooting it does; shooting only buys distance from the
    // rim." Anywhere earlier (e.g. beside the container hatch) and the two
    // paths would spawn the children before the explosion instead of after,
    // which is a different draw order AND a different rand() order.
    //
    // The reference is re-fetched rather than reusing `enemy` for the same
    // reason the removal below does: a hatch may have pushed into this lane.
    if (isArcadeEnemy(eid)) {
        const enemyfam::EnemyCtx dctx{ time, engine.lane_count,
                                       engine.grid_level_go_round };
        arcade_enemy_die(engine, dctx, lane, idx, engine.grid[lane].enemies[idx]);
    }

    // Re-fetch: container hatching may have reallocated this lane's vector.
    _remove_enemy(engine, lane, idx, engine.grid[lane].enemies[idx]);
    return false;
}

// Post-hatch tweak of a container child that init_enemy just appended to
// child_lane. init_enemy already rolled FRESH randoms for
// this child; here we override z, halve its max_animation (or force it for the
// second el-zapper so both zappers share the same cadence), double its
// sidestep, and preset animation_phase = +-(max-1) so the pair spreads apart on
// the first frame. Returns the resulting max_animation (so the first child's
// halved value can be forced onto the second). is_zapper children (C4) keep
// their fresh phase/vec — only their z and max are constrained.
static int _hatch_child_tweak(GameEngine& engine, int child_lane, float z,
                              bool is_zapper, bool second, int parent_lane, int forced_max) {
    GridElement& e = engine.grid[child_lane];
    if (e.num_enemies == 0) return 0;
    Enemy& c = e.enemies[e.num_enemies - 1];
    c.z = z;
    int m = (forced_max >= 0) ? forced_max : c.max_animation / 2;
    if (m < 1) m = 1;
    c.max_animation = m;
    if (!is_zapper) {
        c.sidestep_freq = std::min(25, c.sidestep_freq * 2);
        if (!second) {
            c.animation_phase = c.max_animation - 1;
            c.animation_vec = (!engine.grid_level_go_round && parent_lane >= engine.lane_count - 2) ? -1 : 1;
        } else {
            c.animation_phase = -(c.max_animation - 1);
            c.animation_vec = (!engine.grid_level_go_round && parent_lane <= 0) ? 1 : -1;
        }
    }
    c.oo_max_animation = 1.0f / c.max_animation;
    return m;
}

// A multi-part mushroom sheds ONE part into a new fragment
// and the PARENT lives on (life restored, pushed back). Returns true when it
// split (parent survives); false for a single-part mushroom (dies normally).
static bool _handle_mushroom_split(GameEngine& engine, int lane, int idx, Enemy& enemy) {
    // Single-part mushroom: nothing to shed -> normal death.
    if (enemy.mush_flag == 1 || enemy.mush_flag == 2 ||
        enemy.mush_flag == 4 || enemy.mush_flag == 8) {
        return false;
    }

    GridElement& elem = engine.grid[lane];
    if (elem.num_enemies >= MAX_ENEMIES) return false;   // no room -> die normally

    // Which part splits off: check order 4, then 1, then 8.
    int split_bit = 0;
    int check_bits[] = {4, 1, 8};
    for (int i = 0; i < 3; i++) {
        if (enemy.mush_flag & check_bits[i]) {
            split_bit = check_bits[i];
            break;
        }
    }
    if (split_bit == 0) return false;

    // The new fragment is a full COPY of the parent — it KEEPS the
    // parent's current z — carrying only the split-off part with high life. The
    // old port wrongly pushed the FRAGMENT back and left the parent in place.
    Enemy fragment = enemy;
    fragment.life = 11250;
    fragment.mush_flag = split_bit;

    elem.enemies.push_back(fragment);   // may reallocate -> `enemy` ref is now stale
    elem.num_enemies += 1;
    engine.enemies_nums[MUSHROOM] += 1;

    // The PARENT survives: life restored, one fewer part, pushed back toward the
    // throat (ENEMY_DZ<0, so z-=dz*20 increases z). Re-fetch after the push_back.
    Enemy& parent = elem.enemies[idx];
    parent.mush_flag &= ~split_bit;
    parent.life = ENEMY_LIFE[MUSHROOM];
    parent.z -= ENEMY_DZ[MUSHROOM] * 20.0f;
    return true;
}

// =============================================================================
// Embryo System
// =============================================================================

void move_embrios(GameEngine& engine, int time) {
    if (engine.player.gameover_animation > 0) return;

    for (int v = 0; v < engine.lane_count; v++) {
        GridElement& elem = engine.grid[v];
        int v2 = 0;
        while (v2 < elem.num_embrios) {
            Enemy& embryo = elem.embrios[v2];
            embryo.z += ENEMY_DZ[embryo.id] * 0.5f;

            if (embryo.z <= GRID_ELEMENT_LENGTH) {
                // Hatch: spawn real enemy
                engine.init_enemy(v, embryo.id);
                engine.init_explosion(
                    time, ENEMY_LIFE[embryo.id], v,
                    GRID_ELEMENT_LENGTH, EXPLOSION_EMBRYO, embryo.id
                );
                // Remove embryo (swap with last)
                engine.embrios_nums[embryo.id] -= 1;
                elem.num_embrios -= 1;
                if (v2 < elem.num_embrios) {
                    elem.embrios[v2] = elem.embrios[elem.num_embrios];
                }
                if (!elem.embrios.empty()) {
                    elem.embrios.pop_back();
                }
            } else {
                v2++;
            }
        }
    }
}

// =============================================================================
// Utility Functions
// =============================================================================

static bool _transfer_enemy(GameEngine& engine, int from_lane, int idx, int to_lane) {
    GridElement& from_elem = engine.grid[from_lane];
    GridElement& to_elem = engine.grid[to_lane];

    if (to_elem.num_enemies >= MAX_ENEMIES) {
        from_elem.enemies[idx].animation_vec = -from_elem.enemies[idx].animation_vec;
        return false;
    }

    Enemy enemy = from_elem.enemies[idx];
    enemy.animation_phase = -enemy.animation_phase;  // Reverse animation (the reference build: animation_phase:=-animation_phase)
    to_elem.enemies.push_back(enemy);
    to_elem.num_enemies += 1;

    // Remove from source (swap with last)
    from_elem.num_enemies -= 1;
    if (idx < from_elem.num_enemies) {
        from_elem.enemies[idx] = from_elem.enemies[from_elem.num_enemies];
    }
    from_elem.enemies.pop_back();

    return true;
}

static void _remove_enemy(GameEngine& engine, int lane, int idx, Enemy& enemy) {
    engine.enemies_nums[enemy.id] -= 1;
    GridElement& elem = engine.grid[lane];
    elem.num_enemies -= 1;
    if (idx < elem.num_enemies) {
        elem.enemies[idx] = elem.enemies[elem.num_enemies];
    }
    if (!elem.enemies.empty()) {
        elem.enemies.pop_back();
    }
}

} // namespace ts
