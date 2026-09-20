// ============================================================================
// arcade_spiker.cpp -- ARCADE_SPIKER and the SPIKE it builds. See arcade_spiker.h for
// the provenance, the IP boundary, the unit conversion, every constant with
// its source, and the shared-vs-duplicated split against this engine's own
// spiker.
//
// STRUCTURE. Three mode handlers and one lane-adoption helper, all file-local;
// the only things the rest of the game sees are the entry points declared in
// the header. No virtual, no vtable, no function-pointer table -- the mode
// dispatch is a `switch` on a three-value enum, which is a compiler-chosen
// branch, exactly like enemies.cpp's own id dispatch (DOCTRINE.md perf
// doctrine, enemy_family.h). No STL growth in the per-tick path: the only
// container touch is the lane adoption's push_back, which happens at most once
// per CLIMB cycle onto a vector init_level already reserve()s to MAX_ENEMIES.
// ============================================================================

#include "arcade_spiker.h"

#include "enemies_shared.h"   // arcade_alien_fire_tick -- the CLIMB/DESCEND sites

#include <cstdlib>   // rand

namespace ts {
namespace enemyfam {

// ============================================================================
// Per-level scalars
// ============================================================================

int ArcadeSpiker::speedRaw(int level) {
    // Consecutive wave PAIRS share a value. `level` is engine.current_level,
    // 0-based. The table covers 0..111; the arcade relies on its own wave-wrap
    // to stay inside it, this port clamps -- which is a real difference only
    // above level 111, where the arcade would read past the table.
    int idx = level / 2;
    if (idx < 0) idx = 0;
    if (idx >= SpeedTableLen) idx = SpeedTableLen - 1;
    return SpeedRaw[idx];
}

float ArcadeSpiker::stepZ(int level) {
    // A per-tick DELTA, so it takes the RATE conversion.
    return (float)speedRaw(level) * RAW_Z * TICK_RATE;
}

float ArcadeSpiker::buildAllowance(int level) {
    // A DISTANCE, so it takes NO rate conversion: BuildTicks arcade ticks at
    // the arcade's own step is a fixed length of spike however fast the host
    // ticks. That is the whole point of carrying the budget as a distance.
    return (float)BuildTicks * (float)speedRaw(level) * RAW_Z;
}

// ============================================================================
// Mode handlers
// ============================================================================

namespace {

// Move this spiker from `fromLane` slot `idx` to `toLane`. Mirrors the shared
// _transfer_enemy (enemies.cpp) exactly -- same swap-down removal, same
// reserve()d vectors -- MINUS its `animation_phase = -animation_phase` and its
// `animation_vec` flip, both of which are classic-family lane-crossing
// conventions (audit §7 R3) and are wrong for an arcade family that stores a
// MODE there. _transfer_enemy is `static` in enemies.cpp and unreachable from
// here; the audit §3.3 and arcade_enemies.md §3.3 both say the end state is a
// shared enemies_shared.h that both sets call. See the report.
//
// Caller must have checked the destination's capacity first.
bool adoptLane(GameEngine& engine, int fromLane, int idx, int toLane) {
    GridElement& from = engine.grid[fromLane];
    GridElement& to   = engine.grid[toLane];

    Enemy moved = from.enemies[idx];
    moved.current_lane = toLane;
    to.enemies.push_back(moved);
    to.num_enemies += 1;

    from.num_enemies -= 1;
    if (idx < from.num_enemies) {
        from.enemies[idx] = from.enemies[from.num_enemies];
    }
    from.enemies.pop_back();
    return true;
}

// ---- MODE 0: SEEK ----------------------------------------------------------
// Scan the whole web once. Prefer any CLEAR lane (and make a new spike there);
// otherwise adopt the SHORTEST existing spike. Then reload the build allowance
// and enter CLIMB.
//
// Returns true iff the spiker left `lane`'s slot (the enemy_family.h removal
// contract).
bool seek(GameEngine& engine, const EnemyCtx& ctx, int lane, int idx,
          Enemy& enemy) {
    const int n = ctx.lane_count;
    if (n <= 0) return false;

    int   clearLanes[GRID_MAX_ELEMENTS];
    int   clearCount   = 0;
    int   shortestLane = -1;
    float shortest     = ArcadeSpiker::ShortestSentinel;

    const int scanTo = (n < GRID_MAX_ELEMENTS) ? n : GRID_MAX_ELEMENTS;
    for (int v = 0; v < scanTo; ++v) {
        const float h = engine.grid[v].spike;
        if (h <= 0.0f) {
            clearLanes[clearCount++] = v;
        } else if (h <= shortest) {
            // `<=`, not `<`: the arcade's comparison SKIPS only when the
            // running best is strictly shorter, so an equal-length spike
            // replaces the incumbent and among equal-shortest lanes the
            // HIGHEST index wins. Reproduced deliberately -- it is the tie
            // break a player would see as "it always goes to the far one".
            shortest     = h;
            shortestLane = v;
        }
    }

    bool makeNew = false;
    int  target;
    if (clearCount > 0) {
        // THE OFF-BY-ONE, DELIBERATELY FIXED. The arcade decrements the clear
        // count before handing it to a helper that already returns 0..n-1, so
        // the LAST clear lane in scan order can never be chosen -- a real
        // placement bias (with exactly one clear lane it is correct only by
        // luck). arcade_enemies.md §2.3 / §8.8(a) recommend fixing it; this is
        // that choice, recorded rather than inherited.
        target  = clearLanes[rand() % clearCount];
        makeNew = true;
    } else if (shortestLane >= 0) {
        target = shortestLane;
    } else {
        // Every lane is spiked yet none was recorded: only reachable with no
        // lanes at all. Do nothing this tick rather than inventing a target.
        return false;
    }

    // Capacity is checked BEFORE anything is committed, so a refusal leaves
    // the spiker in SEEK with no spike created and no allowance spent, and it
    // simply retries next tick.
    if (target != lane && engine.grid[target].num_enemies >= MAX_ENEMIES) {
        return false;
    }

    if (makeNew) {
        // The super-spike roll is returned and currently dropped: storing it
        // needs one bool per lane on GridElement, a shared file this wave may
        // not edit. See arcade_spiker.h "THE FLASHING SPIKE" and the report.
        (void)ArcadeSpike::create(engine, target);
    }

    // Adopt. The allowance reloads on EVERY adoption -- that is what makes a
    // spiker's contribution per visit fixed, so a lane needs several visits to
    // reach the cap.
    ArcadeSpiker::budget(enemy) = ArcadeSpiker::buildAllowance(engine.current_level);
    ArcadeSpiker::setMode(enemy, ArcadeSpiker::CLIMB);
    // The arcade re-snaps the body angle to the adopted lane's own
    // orientation, so the spin restarts from the lane angle each cycle. This
    // field carries the spin RELATIVE to that orientation (models.h anchor_rot
    // = "degrees already rotated"); the renderer composes it with the lane's
    // own angle, so zero here is the arcade's re-snap.
    ArcadeSpiker::roll(enemy)   = 0.0f;
    enemy.anchor             = ANCHOR_LANE_MID;
    enemy.current_lane       = target;

    if (target == lane) return false;
    return adoptLane(engine, lane, idx, target);
}

// ---- MODE 1: CLIMB ---------------------------------------------------------
void climb(GameEngine& engine, int lane, Enemy& enemy, float step) {
    // THE ONE GLOBAL ALIEN-FIRE TIMER (arcade_enemies.md §2.5) -- the spiker's
    // climb and descend are two of its only three call sites (the flipper's
    // rail is the third). It is per-SET state on GameEngine, cleared in
    // init_level; the policy lives in ArcadeFlipper::alienFireTick and the free
    // function in enemies_shared.h. Ticked BEFORE the move, matching the
    // flipper's rail, so the bullet is born at the z the shooter is leaving.
    arcade_alien_fire_tick(engine, lane, enemy.z);

    enemy.z -= step;

    // TURNAROUND. Strict less-than, evaluated AFTER the move and BEFORE any
    // growth, so the turnaround tick grows nothing and spends no allowance.
    if (enemy.z < ArcadeSpiker::TurnaroundZ) {
        ArcadeSpiker::setMode(enemy, ArcadeSpiker::DESCEND);
        return;
    }

    // Grow only while the spiker is AHEAD OF THE TIP. The arcade computes
    // (base - z) - length and returns on <= 0; the tip sits at base - length,
    // so that is exactly "z is nearer the rim than the tip".
    const float tipZ = ArcadeSpiker::BaseZ - engine.grid[lane].spike;
    if (enemy.z >= tipZ) return;

    // One step of growth -- or none, if the cap blocked the store.
    ArcadeSpike::growStep(engine, lane, step);

    // The allowance is spent on EVERY tick the spiker is ahead of the tip,
    // INCLUDING cap-blocked ones: the predicate is "the spiker is ahead of the
    // tip", not "growth occurred". When it runs out the spiker turns around --
    // it does NOT keep climbing to the turnaround plane building nothing.
    ArcadeSpiker::budget(enemy) -= step;
    if (ArcadeSpiker::budget(enemy) < 0.0f) {
        ArcadeSpiker::setMode(enemy, ArcadeSpiker::DESCEND);
    }
}

// ---- MODE 2: DESCEND -------------------------------------------------------
void descend(GameEngine& engine, int lane, Enemy& enemy, float step) {
    // The alien-fire timer ticks here too -- same note as climb(). A DESCENDING
    // spiker is still a firing enemy in the reference, which is why this is a
    // separate call site and not a shared prologue.
    arcade_alien_fire_tick(engine, lane, enemy.z);

    enemy.z += step;
    if (enemy.z < ArcadeSpiker::BaseZ) return;
    // Land exactly on the far plane and start looking for the next lane. The
    // arcade clamps here (unlike its super-flipper-3 rim promotion, which
    // deliberately overshoots), so there is no drift across cycles.
    enemy.z = ArcadeSpiker::BaseZ;
    ArcadeSpiker::setMode(enemy, ArcadeSpiker::SEEK);
}

} // namespace

// ============================================================================
// ArcadeSpiker entry points
// ============================================================================

bool ArcadeSpiker::update(GameEngine& engine, const EnemyCtx& ctx,
                       int lane, int idx, Enemy& enemy) {
    // ---- Per-tick prologue: the roll, in EVERY mode ------------------------
    float& r = roll(enemy);
    r += RollDegPerTick;
    if (r >= 360.0f) r -= 360.0f;   // one subtract: the step is far under 360

    // The arcade runs its bullet test here, before the mode body; this port's
    // shared shot sweep runs AFTER the family update (enemies.cpp). That is a
    // property of the shared pipeline, not of this family, and it costs at
    // most one tick of ordering on a one-hit-kill enemy.

    const float step = stepZ(engine.current_level);

    switch (mode(enemy)) {
    case SEEK:
        return seek(engine, ctx, lane, idx, enemy);
    case CLIMB:
        climb(engine, lane, enemy, step);
        return false;
    case DESCEND:
        descend(engine, lane, enemy, step);
        return false;
    default:
        // Unreachable; a spiker that somehow holds a foreign mode re-seeks
        // rather than freezing on the far plane and pinning the level open.
        setMode(enemy, SEEK);
        return false;
    }
}

bool ArcadeSpiker::canSpawn(const GameEngine& engine) {
    // See the header: this IS the arcade semaphore, expressed in state the
    // engine already keeps, and the "a kill returns the token" half falls out
    // of _remove_enemy's decrement.
    return engine.enemies_nums[ARCADE_SPIKER] == 0;
}

bool ArcadeSpiker::spawn(GameEngine& engine) {
    if (!canSpawn(engine)) return false;
    const int n = engine.lane_count;
    if (n <= 0) return false;

    // Uniform over the WHOLE web, both end lanes included -- unlike the
    // tanker, whose end-lane exclusion exists to keep its two children in
    // distinct adjacent lanes (arcade_enemies.md §2.2). A spiker has no children.
    const int lane = rand() % n;
    GridElement& elem = engine.grid[lane];
    if (elem.num_enemies >= MAX_ENEMIES) return false;

    Enemy e{};
    e.id   = ARCADE_SPIKER;
    // life 1 is load-bearing, not a placeholder: the shared shot/enemy
    // exchange is a mutual hit-point subtraction, so one hit kills and the
    // laser pierces, for free (arcade_enemies.md §3.5).
    e.life = ENEMY_LIFE[ARCADE_SPIKER];
    // NO ARRIVAL DOT. The flipper and the tanker spawn 300 units past the far
    // plane as an inert perspective dot; the arcade spiker is built directly
    // ON the far plane, fully live and drawn normally.
    e.z            = BaseZ;
    e.current_lane = lane;
    e.anchor       = ANCHOR_LANE_MID;

    // ---- THE mush_flag TRAP (enemy_pipeline_audit.md §3) -------------------
    // `mush_flag` is named as the mushroom part bitmask and is not used that
    // way: four shared sites treat bit 1 as a general gate. A hand-built enemy
    // inherits 0 and is therefore (a) damaged by an upgraded player shot only
    // 1 roll in 11 -- reading as "the spiker is a bullet sponge", the exact
    // opposite of the arcade one-hit rule -- and (b) given this engine's
    // breathing wobble and 2.25 z-stretch, which arcade_enemies.md §5.3 forbids
    // for arcade enemies. Setting 15 (what GameEngine::init_enemy sets)
    // answers both correctly. The audit's own recommendation is to replace the
    // four gates with named predicates as its OWN harness-gated change; until
    // that lands this is the one-line form of the same answer.
    e.mush_flag = 15;

    setMode(e, SEEK);
    roll(e)   = 0.0f;
    budget(e) = 0.0f;

    elem.enemies.push_back(e);
    elem.num_enemies += 1;
    engine.enemies_nums[ARCADE_SPIKER] += 1;

    // AUDIT §8, THE LEVEL-HANG: enemies_todo[] is decremented by the SHARED
    // machinery in exactly one place -- init_embrio -- and the arcade set has
    // no embryo. If nothing decrements it, is_level_clear() never fires and
    // the level hangs forever with nothing on screen, with no diagnostic. The
    // release point IS the spawn for this set, so every arcade family
    // decrements its own here (see the accounting rule above arcade_release in
    // enemies_shared.h) -- which means the caller's spawner must NOT also
    // decrement it.
    if (engine.enemies_todo[ARCADE_SPIKER] > 0) {
        engine.enemies_todo[ARCADE_SPIKER] -= 1;
    }
    return true;
}

// ============================================================================
// ArcadeSpike -- the arcade rules for GridElement::spike
// ============================================================================

bool ArcadeSpike::rollSuper(int level) {
    int w = level;
    if (w < 0)   w = 0;
    if (w > 127) w = 127;
    // 2 * min(wave,127) out of 256. The arcade compares the raw 0..255 draw
    // against the probability and takes the flag on strictly-less.
    return (rand() % 256) < 2 * w;
}

bool ArcadeSpike::damages(int shot_id, bool superSpike) {
    // Enemy shots and the powerup capsule never touch a spike; this mirrors
    // collision.cpp's existing `shot.id < ENEMY_SHOT1 && shot.id !=
    // POWERUP_SHOT` gate so the two sets agree about WHICH shots reach here.
    if (shot_id >= ENEMY_SHOT1 || shot_id == POWERUP_SHOT) return false;
    if (!superSpike) return true;
    // A flashing spike is immune to the ordinary claw shot; only the particle
    // laser hurts it.
    return shot_id == PLAYER_SHOT2;
}

bool ArcadeSpike::consumesShot(int shot_id, bool superSpike) {
    // THE LASER DRILLS. On a plain spike the particle laser is NOT consumed:
    // it stays alive and keeps hitting once per tick as it travels down the
    // lane, and that PERSISTENCE -- not a bigger per-hit number -- is what
    // makes the laser eat spikes. (The arcade's own "was it a laser" damage
    // branch assigns the same amount in both arms, i.e. it is dead code.)
    // A super spike does consume it.
    if (shot_id == PLAYER_SHOT2) return superSpike;
    return true;
}

bool ArcadeSpike::create(GameEngine& engine, int lane) {
    engine.grid[lane].spike = Init;
    return rollSuper(engine.current_level);
}

bool ArcadeSpike::growStep(GameEngine& engine, int lane, float step) {
    const float next = engine.grid[lane].spike + step;
    // THE CAP IS A SKIP, NOT A CLAMP: when the next step WOULD REACH the
    // ceiling the store is abandoned entirely. There is no partial growth up
    // to the cap, so the attained maximum is always one step short of Cap.
    if (next >= Cap) return false;
    engine.grid[lane].spike = next;
    return true;
}

bool ArcadeSpike::shotHit(GameEngine& engine, int time, int lane, Shot& shot,
                       bool superSpike) {
    GridElement& elem = engine.grid[lane];
    const float tipZ = GRID_ELEMENT_LENGTH - elem.spike;

    if (!damages(shot.id, superSpike)) {
        // Flashing spike vs an ordinary shot: the shot is spattered into a
        // spark and NOTHING else happens -- no height removed, no score, no
        // "tink". The arcade's collision helper returns no-hit here, so the
        // spike's own routine never reaches its damage path.
        shot.life = 0;
        return false;
    }

    // Capture the spike's height BEFORE this hit chips it: the legacy decspike
    // computes the tink's period from the length as it stood at the hit, so the
    // ramp reflects the pre-hit height. Passed to init_explosion, which turns it
    // into the pitch (see engine.h).
    const float spike_height = elem.spike;
    elem.spike -= Chip;
    bool destroyed = false;
    // ONE DELIBERATE DIVERGENCE, and it is forced by the storage this family
    // SHARES. The arcade subtracts and then tests the SIGN, so a spike reduced
    // to exactly zero SURVIVES as a zero-length object and can be hit an 11th
    // time -- which is where "a fresh 30-unit spike takes 11 shots, not 10"
    // comes from. Here a spike is a lane FLOAT, and every consumer (the shot
    // sweep's `elem.spike > 0.0f` gate, buildSpikeSegs, player.cpp's lethality
    // test) treats zero as absent, so an arcade-exact `< 0` would strand a
    // never-destroyed zero-height spike that can never be hit again, is never
    // drawn, and never pays its destruction. `<= 0` instead: a FRESH spike
    // clears in 10 shots rather than 11.
    //
    // The number that was adjudicated is unaffected -- a FULL spike still
    // takes exactly 50 -- because a full spike's height is not a whole
    // multiple of Chip and its last hit crosses zero rather than landing on
    // it. Fixing this the other way would mean widening the shared
    // `> 0.0f` gate for BOTH rosters; see the report.
    if (elem.spike <= 0.0f) {
        elem.spike = 0.0f;
        destroyed  = true;
    }

    // Every landed hit pays `Score` points and raises the spike "tink", both
    // through this engine's own economy: init_explosion routes EXPLOSION_SPIKE
    // to SfxId::SPIKE, bumps the multiplier by 0.001 and awards
    // round(energy * multiplier * ScoreRate) -- so HitEnergy is the energy
    // that pays exactly `Score` at multiplier 1.0.
    engine.init_explosion(time, HitEnergy, lane, tipZ, EXPLOSION_SPIKE, 0, true, true, spike_height);

    if (destroyed && engine.note_powerup_kill()) {
        // SHARED AND DELIBERATELY UNCHANGED: the capsule cadence is this
        // port's, fitted to keep the warp token reachable (DOCTRINE.md), and
        // arcade_enemies.md §3.6 says one cadence for both sets.
        engine.init_shot(lane, tipZ, POWERUP_SHOT);
        engine.show_powerup_text(time, -1);
    }

    if (consumesShot(shot.id, superSpike)) shot.life = 0;
    return destroyed;
}

} // namespace enemyfam
} // namespace ts
