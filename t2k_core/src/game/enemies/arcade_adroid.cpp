// ============================================================================
// arcade_adroid.cpp -- the UFO's behaviour. See arcade_adroid.h for the spec,
// the z-axis derivation and the IP boundary.
//
// The whole enemy in three moves:
//   * UPPWEB  -- dive from the far plane to the hover plane, SPINNING.
//   * ADMOVE/ADGMOVE -- pick a direction and GLIDE one lane across the web,
//                        interpolating the free x/y (a smooth, weighty lerp).
//   * ZAPPAGE -- hover over the lane, BOBBING in z, and kill a player standing
//                in it on the web.
// The SHARED shot sweep kills the saucer (it is shootable in every mode, life 1);
// this file never removes it for a bullet hit. The only removal this file does is
// the lane GLIDE, which ends in a transfer and therefore returns true.
//
// WHY THE GLIDE KEEPS THE BODY IN THE SOURCE LANE UNTIL IT LANDS:
// This engine stores enemies per-lane and the shared shot sweep only tests a shot
// against the enemies in the SHOT'S OWN lane array. A body that transferred to
// the destination array at the START of the glide would be unshootable in the
// lane it is still visually leaving and shootable in the lane it has not reached.
// Staying in the source array for the whole glide keeps it in exactly one array
// (no double-processing) and shootable in the lane the player can see it in for
// the majority of the crossing; the last sliver of mismatch is one lane-edge.
// The transfer happens the instant it lands.
// ============================================================================

#include "arcade_adroid.h"

#include "../constants.h"
#include "../models.h"
#include "../web_geometry.h"   // webLane(): the lane midpoint we lerp toward

namespace ts {
namespace enemyfam {

static inline float lerpF(float a, float b, float t) { return a + (b - a) * t; }

// Advance the hover bob one tick and write z. Called in EVERY non-dive mode so
// the bob is continuous across mode changes AND the lane transfer -- the freeze-
// then-snap of the old per-mode bob was the jerk the user saw (see header). The
// phase wraps at 2 (one full bounce per BobCycleTicks); the wrap is a single
// subtract because BobStep is far under 2.
static inline void bobTick(Enemy& enemy) {
    float ph = ArcadeAdroid::bobPhase(enemy) + ArcadeAdroid::BobStep;
    if (ph >= 2.0f) ph -= 2.0f;
    ArcadeAdroid::bobPhase(enemy) = ph;
    enemy.z = ArcadeAdroid::bobZ(ph);
}

// The destination lane for a glide in `dir`, wrapped if the level is go_round.
// Returns -1 when there is no lane to glide to in that direction.
static int destLane(const GameEngine& engine, int lane, int dir) {
    const int n = engine.lane_count;
    if (n <= 1) return -1;
    int d = lane + dir;
    if (engine.grid_level_go_round) {
        // d is at most one period out of range (lane +/- 1), so a single adjust
        // is an EXACT identity -- deliberately not the ((x%n)+n)%n double-
        // runtime-divide idiom R11 names as the worst case.
        if (d < 0) d += n;
        else if (d >= n) d -= n;
    } else if (d < 0 || d >= n) {
        return -1;  // open-web end: nothing to glide to in this direction
    }
    if (d == lane) return -1;
    return d;
}

static bool laneFull(const GameEngine& engine, int lane) {
    if (lane < 0 || lane >= (int)engine.grid.size()) return true;
    return engine.grid[lane].num_enemies >= MAX_ENEMIES;
}

// Land the gliding saucer in `dst`: push the moved copy into the destination,
// decrement the source, swap the last element down over the vacated slot, pop.
// `enemies_nums` is UNTOUCHED -- a transfer is not a removal. Returns false if
// the destination is full (caller then zaps in place instead).
static bool transferTo(GameEngine& engine, const EnemyCtx& ctx, int lane, int idx,
                      Enemy& enemy, int dst) {
    if (laneFull(engine, dst)) return false;

    using A = ArcadeAdroid;
    const LaneFrame f = webLane(engine, dst);

    Enemy moved = enemy;
    A::stamp(moved) = ctx.time;
    A::mode(moved)  = A::MODE_ZAPPAGE;
    A::zapCount(moved) = A::zapReload(moved);
    A::glideProgress(moved) = 0.0f;
    moved.px = f.mx;
    moved.py = f.my;
    // z and bobPhase are deliberately NOT reset: the bob is continuous through
    // the transfer, so the body keeps bobbing from wherever it is. Forcing
    // z = HoverZ here was the snap that made the hover look jerky.

    GridElement& from = engine.grid[lane];
    GridElement& to   = engine.grid[dst];
    to.enemies.push_back(moved);
    to.num_enemies += 1;

    from.num_enemies -= 1;
    if (idx < from.num_enemies)
        from.enemies[idx] = from.enemies[from.num_enemies];
    if (!from.enemies.empty())
        from.enemies.pop_back();

    return true;
}

// ===========================================================================
// update
// ===========================================================================

bool ArcadeAdroid::update(GameEngine& engine, const EnemyCtx& ctx,
                         int lane, int idx, Enemy& enemy) {
    // ONE ACT PER TICK. The glide-end transfer lands in a (possibly higher) lane
    // index that move_enemies would otherwise visit again this frame.
    if (stamp(enemy) == ctx.time) return false;
    stamp(enemy) = ctx.time;

    switch (mode(enemy)) {

    // ---- S-1 DIVE (reference `uppweb`) ----------------------------------
    // Fall from the far plane to the hover plane, turning the whole way down.
    // Shootable the whole way (the shared sweep tests it against bullets in
    // this lane). No zap while diving. px/py hold the spawn lane midpoint, so
    // the dive is straight down the lane it was born in.
    case MODE_UPPWEB: {
        float& a = spin(enemy);
        a += SpinDegPerTick;
        if (a >= 360.0f) a -= 360.0f;   // one subtract: the step is far under 360

        enemy.z -= DiveDz;
        if (enemy.z <= HoverZ) {
            enemy.z = HoverZ;            // land exactly on the hover plane
            bobPhase(enemy) = 0.0f;      // cosine apex == HoverZ: continuous handoff
            mode(enemy) = MODE_ADMOVE;
        }
        return false;
    }

    // ---- PICK A DIRECTION (reference `admove` -> gstartmove) -------------
    // Choose a lane we can actually land in. If neither direction is open, zap
    // in place rather than stall. The chosen direction is committed for the
    // whole glide so the lerp target is stable.
    case MODE_ADMOVE: {
        bobTick(enemy);                  // keep bobbing while it picks a direction
        int dir = glideDir(enemy);
        const bool okFwd = destLane(engine, lane, dir) >= 0
                          && !laneFull(engine, destLane(engine, lane, dir));
        const bool okBack = destLane(engine, lane, -dir) >= 0
                           && !laneFull(engine, destLane(engine, lane, -dir));
        if (!okFwd && !okBack) {
            mode(enemy) = MODE_ZAPPAGE;
            zapCount(enemy) = zapReload(enemy);
            return false;
        }
        if (!okFwd) { glideDir(enemy) = -dir; }   // commit the open direction
        glideProgress(enemy) = 0.0f;
        mode(enemy) = MODE_ADGMOVE;
        return false;
    }

    // ---- GLIDE ONE LANE (reference `adgmove` -> gmove) -------------------
    // Interpolate the free x/y from this lane's midpoint to the destination's,
    // eased so it accelerates out and settles in. Stays in the source array the
    // whole time; transfers on landing.
    case MODE_ADGMOVE: {
        bobTick(enemy);                  // bob while it glides (continuous hover)
        const int dst = destLane(engine, lane, glideDir(enemy));
        if (dst < 0) {                    // became invalid mid-glide: zap here
            mode(enemy) = MODE_ZAPPAGE;
            zapCount(enemy) = zapReload(enemy);
            return false;
        }
        const float p = glideProgress(enemy) + GlideStep;
        if (p >= 1.0f) {
            if (transferTo(engine, ctx, lane, idx, enemy, dst)) return true;
            // Destination filled while we were crossing: settle and zap here.
            mode(enemy) = MODE_ZAPPAGE;
            zapCount(enemy) = zapReload(enemy);
            return false;
        }
        const LaneFrame s = webLane(engine, lane);
        const LaneFrame d = webLane(engine, dst);
        const float e = easeInOut(p);
        enemy.px = lerpF(s.mx, d.mx, e);
        enemy.py = lerpF(s.my, d.my, e);
        glideProgress(enemy) = p;
        return false;
    }

    // ---- HOVER, BOB AND ZAP (reference `zappage`) ------------------------
    // Breathe up and down in z (a lerp on an eased triangle), and kill the
    // player if they are standing in THIS lane ON THE WEB. The jump takes the
    // claw below the rim plane, so a jumping player is not hit -- the same
    // input that reaches the saucer to shoot it also dodges its zap.
    case MODE_ZAPPAGE: {
        bobTick(enemy);                  // continuous weighty bounce (see header)

        const PlayerInfo& p = engine.player;
        // Only a live, on-web player in the lane dies. Guard against re-killing
        // a player already going out, and let the level-ending state skip the
        // zap as the reference does.
        if (p.out_animation == 0 && p.gameover_animation == 0
            && p.grid_element_pos == lane && p.z >= RimPlaneZ) {
            engine.init_gameover(ctx.time, ZapDeathText);
            return false;
        }
        zapCount(enemy) -= 1;
        if (zapCount(enemy) <= 0) {
            zapCount(enemy) = zapReload(enemy);
            mode(enemy) = MODE_ADMOVE;   // go pick a direction and glide again
        }
        return false;
    }

    default:
        return false;
    }
}

// ===========================================================================
// spawn
// ===========================================================================

bool ArcadeAdroid::spawn(GameEngine& engine, int lane) {
    if (lane < 0 || lane >= engine.lane_count) return false;
    GridElement& elem = engine.grid[lane];
    if (elem.num_enemies >= MAX_ENEMIES) return false;

    Enemy e{};
    e.id   = ARCADE_ADROID;
    e.life = ENEMY_LIFE[ARCADE_ADROID];   // 1 -- one hit kills (shared exchange)
    e.z    = FarPlaneZ;                   // born at the far plane, then dives
    e.anchor = ANCHOR_FREE;               // the family owns px/py (see header)

    // ANCHOR_FREE contract (models.h): clear px/py on entry. The dive is
    // straight down the spawn lane, so start at that lane's midpoint.
    const LaneFrame f = webLane(engine, lane);
    e.px = f.mx;
    e.py = f.my;

    // mush_flag 15 is what GameEngine::init_enemy writes for a non-breathing
    // body; an inline-constructed Enemy inherits 0, which would give the saucer
    // this engine's breathing wobble and z-stretch -- forbidden for arcade
    // bodies (arcade_enemies.md sec 5.3). Same value the Mirror's spawn uses.
    e.mush_flag = 15;

    mode(e)      = MODE_UPPWEB;           // make_adroid starts the saucer diving
    zapReload(e) = ZapReload;
    zapCount(e)  = ZapReload;
    // The spawn angle's sign picks the glide direction (gstartmove). Random here.
    glideDir(e)  = (rand() & 1) ? DIR_RIGHT : DIR_LEFT;
    spin(e)      = 0.0f;
    glideProgress(e) = 0.0f;
    bobPhase(e)  = 0.0f;
    stamp(e)     = -1;                    // never acted (ctx.time is >= 0)

    elem.enemies.push_back(e);
    elem.num_enemies += 1;
    engine.enemies_nums[ARCADE_ADROID] += 1;

    // The arcade release decrements its OWN enemies_todo (enemies_shared.h sec
    // 7): the shared machinery only decrements it in init_embrio, the arcade set
    // has no embryo, and a missed decrement hangs is_level_clear() forever.
    if (engine.enemies_todo[ARCADE_ADROID] > 0) {
        engine.enemies_todo[ARCADE_ADROID] -= 1;
    }
    return true;
}

// ===========================================================================
// rollDeathScore -- the generic blowmeaway bonus, 250 / 500 / 750.
// CONSUMES ONE rand() DRAW at the moment of death (enemies_shared.h sec 3a).
// ===========================================================================

int ArcadeAdroid::rollDeathScore() {
    return DeathScoreLo + (rand() % DeathScoreN) * DeathScoreStep;
}

} // namespace enemyfam
} // namespace ts
