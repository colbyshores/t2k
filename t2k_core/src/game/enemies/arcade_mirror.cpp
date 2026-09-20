// ============================================================================
// arcade_mirror.cpp -- ARCADE_MIRROR and the SHARED REFLECTED-SHOT RAIL. See
// arcade_mirror.h for the provenance, the IP boundary, the unit conversion,
// every constant with its source, and the "one sentence" this whole file
// exists to make true.
//
// STRUCTURE. Two exported scopes and three file-local helpers; the mode
// dispatch is a plain `if` chain over a two-value enum and the kind dispatch a
// single branch on one bit -- compiler-chosen branches, exactly like
// enemies.cpp's own id `switch` (DOCTRINE.md perf doctrine, enemy_family.h). No
// virtual, no vtable, no function-pointer table. No STL growth in any per-tick
// path: the take-over is five stores into a Shot the shot vector ALREADY
// holds, so a Mirror's whole gimmick allocates nothing at all.
//
// STATE MACHINE, one line each:
//   ARRIVAL  inert, invulnerable, closing 0.35131 z/tick from z = 71.875 to
//            the far plane, then LIVE on that same tick.
//   LIVE     spin; descend while deeper than the park plane; then look for a
//            bullet, and if there is one: count it, shove itself back up the
//            web, and hand the bullet back to the player pointing the other
//            way.
// ============================================================================

#include "arcade_mirror.h"

#include <cmath>
#include <cstdlib>   // rand

namespace ts {
namespace enemyfam {

// ============================================================================
// File-local helpers
// ============================================================================

namespace {

// ---------------------------------------------------------------------------
// THE MIRROR'S DESCENT SPEED, in the reference's own world-z per reference
// tick. Split out from the two public conversions because the SAME number
// feeds a RATE (the descent) and a DISTANCE (the knockback), and those two
// take DIFFERENT conversions -- see the KnockbackTicks note in the header.
// Getting that wrong is a silent 12.4% error on one of them.
//
// RE-AUTHORED, narrowly, and flagged for the verifier. The recovery
// deliberately does not reproduce the 56-entry table (IP boundary) and gives
// only its range across the Mirror's own levels: ~0.75 at 57 rising to ~0.97
// at 98. So: linear, EXACT at both recovered ends, and HELD FLAT outside the
// band rather than extrapolated. Holding rather than extrapolating is the
// honest choice -- the Mirror does not spawn outside 57-98, so every
// extrapolated value would be an unverifiable number used by nothing.
//
// The `>> 1` is the recovered STRUCTURE: the table is indexed by wave/2, so
// consecutive wave PAIRS share a value and the ramp steps every second level
// rather than every level. That is visible in play and costs one shift.
// ---------------------------------------------------------------------------
float descentWorld(int wave) {
    int i = wave >> 1;
    if (i < 0) i = 0;
    if (i > ArcadeMirror::DescentTableSize - 1)
        i = ArcadeMirror::DescentTableSize - 1;

    constexpr int lo = ArcadeMirror::DescentFirstWave >> 1;   // 28
    constexpr int hi = ArcadeMirror::DescentLastWave  >> 1;   // 49
    static_assert(hi > lo, "the Mirror's recovered level band must be non-empty");

    if (i <= lo) return ArcadeMirror::DescentWorldFirst;
    if (i >= hi) return ArcadeMirror::DescentWorldLast;
    const float t = (float)(i - lo) / (float)(hi - lo);
    return ArcadeMirror::DescentWorldFirst
         + (ArcadeMirror::DescentWorldLast - ArcadeMirror::DescentWorldFirst) * t;
}

// ---------------------------------------------------------------------------
// THE BEAST'S REFLECTED SHOT ABSORBS AN ORDINARY PLAYER BULLET.
//
// One-way: the incoming bullet is destroyed and the horn is NOT (the reference
// discards the helper's return value, so there is no trade to be had -- you
// cannot shoot a horn out of the air). The Mirror's ring runs no test at all,
// so player shots pass straight through it with no effect in either
// direction.
//
// THE PARTICLE LASER PASSES THROUGH AND IS NOT CONSUMED. The reference's
// helper lets its laser-class bullets past the test entirely, which makes the
// laser the answer to a screen full of horns -- and is the same
// "the laser drills" property arcade_spiker.h records for spikes, arrived at
// independently in a different routine.
//
// It marks the bullet dead rather than removing it, on purpose: the shared
// move_shots loop is what is iterating this vector, and its own bottom-of-body
// removal will take a life-0 shot on its own visit with no reentrancy, no
// index shuffling under the caller, and no chance of the caller holding a
// dangling reference. A life-0 player shot is inert in the meantime -- the
// shared mutual-subtraction exchange subtracts zero and raises no explosion.
// ---------------------------------------------------------------------------
void absorbPlayerShot(GameEngine& engine, int lane, const Shot& self) {
    GridElement& elem = engine.grid[lane];
    for (int i = 0; i < elem.num_shots; ++i) {
        Shot& s = elem.shots[i];
        if (&s == &self) continue;                        // never itself
        if (s.id >= ENEMY_SHOT1) continue;                // enemy fire
        if (s.id == POWERUP_SHOT) continue;               // the capsule
        if (ArcadeReflectedShot::is(s.id)) continue;      // another horn
        if (s.id == PLAYER_SHOT2) continue;               // THE LASER DRILLS
        if (s.life <= 0) continue;
        const float d = s.z - self.z;
        if (d < ArcadeReflectedShot::AbsorbDz
            && d > -ArcadeReflectedShot::AbsorbDz) {
            s.life = 0;
            return;                                       // one per tick
        }
    }
}

// ---------------------------------------------------------------------------
// THE LETHAL TEST (the reference's shared lane check).
//
// Same lane AND the claw is vulnerable AND |dz| <= 2 world-z. The middle term
// is the reference's own "is this claw currently a target" flag; in this
// engine the equivalent conditions are that the player is neither in the
// level-complete zoom nor already dying -- which is also exactly what
// ArcadeFlipper's tryGrab() tests, and the two must not drift apart.
//
// NO JUMP GUARD, deliberately: collision.cpp's own enemy-shot-vs-player block
// records that jumping does not dodge shots in this engine, and the reference
// agrees (its lane check does not consult the claw's z at all beyond the
// window).
// ---------------------------------------------------------------------------
bool killsPlayer(const GameEngine& engine, int lane, const Shot& shot) {
    const PlayerInfo& p = engine.player;
    if (p.out_animation != 0 || p.gameover_animation > 0) return false;
    if (lane != p.grid_element_pos) return false;
    const float d = shot.z - p.z;
    return (d <= ArcadeReflectedShot::LethalDz)
        && (d >= -ArcadeReflectedShot::LethalDz);
}

// ---------------------------------------------------------------------------
// THE MIRROR'S DEATH -- the reference's `blowmeaway`, which does not "kill"
// the object so much as CONVERT IT IN PLACE into a pixel-shatter graphic
// showing the bonus it just paid.
//
// This engine's init_explosion already is that: it booms, sheds bonus
// pickups, bumps the multiplier, awards the score and FLOATS THE AWARDED
// NUMBER at the projected kill point. So the whole of `blowmeaway` is one
// call plus the shared capsule cadence plus the removal.
//
// WHY THIS FAMILY OWNS ITS OWN DEATH instead of setting life = 0 and letting
// the shared `_handle_death` run it (which is what the Beast does): NO SHARED
// SITE CAN EVER REACH A MIRROR. `shootable()` is false in every state
// (arcade_mirror.h) and `enemy_zappable()` defers to it (enemies_shared.h), so
// all four damage sites skip it and its life never goes to 0 -- there is no
// shared death to inherit. The corollary is the reason the roll lives HERE:
// `arcade_kill_score()` is NOT pure for this id -- it returns
// `rollDeathScore()` (enemies_shared.h) and must be called EXACTLY ONCE per
// corpse -- so a second path raising its own explosion would be a second roll
// and a second payout. Everything else here is a line-for-line copy of
// `_handle_death`'s tail IN ITS ORDER (explosion, then cadence, then removal)
// so the two paths cannot present differently.
//
// THE 250/500/750 IS PAID AND FLOATED EXACTLY -- and it once was not. This
// used to be a live divergence: init_explosion CLAMPED `energy` to 300 before
// awarding, so a 500 or a 750 roll paid 301 and showed 300, losing the
// recovered spread on the one enemy whose whole payoff is that the amount is a
// surprise. THE FIX LANDED IN THE SHARED FILE, not here. init_explosion now
// routes an ARCADE enemy kill's SCORE around that clamp (engine.cpp, "THE 300
// CLAMP, AND THE ONE ROUTE AROUND IT" -- the `arcade_kill` gate on
// EXPLOSION_ENEMY + isArcadeEnemy(ex_id2), which this call satisfies), so the
// award and the floating number both carry the unclamped roll. The explosion's
// own VISUAL energy stays clamped, deliberately: a Mirror's corpse is not two
// and a half times the size of a flipper's. The classic roster is
// bit-identical, because for it the two values are the same number. Do NOT add
// a compensating award_score() call -- it was the rejected workaround while the
// divergence stood (it would have fixed the score and left the DISPLAYED number
// wrong), and today it would simply double-pay.
// (enemies_shared.h's arcade_kill_score note (b) records the same.)
// ---------------------------------------------------------------------------
bool blowMeAway(GameEngine& engine, int time, int lane, int idx, Enemy& enemy) {
    const float z     = enemy.z;
    const int   bonus = ArcadeMirror::rollDeathScore();

    engine.init_explosion(time, bonus, lane, z, EXPLOSION_ENEMY, ARCADE_MIRROR);

    // SHARED AND DELIBERATELY UNCHANGED: the capsule cadence is this port's,
    // fitted to keep the warp token reachable (DOCTRINE.md), and
    // arcade_enemies.md sec 3.6 says one cadence for both sets.
    if (engine.note_powerup_kill()) {
        engine.init_shot(lane, z, POWERUP_SHOT);
        engine.show_powerup_text(time, -1);
    }

    // enemies.cpp `_remove_enemy`, mirrored exactly -- same counter, same
    // swap-down, same empty guard. It is `static` there and unreachable from
    // here; the audit's end state is a shared enemies_shared.h removal both
    // rosters call. See the report.
    engine.enemies_nums[ARCADE_MIRROR] -= 1;
    GridElement& elem = engine.grid[lane];
    elem.num_enemies -= 1;
    if (idx < elem.num_enemies) {
        elem.enemies[idx] = elem.enemies[elem.num_enemies];
    }
    if (!elem.enemies.empty()) {
        elem.enemies.pop_back();
    }
    return true;
}

} // namespace

// ============================================================================
// ArcadeReflectedShot
// ============================================================================

void ArcadeReflectedShot::releaseOwnerSlot(GameEngine& engine, const Shot& shot) {
    // UNLINK CODE 3 -- "give the player back his shot". The take-over
    // deliberately left `shots_nums[owner]` charged (arcade_mirror.h
    // takeOver(), write 4), so this is the ONLY place a reflected shot's
    // original slot comes back. Call it exactly once per shot; the `> 0` guard
    // is a floor against a stray second call, not a licence to make one.
    const int owner = ownerShotId(shot);
    if (owner >= 0 && owner < SHOTS_NUM_IDS && engine.shots_nums[owner] > 0) {
        engine.shots_nums[owner] -= 1;
    }
}

bool ArcadeReflectedShot::tick(GameEngine& engine, int time, int lane,
                               Shot& shot) {
    const Kind k = kind(shot);

    // ---- the Beast's prologue ---------------------------------------------
    // The two things that separate the horn from the ring, both of them
    // reference-recovered and neither of them affecting the other, so their
    // relative order is immaterial (it was not recovered beyond "the Beast's
    // routine does these, then falls into the shared tail").
    if (k == KIND_BEAST) {
        float& a = spin(shot);
        a += BeastSpinDegPerTick;
        if (a >= 360.0f) a -= 360.0f;   // one subtract: the step is far under 360
        absorbPlayerShot(engine, lane, shot);
    }

    // ---- THE SHARED TAIL, six instructions in the reference and three here -
    // 1. travel toward the player. The speed was captured at take-over, so
    //    this is the only place a reflected shot's z moves and there is no
    //    table read on the path. SHOT_DZ[SHOT_ID] is 0 precisely so the
    //    SHARED mover cannot step it a second time.
    shot.z -= speed(shot);

    // 2. did it get him? Same lane, vulnerable claw, |dz| <= 0.3125.
    if (killsPlayer(engine, lane, shot)) {
        // The reference's death path: the "Shot You!" message and the ouch,
        // then the shot itself is destroyed. init_gameover(13) is the same
        // entry ArcadeFlipper's grab uses, so the two arcade deaths read
        // identically.
        engine.init_gameover(time, 13);
        releaseOwnerSlot(engine, shot);
        return true;
    }

    // 3. expiry, 29 world-z PAST the rim -- it flies visibly through and
    //    behind the player before vanishing. Setting unlink code 3 is what
    //    returns the shot slot to its owner, so the release belongs on THIS
    //    exit and on the lethal one above, and on no other.
    if (shot.z >= ExpiryZ) return false;
    releaseOwnerSlot(engine, shot);
    return true;
}

// ============================================================================
// ArcadeMirror -- per-level scalars
// ============================================================================

float ArcadeMirror::descentDz(int wave) {
    // A per-tick DELTA, so it takes the RATE conversion.
    return descentWorld(wave) * UNIT_Z * TICK_RATE;
}

float ArcadeMirror::knockbackZ(int wave) {
    // A DISTANCE -- sixteen reference ticks' worth of travel is a fixed
    // LENGTH however fast the host ticks -- so it takes NO rate conversion.
    // Same distinction ArcadeSpiker::buildAllowance() exists to preserve.
    return descentWorld(wave) * (float)KnockbackTicks * UNIT_Z;
}

int ArcadeMirror::rollDeathScore() {
    // 250 / 500 / 750, uniform. CONSUMES ONE rand() DRAW at the moment of
    // death, exactly as the reference does.
    return DeathScoreLo + DeathScoreStep * (int)(rand() % DeathScoreN);
}

// ============================================================================
// ArcadeMirror -- update
// ============================================================================

bool ArcadeMirror::update(GameEngine& engine, const EnemyCtx& ctx,
                          int lane, int idx, Enemy& enemy) {
    // ---- ARRIVAL ----------------------------------------------------------
    // Inert, invulnerable (shootable() is false in every state, so this needs
    // no separate gate), no spin, no bullet test. It DOES still participate in
    // the level-clear count, which falls out for free because is_level_clear()
    // counts grid[].num_enemies.
    if (mode(enemy) == MODE_ARRIVAL) {
        enemy.z -= ArrivalDz;
        if (enemy.z > FarPlaneZ) return false;
        // Land exactly on the far plane, so the descent below always starts
        // from z = 25 with no sub-tick residue.
        enemy.z     = FarPlaneZ;
        mode(enemy) = MODE_LIVE;
        // ...and it begins that mode ON THAT SAME TICK: fall through. Same
        // shape as ArcadeFlipper's arrival -> rail handover.
    }

    // ---- LIVE, in the reference's own order --------------------------------

    // 1. SPIN, on every LIVE tick and with no gate of its own -- unlike the
    //    descent below, nothing conditions it. It does NOT run during the
    //    arrival: that branch returns above on every tick but the handover
    //    one, which is what the reference does too (an object still on the
    //    approach drift has its update routine not run at all). A quarter of
    //    the reference's house rotation rate, so the disc's facets glint
    //    slowly rather than tumbling.
    {
        float& a = spin(enemy);
        a += SpinDegPerTick;
        if (a >= 360.0f) a -= 360.0f;
    }

    // 2. DESCEND -- but only while still DEEPER than the park plane, and
    //    NOTHING CLAMPS IT. The reference's test is "if z is past the park
    //    plane, step; otherwise do not move at all", so the last step
    //    overshoots the plane by up to one tick's worth and the Mirror then
    //    holds there forever. That residue is real and is deliberately kept
    //    (the same reading arcade_flipper.cpp records for super-flipper-3's
    //    unclamped rim promotion); clamping would be a tidier number and a
    //    different enemy.
    if (enemy.z > ParkZ) {
        enemy.z -= descentDz(engine.current_level);
    }

    // 3. THE BULLET SEARCH. Non-destructive: what it finds is still a live
    //    player bullet, which is the whole point.
    Shot* bullet = ArcadeReflectedShot::findBullet(engine, lane, enemy.z, HitDz);
    if (bullet == nullptr) return false;

    // 4. COUNT THE HIT. Decrement, THEN test the sign -- so 4,3,2,1,0 all
    //    reflect and the sixth decrement is the death. FIVE HITS KILL A
    //    MIRROR and the first four each hand you back a shot.
    //
    //    THE KILLING SHOT IS NOT CONSUMED (recovery sec 1.5, sec 10 item 2):
    //    this branch jumps away BEFORE the take-over, so the bullet found
    //    above is left exactly as it was -- still live, still flying up the
    //    web, still able to kill something else the same frame. That is a
    //    real, exploitable property and it is why the search had to be the
    //    non-destructive helper.
    hits(enemy) -= 1;
    if (hits(enemy) < 0) {
        return blowMeAway(engine, ctx.time, lane, idx, enemy);
    }

    // 5. THE KNOCK-BACK. Shooting a Mirror does not damage it, IT SHOVES IT
    //    FURTHER AWAY FROM YOU -- sixteen ticks' worth of its own descent,
    //    1.875 engine z at level 57 rising to 2.425 at 98. Four shoves from a
    //    park of 6.25 land it around 13.75, against a far-plane death
    //    threshold of 25, so the second death branch below is a SAFETY NET
    //    RATHER THAN A STRATEGY: it is not reachable at any normal rate of
    //    fire. It is reproduced anyway because it is one compare and because
    //    the port's 100 webs and this engine's faster primary shot are not the
    //    conditions the reference's arithmetic was checked under.
    enemy.z += knockbackZ(engine.current_level);
    if (enemy.z > FarPlaneZ) {
        return blowMeAway(engine, ctx.time, lane, idx, enemy);
    }

    // 6. THE SOUND. The reference plays the same sample its versus-mode
    //    shield plays when it deflects -- see the header; this engine already
    //    owns exactly that event as SfxId::REFLECT.
    engine.sfx.push(HitSfx);

    // 7. TAKE THE BULLET OVER. Five writes, no allocation, and it is now
    //    coming back down this lane at you.
    ArcadeReflectedShot::takeOver(engine, *bullet,
                                  ArcadeReflectedShot::KIND_MIRROR);
    return false;
}

// ============================================================================
// ArcadeMirror -- construction
// ============================================================================

bool ArcadeMirror::spawn(GameEngine& engine, int lane) {
    if (lane < 0 || lane >= engine.lane_count) return false;
    GridElement& elem = engine.grid[lane];
    if (elem.num_enemies >= MAX_ENEMIES) return false;

    Enemy e{};
    e.id = ARCADE_MIRROR;
    // life 1 is the roster-wide arcade value. On this ONE family it is inert
    // rather than load-bearing: shootable() is always false, so no shared
    // damage site ever subtracts from it and the five-hit counter below is the
    // only thing that can kill a Mirror. Set from the descriptor anyway, so a
    // Mirror looks like every other arcade enemy to anything that reads life.
    e.life = ENEMY_LIFE[ARCADE_MIRROR];
    // Spawned 300 world-z BEHIND the far plane and inert until it arrives.
    e.z          = SpawnZ;
    e.anchor     = ANCHOR_LANE_MID;
    e.pivot_side = -1;
    e.cross_t    = 0.0f;

    // ---- THE mush_flag TRAP (enemy_pipeline_audit.md sec 3) ----------------
    // Four shared gates read bit 1 of `mush_flag` as a general-purpose "takes
    // direct damage / does not breathe" flag, and an inline-constructed Enemy
    // inherits 0. The damage half is moot here (nothing shared damages a
    // Mirror), but the OTHER half is not: a zero flag gives this engine's
    // breathing wobble and z-stretch, which arcade_enemies.md sec 5.3 forbids
    // for arcade bodies -- and a wobbling Mirror would read as a soft blob
    // rather than a hard faceted disc. 15 is what GameEngine::init_enemy
    // writes. REPLACE THIS with the audit's named predicate when that lands;
    // do not spread `= 15` to more spawn sites.
    e.mush_flag = 15;

    mode(e) = MODE_ARRIVAL;
    hits(e) = InitialHits;
    spin(e) = 0.0f;

    elem.enemies.push_back(e);
    elem.num_enemies += 1;
    engine.enemies_nums[ARCADE_MIRROR] += 1;

    // AUDIT sec 8, and this is the one that HANGS THE LEVEL if it is missed:
    // enemies_todo[] is decremented by the SHARED machinery in exactly one
    // place -- init_embrio -- and the arcade set has no embryo. If nothing
    // decrements it, is_level_clear() never fires and the level hangs forever
    // with nothing on screen and no diagnostic. The release point IS the spawn
    // for this set, so every arcade family decrements its own here (see the
    // accounting rule above arcade_release in enemies_shared.h) -- which means
    // the caller's spawner must NOT also decrement it.
    if (engine.enemies_todo[ARCADE_MIRROR] > 0) {
        engine.enemies_todo[ARCADE_MIRROR] -= 1;
    }
    return true;
}

} // namespace enemyfam
} // namespace ts
