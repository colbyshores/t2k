#include "weapons.h"
#include "engine.h"
#include "constants.h"
#include "models.h"
#include "enemies/enemies_shared.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace ts {

// =============================================================================
// move_tremor
// =============================================================================

void move_tremor(GameEngine& engine, int time) {
    PlayerInfo& p = engine.player;
    if (p.gameover_animation > 0) return;

    if (p.animation_tremor == 0) {
        engine.tremor_strength = engine.base_tremor_strength;
        return;
    }

    // the reference source:3883-3887: at frame 30, three descending snd_boom hits fire.
    // Audio not yet ported (see collision.cpp reflect-sound TODOs).

    int t = p.animation_tremor;
    if (t < 30) {
        // Phase 1: Charge-up
        float val = (t - 30) * 0.2f / 30.0f;
        engine.tremor_strength = 0.04f - val * val;
    } else if (t < 100) {
        // Phase 2: Shockwave
        engine.tremor_strength = 0.275f - sqrtf((t - 30) * 0.075625f / 70.0f);
    } else {
        // Phase 3: Decay — float-suffixed envelope (1x/tick, not per-element).
        // NOTE: sinf keeps single-precision throughout; envelope feeds only
        // tremor_strength scalar, never rand/score streams.
        /* @vfp-exempt R6 — tremor decay sinf envelope: 1x/tick scalar in Phase 3 of a rare 180-tick cycle; no fastExp/fastPow involved. measured n/a. Verified 2026-09-06. */
        engine.tremor_strength = (float)(
            sinf((t - 100) * (float)M_PI / 80.0f)
            * (180 - t) * 0.02f / 80.0f
        );
    }

    engine.tremor_strength += engine.base_tremor_strength;

    // Kill zone during shockwave phase (frames 31-99)
    /* @vfp-exempt R3 — tremor kill-zone window: per-element float z-range over live rosters; reference-ported window, comparison is the kill predicate. measured n/a. Verified 2026-09-06. */
    if (t >= 31 && t <= 99) {
        float z_min = (t - 30 - 15) * GRID_ELEMENT_LENGTH / 70.0f;
        float z_max = (t - 30) * GRID_ELEMENT_LENGTH / 70.0f;

        for (int v = 0; v < engine.lane_count; v++) {
            GridElement& elem = engine.grid[v];

            /* @vfp-exempt R3 — tremor enemy kill branch (pins the zappable/range if below; split_loops segments start at the loop head). measured n/a. Verified 2026-09-06. */
            // Destroy enemies in the kill zone (12.5% chance)
            int i = 0;
            /* @vfp-exempt R3 — tremor enemy kill loop (pins the z-window branch below): per-enemy float range over live roster. measured n/a. Verified 2026-09-06. */
            /* @vfp-exempt R3 — enemy kill branch island (pins the if below; split_loops emits a bare-if segment here). measured n/a. Verified 2026-09-06. */
            while (i < elem.num_enemies) {
                /* @vfp-exempt R3 — enemy ref-bind island (pins the zappable/range if below; segment starts at the ref-bind). measured n/a. Verified 2026-09-06. */
                Enemy& enemy = elem.enemies[i];
                // The shockwave is a DAMAGE SITE and needs a vulnerability
                // window (audit sec 9/Z4): in the reference the arcade arrival
                // dot has no collision test of any kind, and a tremor would
                // otherwise destroy it. Tested FIRST so an invulnerable enemy
                // does not even consume the roll; always true for every classic
                // id, so the rand() stream is untouched for the shipped roster.
                //
                // enemy_ZAPPABLE, not enemy_shootable: the tremor is an AREA
                // weapon and is treated as the superzapper is. That distinction
                // exists for the fuseball, which is unshootable ~78% of its
                // life and NEVER lands and NEVER expires -- see
                // enemies_shared.h, where the split and the level hang it
                // prevents are written out.
                /* @vfp-exempt R3 — zappable/range branch: tremor kill predicate over per-enemy float z; reference-ported window, rand roll preserved. measured n/a. Verified 2026-09-06. */
                const bool inRange = (enemy.z >= z_min) && (enemy.z <= z_max);
                if (enemy_zappable(enemy)
                    && rand() % 8 == 0 && inRange) {
                    // Randomized energy bonus (the reference source:3903): life + (rnd(life)+life)/20.
                    int ebonus = enemy.life > 0
                        ? (rand() % enemy.life + enemy.life) / 20 : 0;
                    // ARCADE: pay the enemy's OWN score, not its life. Arcade
                    // life is 1 by design, so this site used to pay a fuseball
                    // ONE POINT where a bullet pays it 250-750 -- the same
                    // audit sec 6/D4 defect the shot death path already routes
                    // around, at the fourth damage site. It matters more here
                    // than it looks: with the seam split above, the tremor and
                    // the zapper are the ONLY way to kill a parked fuseball, so
                    // this is the COMMON case for that enemy rather than an
                    // edge one. Called once per corpse, which is what the two
                    // random-score rows require.
                    const int e_energy = isArcadeEnemy(enemy.id)
                                       ? arcade_kill_score(enemy.id)
                                       : enemy.life + ebonus;
                    engine.init_explosion(
                        time, e_energy, v, enemy.z,
                        EXPLOSION_ENEMY, enemy.id
                    );
                    engine.enemies_nums[enemy.id] -= 1;
                    elem.num_enemies -= 1;
                    if (i < elem.num_enemies) {
                        elem.enemies[i] = elem.enemies[elem.num_enemies];
                    }
                    elem.enemies.pop_back();
                } else {
                    i++;
                }
            }

            // Destroy enemy shots in the kill zone
            i = 0;
            /* @vfp-exempt R3 — tremor shot kill loop (pins the z-window branch below): per-shot float range the reference source:3910. measured n/a. Verified 2026-09-06. */
            while (i < elem.num_shots) {
                Shot& shot = elem.shots[i];
                /* @vfp-exempt R3 — tremor shot kill-zone window: per-shot float z-range; reference-ported the reference source:3910 window. measured n/a. Verified 2026-09-06. */
                if (shot.id >= ENEMY_SHOT1
                    && rand() % 8 == 0
                    && shot.z >= z_min && shot.z <= z_max) {
                    // Randomized energy bonus (the reference source:3910): life + (rnd(life)+life)/12.
                    int sbonus = shot.life > 0
                        ? (rand() % shot.life + shot.life) / 12 : 0;
                    engine.init_explosion(
                        time, shot.life + sbonus, v, shot.z,
                        EXPLOSION_SHOT, shot.id
                    );
                    engine.shots_nums[shot.id] -= 1;
                    elem.num_shots -= 1;
                    if (i < elem.num_shots) {
                        elem.shots[i] = elem.shots[elem.num_shots];
                    }
                    elem.shots.pop_back();
                } else {
                    i++;
                }
            }
        }
    }

    p.animation_tremor += 1;
    if (p.animation_tremor > 180) {
        p.animation_tremor = 0;
        p.has_tremor = false;
    }
}

// =============================================================================
// move_zapper -- THE SERIAL SCREEN-CLEAR SUPER ZAPPER (user request 2026-09-16)
// =============================================================================
//
// The super zapper clears the WHOLE SCREEN, but serially -- one bolt per beat,
// never all at once. It fires from the claw at the nearest zappable enemy and
// KILLS it outright, then re-scans for the next nearest zappable enemy and arcs
// a fresh bolt to it, repeating until no zappable enemy remains on screen. Each
// beat is TRAVEL (the bolt visibly races to the target) then IMPACT DWELL (it
// sits on the kill), and the kill lands at the impact through the SAME
// init_explosion / arcade_enemy_die / counter plumbing the continuous beam used,
// so scoring, the custom arcade corpse, and the "a zapped corpse never drops a
// capsule" rule are unchanged.
//
// The chain state lives on the engine (GameEngine::ZapperChain): a sliding
// window of struck points the renderer strokes as a comet tail, plus the current
// target reference. The renderer (line_geometry.cpp buildZapper) is unchanged --
// it only reads pts / pt_tick / front.
//
// ---- WHAT THIS IS, AGAINST THE OLD BEAM (deliberate, user-requested) --------
//  * EVERY BOLT KILLS. The old beam chipped ZAPPER_STRENGTH per tick and ground
//    a high-life enemy down over frames. Here each bolt is lethal on impact, so
//    the cascade destroys everything it can reach regardless of life.
//  * UNTIL THE SCREEN IS CLEAR. The target list is not fixed at fire time; the
//    bolt re-scans to the nearest remaining zappable enemy after every kill, so
//    the cascade continues until nothing zappable is left (bounded by a hard cap).
//  * STILL ONLY WHAT THE ZAPPER MAY HIT. "Everything" means everything the
//    superzapper is allowed to damage -- enemy_zappable(). The Mirror stays
//    immune and inbound arrival dots stay inert; those are hard fidelity rules,
//    not oversights. Each kill also clears the nearest enemy shot in the struck
//    lane (the defensive tool the old beam's shot-lock provided).
//  * The RNG stream and the per-tick damage timing differ from the beam, so the
//    demo/audit baselines shift and must be re-blessed.
//
// The PULSAR is deliberately NOT part of this cascade: its electric attack is
// lane-only (it electrifies the tube it travels down and kills the player there),
// so it gets no chaining bolts. See buildPulsarLaneBolts -- a single lane bolt,
// no arcs to other lanes.
//
// ---- TIMING (ticks @ 16 ms; presentation cadence, free under DOCTRINE.md) ----
// A beat is TRAVEL + DWELL ~= 9 ticks (~145 ms), so clearing a dozen enemies
// reads as a fast ~1.7 s cascade. The dwell is where the kill flash lands.
namespace {
constexpr int ZAP_TRAVEL_TICKS = 6;
constexpr int ZAP_DWELL_TICKS  = 3;
constexpr int ZAP_FADE_TICKS   = 8;   // tail: keep drawing while the last bolt fades out
constexpr int ZAP_HARD_CAP     = 1000; // safety net against a spawn that outruns the clear
}

// Resolve a chain reference to a live enemy index (see the body).
static int _resolve_chain_link(const GridElement& elem, int id, float ref_z) {
    // The zappable enemy in `elem` with id `id` whose z is nearest `ref_z`, or
    // -1 if none (killed by another source, or became un-zappable). The chain
    // stores (lane, id, captured-z) rather than an index precisely so a
    // swap-down from an unrelated kill cannot strand the chain on the wrong
    // enemy -- hence the re-resolve by nearest captured-z on every use.
    int best = -1;
    float best_d = 1.0e30f;
    for (int i = 0; i < elem.num_enemies; i++) {
        const Enemy& e = elem.enemies[i];
        if (e.id != id) continue;
        if (!enemy_zappable(e)) continue;
        float d = fabsf(e.z - ref_z);
        /* @vfp-exempt R3 — chain resolve: loop-carried minimum over |z-ref_z|; integerizing flips which same-id enemy the reference binds to after a swap-down. measured n/a. Verified 2026-09-16. */
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

// Draw-space position (z negated) of a chain reference's live target. Fills
// out[3]; returns false if the target is gone.
static bool _chain_target_draw(GameEngine& engine, int lane, int id, float ref_z,
                              float out[3]) {
    if (lane < 0 || lane >= (int)engine.grid.size()) return false;
    const GridElement& elem = engine.grid[lane];
    int idx = _resolve_chain_link(elem, id, ref_z);
    if (idx < 0) return false;
    const Vec3 mp = engine.grid_level_pos[lane];
    out[0] = mp.x; out[1] = mp.y; out[2] = -elem.enemies[idx].z;
    return true;
}

// The bolt's discharge also burns away the enemy shot nearest the impact (body).
static void _zap_clear_lane_shot(GameEngine& engine, int time, int lane, float ref_z) {
    // The chain's replacement for the old beam's shot lock, and the defensive
    // tool that carried (user request 2026-09-16): one shot per impact, the one
    // closest to where the bolt landed.
    if (lane < 0 || lane >= (int)engine.grid.size()) return;
    GridElement& elem = engine.grid[lane];
    int best = -1;
    float best_d = 1.0e30f;
    for (int i = 0; i < elem.num_shots; i++) {
        const Shot& shot = elem.shots[i];
        if (shot.id < ENEMY_SHOT1) continue;
        float d = fabsf(shot.z - ref_z);
        /* @vfp-exempt R3 — chain shot-lock: loop-carried minimum over |shot.z-impact_z|; integerizing flips which shot the discharge clears. measured n/a. Verified 2026-09-16. */
        if (d < best_d) { best_d = d; best = i; }
    }
    if (best < 0) return;
    Shot& shot = elem.shots[best];
    engine.init_explosion(time,
        (rand() % ZAPPER_STRENGTH + ZAPPER_STRENGTH) * 6,
        lane, shot.z, EXPLOSION_SHOT, shot.id);
    engine.shots_nums[shot.id] -= 1;
    elem.num_shots -= 1;
    if (best < elem.num_shots) {
        elem.shots[best] = elem.shots[elem.num_shots];
    }
    elem.shots.pop_back();
}

// Append a struck point to the sliding window, dropping the oldest when full.
// Runs once per kill (per beat), not per tick, so the shift is COLD.
static void _chain_push(GameEngine::ZapperChain& ch,
                       float x, float y, float z, int tick) {
    using Chain = GameEngine::ZapperChain;
    if (ch.pt_count >= Chain::WINDOW) {
        for (int i = 0; i + 1 < Chain::WINDOW; i++) {
            ch.pts[i][0] = ch.pts[i + 1][0];
            ch.pts[i][1] = ch.pts[i + 1][1];
            ch.pts[i][2] = ch.pts[i + 1][2];
            ch.pt_tick[i] = ch.pt_tick[i + 1];
        }
        ch.pt_count = Chain::WINDOW - 1;
    }
    ch.pts[ch.pt_count][0] = x;
    ch.pts[ch.pt_count][1] = y;
    ch.pts[ch.pt_count][2] = z;
    ch.pt_tick[ch.pt_count] = tick;
    ch.pt_count++;
}

// The nearest zappable enemy to a WORLD-space point (fx, fy, fz), written to the
// out refs; false if none remain. This is what makes the cascade run until the
// screen is clear: called after every kill, it hands the bolt its next victim.
// Squared distance only (no sqrt), and it runs once per beat -- COLD.
static bool _find_nearest_zappable(GameEngine& engine,
                                  float fx, float fy, float fz,
                                  int& out_lane, int& out_id, float& out_z) {
    int   best_lane = -1, best_id = 0;
    float best_z = 0.0f, best_d = 1.0e30f;
    /* @vfp-exempt R3 — cascade scan outer loop (pins the lane/ref-bind segment below; split_loops anchors the segment at the loop head): per-lane walk over the live roster to find the next bolt target. measured n/a. Verified 2026-09-16. */
    for (int v = 0; v < engine.lane_count && v < (int)engine.grid.size(); v++) {
        const GridElement& elem = engine.grid[v];
        const Vec3 mp = engine.grid_level_pos[v];
        for (int i = 0; i < elem.num_enemies; i++) {
            const Enemy& e = elem.enemies[i];
            if (!enemy_zappable(e)) continue;
            float dx = mp.x - fx, dy = mp.y - fy, dz = e.z - fz;
            float d = dx * dx + dy * dy + dz * dz;
            /* @vfp-exempt R3 — cascade target select: loop-carried minimum over squared 3-D distance; integerizing flips which enemy the next bolt takes. measured n/a. Verified 2026-09-16. */
            if (d < best_d) { best_d = d; best_lane = v; best_id = e.id; best_z = e.z; }
        }
    }
    if (best_lane < 0) return false;
    out_lane = best_lane; out_id = best_id; out_z = best_z;
    return true;
}

// Apply one LETHAL chain impact: the bolt kills the resolved target outright,
// regardless of its life. Mirrors the continuous beam's lethal arm exactly --
// explosion + custom arcade corpse + counters + swap-down removal -- so scoring
// and the capsule-cadence exclusion are unchanged. The rand() draw is the same
// shape as the beam's kill, so a kill pays the same energy roll.
static void _zap_kill(GameEngine& engine, int time, int lane, int id, float ref_z) {
    if (lane < 0 || lane >= (int)engine.grid.size()) return;
    GridElement& elem = engine.grid[lane];
    int idx = _resolve_chain_link(elem, id, ref_z);
    if (idx < 0) return;   // already gone: nothing to strike
    Enemy& enemy = elem.enemies[idx];
    engine.init_explosion(time,
        (rand() % ZAPPER_STRENGTH + ZAPPER_STRENGTH) * 12,
        lane, enemy.z, EXPLOSION_ENEMY, enemy.id);
    // THE ARCADE CUSTOM CORPSE (arcade_tanker.h registration item 3): a ZAPPED
    // tanker splits too. Must precede the inline removal or `enemy` is stale. The
    // (deliberately absent) capsule cadence keeps "a zapped corpse never drops a
    // capsule" true (audit Z2).
    if (isArcadeEnemy(enemy.id)) {
        const enemyfam::EnemyCtx zctx{ time, engine.lane_count,
                                       engine.grid_level_go_round };
        arcade_enemy_die(engine, zctx, lane, idx, enemy);
    }
    engine.enemies_nums[enemy.id] -= 1;
    elem.num_enemies -= 1;
    if (idx < elem.num_enemies) {
        elem.enemies[idx] = elem.enemies[elem.num_enemies];
    }
    elem.enemies.pop_back();
    // The discharge clears the nearest enemy shot in the struck lane too, at the
    // impact depth. Runs after the enemy removal above (shots are an independent
    // array, so the swap-down there does not disturb this).
    _zap_clear_lane_shot(engine, time, lane, ref_z);
}

// End the zap: stop the voice, pay the stock, clear the single-use flag, and
// drop the renderer gate.
static void _zapper_end(GameEngine& engine) {
    PlayerInfo& p = engine.player;
    engine.zapper_chain.active = false;
    engine.zapper_chain.phase = 0;
    p.animation_zapp = 0;
    engine.zapper_target_found = false;
    engine.sfx.push(SfxId::THUNDER, SfxAction::LOOP_STOP);
    engine.zapper_loop_active = false;
    if (p.fake_zapp) {
        p.fake_zapp = false;   // free zappers (ladder surprise/warp slots) cost no stock
    } else if (p.zapp_stock > 0) {
        p.zapp_stock -= 1;
    }
    p.zapp_single = false;
}

void move_zapper(GameEngine& engine, int time) {
    PlayerInfo& p = engine.player;
    if (p.gameover_animation > 0 || p.animation_zapp == 0) return;

    using Chain = GameEngine::ZapperChain;
    Chain& ch = engine.zapper_chain;

    // ---- START: light the voice and aim the first bolt. --------------------
    if (!ch.active) {
        const Vec3 pp = engine.grid_level_pos[p.grid_element_pos];
        _chain_push(ch, pp.x, pp.y, -p.z, time);   // pts[0] = the claw at fire
        ch.active = true;

        engine.sfx.push(SfxId::THUNDER, SfxAction::LOOP_START);
        engine.zapper_loop_active = true;

        int lane, id; float z;
        if (_find_nearest_zappable(engine, pp.x, pp.y, p.z, lane, id, z)) {
            ch.cur_lane = lane; ch.cur_id = id; ch.cur_z = z; ch.have_cur = true;
            engine.zapper_target_found = true;
            ch.phase = 1;   // TRAVEL to the first target
            ch.timer = 0;
        } else {
            // Nothing to zap: a bolt straight down the player's lane to the far
            // end, held for the fade, then done. The far end is pushed as a
            // struck point so the renderer fades it like a completed link rather
            // than a head that snaps off.
            ch.have_cur = false;
            engine.zapper_target_found = false;
            _chain_push(ch, pp.x, pp.y, -GRID_ELEMENT_LENGTH, time);
            ch.front[0] = pp.x; ch.front[1] = pp.y; ch.front[2] = -GRID_ELEMENT_LENGTH;
            ch.phase = 3;   // FADE
            ch.timer = 0;
        }
    }

    // ---- TRAVEL: the leading edge homes to the current target. ------------
    if (ch.phase == 1) {
        ch.timer++;
        const float* src = ch.pts[ch.pt_count - 1];
        float tp[3];
        const bool have = _chain_target_draw(engine, ch.cur_lane, ch.cur_id,
                                           ch.cur_z, tp);
        // If the target vanished mid-flight, home to where it was captured so the
        // bolt still lands somewhere sensible before the cascade moves on.
        if (!have) {
            const Vec3 mp = (ch.cur_lane >= 0 && ch.cur_lane < (int)engine.grid.size())
                ? engine.grid_level_pos[ch.cur_lane]
                : Vec3{src[0], src[1], src[2]};
            tp[0] = mp.x; tp[1] = mp.y; tp[2] = -ch.cur_z;
        }
        float t = (float)ch.timer / (float)ZAP_TRAVEL_TICKS;
        if (t > 1.0f) t = 1.0f;
        ch.front[0] = src[0] + (tp[0] - src[0]) * t;
        ch.front[1] = src[1] + (tp[1] - src[1]) * t;
        ch.front[2] = src[2] + (tp[2] - src[2]) * t;
        if (ch.timer >= ZAP_TRAVEL_TICKS) {
            if (have) {
                _zap_kill(engine, time, ch.cur_lane, ch.cur_id, ch.cur_z);
                _chain_push(ch, tp[0], tp[1], tp[2], time);   // record the kill
            }
            ch.phase = 2;   // DWELL on the kill (or on the empty landing)
            ch.timer = 0;
        }
    }
    // ---- IMPACT DWELL: sit on the kill, then aim the next bolt. -----------
    else if (ch.phase == 2) {
        ch.timer++;
        const float* last = ch.pts[ch.pt_count - 1];
        ch.front[0] = last[0]; ch.front[1] = last[1]; ch.front[2] = last[2];
        if (ch.timer >= ZAP_DWELL_TICKS) {
            // Re-scan from the last kill (world z = -draw z) for the next victim.
            int lane, id; float z;
            if (_find_nearest_zappable(engine, last[0], last[1], -last[2], lane, id, z)) {
                ch.cur_lane = lane; ch.cur_id = id; ch.cur_z = z; ch.have_cur = true;
                ch.phase = 1;   // fire the next bolt
                ch.timer = 0;
            } else {
                ch.phase = 3;   // screen clear: FADE
                ch.timer = 0;
            }
        }
    }
    // ---- FADE TAIL: the cascade is done; the bolt fades out, then ends. ---
    else if (ch.phase == 3) {
        ch.timer++;
        const float* last = ch.pts[ch.pt_count - 1];
        ch.front[0] = last[0]; ch.front[1] = last[1]; ch.front[2] = last[2];
        if (ch.timer >= ZAP_FADE_TICKS) {
            _zapper_end(engine);
            return;
        }
    }
    // ---- IDLE (should not persist): end cleanly. --------------------------
    else {
        _zapper_end(engine);
        return;
    }

    // Keep the beam's age counter alive for the camera shake and the renderer
    // gate; the cascade bounds its own duration, this is the outer safety net.
    p.animation_zapp += 1;
    if (p.animation_zapp > ZAP_HARD_CAP) _zapper_end(engine);
}

// =============================================================================
// move_ai_droid — the arcade reference's `rundroid` (see DOCTRINE.md)
// =============================================================================
//
// ONE tumbling cube that patrols the rim hunting the nearest enemy. Faithful to
// the arcade reference rather than to this engine's own original companion, which was a cube surrounded by six
// orbiting pyramids (removed -- see DOCTRINE.md "Intentional deviations").
//
// The arcade reference's droid does exactly three things per frame:
//   1. cycles its colour through droid_cols every 16 frames,
//   2. steps toward the nearest enemy's lane, one lane per 16-frame glide,
//   3. fires every 6 frames off its own shot pool.
// The colour cycle and the tumble are presentation and live in the renderer;
// this is the simulation half.

// The arcade reference's `lor` (see DOCTRINE.md): which way to walk to reach `target`.
// Returns -1 = already there / no target, 0 = left, 1 = right. On a closed web
// it takes the SHORTER way round (the `web_max`/2 comparison); on an open one it
// simply compares indices.
static int _droid_lor(const GameEngine& engine, int from, int target) {
    if (target < 0 || target == from) return -1;
    const int n = engine.lane_count;
    if (!engine.grid_level_go_round) return (from < target) ? 1 : 0;
    int diff = target - from;
    if (diff < 0) diff += n;
    // diff is now the distance walking RIGHT; take it only if it is the shorter
    // half, else walk left.
    return (diff <= n / 2) ? 1 : 0;
}

void move_ai_droid(GameEngine& engine) {
    PlayerInfo& p = engine.player;
    if (!engine.ai_droid || p.gameover_animation > 0
        || p.init_animation > 50 || p.z >= GRID_ELEMENT_LENGTH) {
        return;
    }
    const int n = engine.lane_count;
    if (n <= 0 || (int)engine.grid_level_pos.size() < n) return;

    if (engine.ai_lane >= n) engine.ai_lane = n - 1;
    if (engine.ai_lane < 0)  engine.ai_lane = 0;

    // ---- Fire ---------------------------------------------------------------
    // `sub [BYTE droidel],1 / jns nodfire` -- fires on the frame the counter
    // goes negative, then reloads. Its own pool, so it never starves the player.
    if (--engine.ai_fire < 0) {
        engine.ai_fire = AI_DROID_FIRE_PERIOD;
        engine.init_shot(engine.ai_lane, -AI_DROID_Z, AI_DROID_SHOT);
    }

    // ---- Move ---------------------------------------------------------------
    if (engine.ai_move > 0) {
        // Mid-glide: position is INTERPOLATED from the step's endpoints on an
        // eased curve, not integrated from a fixed velocity. Endpoints stay
        // exact, so the droid still lands dead on the lane's shot-spawn point.
        --engine.ai_move;
        const float t = (float)(AI_DROID_MOVE_FRAMES - engine.ai_move)
                      / (float)AI_DROID_MOVE_FRAMES;
        const float s = aiDroidEase(t);
        engine.ai_x = engine.ai_x0 + (engine.ai_x1 - engine.ai_x0) * s;
        engine.ai_y = engine.ai_y0 + (engine.ai_y1 - engine.ai_y0) * s;
        // Commit the lane index at the halfway point, not on arrival.
        if (engine.ai_move == AI_DROID_COMMIT_FRAME) engine.ai_lane = engine.ai_next_lane;
        return;
    }

    // Parked: pick the nearest enemy's lane and take one step toward it.
    // The arcade reference keeps `nearest_lane` updated as it walks the object list and hands it
    // to the droid as `droid_data`; we scan the per-lane lists for the same
    // thing -- the enemy with the smallest z.
    int   target = -1;
    float bestZ  = 0.0f;
    /* @vfp-exempt R3 — droid nearest-enemy minimum: loop-carried bestZ over per-enemy float z; integerizing changes hunt target. measured n/a. Verified 2026-09-06. */
    for (int lane = 0; lane < n; ++lane) {
        for (const Enemy& e : engine.grid[lane].enemies) {
            if (target < 0 || e.z < bestZ) { bestZ = e.z; target = lane; }
        }
    }

    const int dir = _droid_lor(engine, engine.ai_lane, target);
    if (dir < 0) return;   // already on it, or nothing to hunt

    int next = engine.ai_lane + (dir ? 1 : -1);
    if (engine.grid_level_go_round) {
        if (next < 0)  next = n - 1;
        if (next >= n) next = 0;
    } else {
        if (next < 0 || next >= n) return;   // open web: no wrap, just stop
    }

    // The glide's ENDPOINTS are captured here; the position between them is
    // interpolated by aiDroidEase every frame (the Move branch above), NOT
    // integrated from a fixed per-frame delta -- see constants.h, which records
    // why the arcade reference's constant-velocity step was replaced. The
    // destination is the lane's SHOT-SPAWN point (midpoint pushed out along the
    // normal, exactly as buildShots places a bullet), not the bare midpoint --
    // the droid must sit on its own muzzle, and gliding between midpoints would
    // drag it back onto the web mid-step.
    const auto& dst = engine.grid_level_pos[next];
    const auto& dnm = engine.grid_level_normal[next];
    engine.ai_next_lane = next;
    engine.ai_x0 = engine.ai_x;
    engine.ai_y0 = engine.ai_y;
    engine.ai_x1 = dst.x + dnm.x * AI_DROID_NORMAL_OFFSET;
    engine.ai_y1 = dst.y + dnm.y * AI_DROID_NORMAL_OFFSET;
    engine.ai_move = AI_DROID_MOVE_FRAMES;
}

} // namespace ts
