#pragma once
// ============================================================================
// enemies_shared.h -- THE ARCADE SEAM. The handful of decisions the SHARED
// pipeline has to route per-roster, in one place, so no shared file has to
// know a family's internals and no family has to be named twice.
//
// Every function here answers identically for the 14 CLASSIC ids -- by
// construction, `isArcadeEnemy()` is the first test in each one and the classic
// answer is the answer the shared code already had. That is what makes wiring
// the arcade roster provably inert for the shipped game: the Wave A harnesses
// (real GameEngine, real tick order, per-enemy IEEE-754 dumps) must stay
// byte-identical, and a difference of one byte means a gate below is missing.
//
// WHY A HEADER OF `inline` FREE FUNCTIONS. Perf doctrine (DOCTRINE.md): no
// virtual, no vtable, no function-pointer table indexed in a loop. Each of
// these is a switch on a small dense enum behind one integer compare, fully
// inlinable at the call site, and the common (classic) path is that single
// compare. Same reasoning as enemy_family.h's "struct of statics".
//
// The map of WHICH shared site needs WHICH of these is
// docs/design/enemy_pipeline_audit.md; the rules they implement are
// docs/design/arcade_enemies.md sec 3.5 / sec 3.6 / sec 2.5.
// ============================================================================

#include "enemy_family.h"
#include "arcade_flipper.h"
#include "arcade_tanker.h"
#include "arcade_spiker.h"
#include "arcade_mirror.h"
#include "arcade_fuseball.h"
#include "arcade_pulsar.h"
#include "arcade_adroid.h"

namespace ts {

// ---------------------------------------------------------------------------
// 1. THE SHOOTABLE PREDICATE (audit sec 2/C9 and sec 9/Z4)
//
// There is no vulnerability hook anywhere in the shared pipeline: every enemy
// is shootable, always. Three arcade states say otherwise, and two of them are
// long -- the flipper's arrival dot and the tanker's approach dot are inert for
// ~133 ticks apiece, and the mid-flip flipper's window is NON-MONOTONIC (it
// opens, locks out, and on a reflex corner opens again; see arcade_flipper.h
// correction (a)).
//
// Governs ALL FOUR damage sites, which is the point of it being one function.
// TWO call it DIRECTLY -- collision.cpp's shot sweep and enemies.cpp's
// enemy-side sweep. The other two, weapons.cpp's tremor kill zone and its
// zapper target search, reach it through enemy_zappable() below, which defers
// here for every id but the fuseball (see 1b -- that deferral is what keeps
// this one predicate the answer for all four).
// Miss one and the dot is killable by exactly that weapon.
// ---------------------------------------------------------------------------
inline bool enemy_shootable(const Enemy& e) {
    if (!isArcadeEnemy(e.id)) return true;      // every classic enemy, always
    switch (e.id) {
    case ARCADE_FLIPPER:
    case ARCADE_SFLIPPER2:
    case ARCADE_SFLIPPER3:
        return enemyfam::ArcadeFlipper::shootable(e);
    case ARCADE_TANKER:
    case ARCADE_FUSE_TANKER:
    case ARCADE_PULSAR_TANKER:
        return enemyfam::ArcadeTanker::shootable(e);
    case ARCADE_MIRROR:
        // ALWAYS FALSE, and that is the enemy: the two shot sweeps must not
        // damage it (it owns its own five-hit counter and re-tasks the
        // bullet), the SUPERZAPPER must not touch it (opt-in per update
        // routine in the reference; the Mirror never opts in), and the tremor
        // is treated the same way by judgement.
        return enemyfam::ArcadeMirror::shootable(e);
    case ARCADE_BEAST:
        // ...and the Beast is the deliberate CONTRAST: its routine tests the
        // zapper flag before its mode dispatch, so one zap kills it whatever
        // its horn count. Spelled out rather than left to the default so the
        // pairing is visible at the one place it is decided.
        return true;
    case ARCADE_SPIKER:
        // The arcade spiker has NO invulnerable state -- it is built live on
        // the far plane with no arrival dot (arcade_spiker.h "Custom
        // construction") and is one hit from death in every mode. Spelled out
        // rather than left to a default so that a family which later grows a
        // window has an obvious place to say so.
        return true;
    case ARCADE_FUSEBALL:
        // FALSE FOR ~78% OF ITS LIFE, and that is the headline fact about the
        // enemy: the bullet test exists only inside its crossing routine, and
        // only for the middle 9 of that crossing's 16 steps. A PARKED fuseball
        // is untouchable by gunfire at any depth. See enemy_zappable() below
        // for why that answer must NOT be reused for the superzapper.
        return enemyfam::ArcadeFuseball::shootable(e);
    case ARCADE_PULSAR:
        // SHOOTABLE IN EVERY PHASE, INCLUDING THE LETHAL ONE. Nothing in either
        // reference gates damage on the pulse counter (every read of it was
        // enumerated: the tick, the shape select, the phase test, the touchdown
        // hold and three draw routines -- none gates damage). There is no
        // "invulnerable while pulsing" rule; do not invent one. The one
        // exception is the inbound arrival dot, which is the same rule the
        // flipper and tanker dots follow.
        return enemyfam::ArcadePulsar::shootable(e);
    case ARCADE_PULSAR_SPARK:
        // No arrival, no invulnerable window: one hit anywhere kills it.
        return enemyfam::ArcadePulsarSpark::shootable(e);
    case ARCADE_ADROID:
        // SHOOTABLE IN EVERY MODE, INCLUDING THE DIVE. The reference's `collie`
        // and `mcollie` both test the saucer against the player's BULLET table
        // (REF.ASM:5754 / :5743), so a bullet hits it whether it is diving
        // or hovering. The jump-to-shoot is NOT a shootability gate -- it is a
        // consequence of the hover plane sitting below the resting claw's z, so a
        // resting shot simply cannot REACH it (arcade_adroid.h). Spelled out
        // rather than left to the default so that fact is visible at the seam.
        return true;
    default:
        return true;
    }
}

// ---------------------------------------------------------------------------
// 1b. THE SUPERZAPPER / TREMOR PREDICATE -- DELIBERATELY NOT enemy_shootable()
//
// Until the fuseball landed, ONE predicate served all four damage sites and
// that was correct, because no enemy's bullet answer and area-weapon answer
// had ever differed. The fuseball is the first for which they do, and getting
// it wrong is a HARD LEVEL HANG rather than a cosmetic slip:
//
//   * a fuseball is unshootable for ~78% of its life (above),
//   * and it NEVER LANDS AND NEVER EXPIRES -- it has no self-destruct and no
//     rim behaviour that removes it,
//   * so with the superzapper ALSO blocked, a single parked fuseball holds
//     `is_level_clear()` open forever with nothing the player can do about it.
//
// The reference closes that case structurally: its fuseball routine runs an
// UNCONDITIONAL collision call on any superzap frame, BEFORE dispatching to a
// mode routine, so the zapper kills a parked fuseball. `zappable()` is that
// rule, and this predicate is the seam that lets the two weapons ask different
// questions.
//
// EVERY OTHER ID DEFERS TO enemy_shootable(), including the classic roster
// (one integer compare, same answer as before) and including the Mirror, whose
// immunity to the superzapper IS the recovered behaviour rather than an
// oversight, and the Beast, which dies to one zap at any horn count.
// ---------------------------------------------------------------------------
inline bool enemy_zappable(const Enemy& e) {
    if (e.id == ARCADE_FUSEBALL) return enemyfam::ArcadeFuseball::zappable(e);
    // The flipper family: the super zapper (and the tremor, which this port
    // treats the same) kills it in ANY mode. The original's run_flipper tests
    // _sz before its mode dispatch and zappits regardless of the mid-flip
    // vulnerability window (yak.s:11017), so that non-monotonic shootable()
    // lockout is a bullet rule and must NOT gate the area weapons. Only the
    // inert arrival dot stays out. (ARCADE_BEAST already returns true below.)
    if (e.id == ARCADE_FLIPPER || e.id == ARCADE_SFLIPPER2 || e.id == ARCADE_SFLIPPER3)
        return enemyfam::ArcadeFlipper::zappable(e);
    return enemy_shootable(e);
}

// ---------------------------------------------------------------------------
// 2. WHO OWNS ENEMY-vs-PLAYER (audit sec 4/P2 and P4)
//
// The shared block in enemies.cpp is "any enemy whose swept z window contains
// the player, in the player's lane, kills the player". It is wrong for the
// arcade roster in BOTH directions:
//
//   * the TANKER can never kill the player at any moment of its life -- only
//     the EVENT of it opening at the rim is dangerous (arcade_tanker.h);
//   * the FLIPPER grabs in the lane it is flipping OUT OF, not the one it has
//     already been indexed into (arcade_flipper.h correction (b)) -- and with the
//     descriptor dz at 0 the shared window degenerates to exact float
//     equality, which is ALWAYS TRUE for a flipper parked at the rim, so the
//     shared block would fire on the DESTINATION lane and silently undo that
//     correction;
//   * the SPIKER's hazard is the SPIKE it leaves behind, which player.cpp
//     already tests as a lane property; the spiker itself turns around 1.5625
//     above the rim and never touches the claw.
//
// So the arcade families own the test, inside their own update(), and the
// shared block skips ALL of them (the predicate below is the whole roster, not
// just the three above). Who owns what, as of the wave-2 families:
//
//   * the FLIPPER through ArcadeFlipper::threatLane -> tryGrab, and the BEAST
//     through the same code (enemies.cpp dispatches ARCADE_BEAST to
//     ArcadeFlipper::update);
//   * the FUSEBALL through its own file-local tryGrab in arcade_fuseball.cpp
//     -- NOT ArcadeFuseball::threatLane, which is an exposed query with no
//     caller;
//   * the PULSAR through ArcadePulsar::killsPlayerInLane (lane only, NO depth
//     window at all -- see that function), and the PULSAR SPARK through the
//     inline claw check at the top of its own update (same lane plus a real
//     depth window, CatchDz -- the asymmetry is deliberate);
//   * the TANKER (all three variants) and the SPIKER by not having one, for
//     the reasons bulleted above.
//
// The Mirror is deliberately absent from this list: its killsPlayer() is the
// reflected-SHOT-vs-player test and belongs to the shot path, not here.
// ---------------------------------------------------------------------------
inline bool enemy_uses_shared_player_collision(int id) { return !isArcadeEnemy(id); }

// ---------------------------------------------------------------------------
// 2b. WHO OWNS ENEMY-vs-PLAYER-BULLET
//
// The Mirror and the Beast do not TAKE DAMAGE from a bullet, they TAKE THE
// BULLET. Both run their own non-destructive same-lane search inside update()
// and then re-task what they find. So the shared exchange must not touch them:
// against the arcade life of 1 it would kill either in one hit AND consume the
// bullet the family was about to hand back.
//
// !! IT IS PER-ENEMY, NOT PER-ID, AND THE BEAST IS WHY. Its horn-shedding
//    search is `beastRail` -- the RAIL dispatch slot, and the WHOLE of the
//    Beast's own code. It is not reached in any other mode: once a Beast lands
//    it runs the ordinary flipper stopped handler, which in this port takes its
//    bullets from the SHARED sweep exactly as every other flipper does.
//
//    Excluding the Beast by ID alone therefore made a parked Beast
//    BULLET-PROOF IN EVERY MODE BUT ONE. Measured: a Beast that reached the rim
//    survived 3000 ticks of continuous fire, flipped to the player's lane and
//    took a life -- lethal, on screen, and unkillable by the only weapon the
//    player always has. (The superzapper still reached it, but two stocked uses
//    a level is not an answer for an enemy that appears from level 10 on.) The
//    control run says the shape of it plainly: an identical PLAIN flipper in
//    the same setup died to gunfire at tick 240 for 150 points.
//
//    So the exclusion is exactly as wide as the family's own test: RAIL only.
//    In RAIL the Beast sheds a horn and hands the bullet back; anywhere else it
//    is a plain flipper and one shot kills it, which is also what the recovery
//    says about its stopped behaviour ("stdstop -- identical, not the supers'
//    scarper"). The MIRROR needs no such gate: its search lives in its own
//    update() and runs in every live mode.
// ---------------------------------------------------------------------------
inline bool enemy_uses_shared_shot_collision(const Enemy& e) {
    if (e.id == ARCADE_MIRROR) return false;
    if (e.id == ARCADE_BEAST)
        return enemyfam::ArcadeFlipper::mode(e) != enemyfam::ArcadeFlipper::MODE_RAIL;
    return true;
}

// A reflected shot damages nothing: neither reference routine runs an enemy
// test at all.
inline bool shot_is_arcade_reflected(int shot_id) {
    return enemyfam::ArcadeReflectedShot::is(shot_id);
}

// ---------------------------------------------------------------------------
// 3. SCORE (audit sec 6/D4, arcade_enemies.md sec 3.6)
//
// `_handle_death` passes `enemy.life` as the explosion's `energy`, and
// `init_explosion` turns `energy` into SCORE. The arcade life is 1 -- which is
// load-bearing, it is what makes "one hit kills, the laser pierces" fall out
// of the shared mutual-subtraction exchange for free -- so without this every
// arcade kill scores ONE POINT and the whole roster reads as a scoring bug.
//
// The tanker's own landing path already passes ArcadeTanker::Score directly,
// so this is also what keeps "shot and landed are identical" true.
//
// !! TWO PROPERTIES OF THIS FUNCTION THAT USED TO HOLD AND NO LONGER DO, both
//    introduced by Wave 2 and both load-bearing:
//
//  a. IT IS NO LONGER PURE. The fuseball and the Mirror pay a UNIFORM RANDOM
//     250 / 500 / 750 -- recovered three independent ways in each case, and not
//     a number that can be a constant -- so those two rows consume ONE draw
//     from the shared rand() stream. Call it exactly ONCE per corpse. That is
//     why the shot-hit sites (collision.cpp, enemies.cpp) no longer raise their
//     own arcade explosion: two calls would be two rolls AND two payouts.
//
//  b. NOT EVERY VALUE IS UNDER init_explosion's 300 CLAMP any more. A 750 roll
//     used to arrive as 301 -- clamped in the award AND in the floating number
//     the player reads. init_explosion now routes an ARCADE enemy kill's score
//     around that clamp (and only the score; the explosion's own visual energy
//     stays bounded exactly as before). The classic roster is untouched, and is
//     bit-identical because for it the two values are the same number.
// ---------------------------------------------------------------------------
inline int arcade_kill_score(int id) {
    switch (id) {
    case ARCADE_FLIPPER:
    case ARCADE_SFLIPPER2:
    case ARCADE_SFLIPPER3:
    case ARCADE_BEAST:                                // the Beast IS a flipper
        return enemyfam::ArcadeFlipper::Score;        // 150
    case ARCADE_TANKER:
    case ARCADE_FUSE_TANKER:
    case ARCADE_PULSAR_TANKER:
        return enemyfam::ArcadeTanker::Score;         // 100
    case ARCADE_SPIKER:
        return enemyfam::ArcadeSpiker::KillScore;     // 150
    case ARCADE_PULSAR:
        return enemyfam::ArcadePulsar::Score;         // 200
    case ARCADE_PULSAR_SPARK:
        // 150, NOT the pulsar's 200: two different table entries, so a single
        // family-wide constant would be wrong. A pulsar allowed to reach the
        // rim ultimately pays 300 across its two sparks -- MORE than shooting
        // it in flight -- and that asymmetry is the recovered behaviour, not a
        // rounding to smooth out.
        return enemyfam::ArcadePulsarSpark::Score;    // 150
    case ARCADE_FUSEBALL:
        return enemyfam::ArcadeFuseball::rollScore();     // 250 / 500 / 750
    case ARCADE_MIRROR:
        return enemyfam::ArcadeMirror::rollDeathScore();  // 250 / 500 / 750
    case ARCADE_ADROID:
        // The generic blowmeaway bonus -- the same 250 / 500 / 750 the Mirror
        // and the fuseball pay, and it CONSUMES ONE rand() DRAW (sec 3a).
        return enemyfam::ArcadeAdroid::rollDeathScore();
    default:
        return 0;
    }
}

// ---------------------------------------------------------------------------
// 4. SHOT CONSUMPTION (arcade_enemies.md sec 3.5)
//
// The shared exchange subtracts the enemy's ORIGINAL life from the shot, so
// against the arcade life of 1 a PLAYER_SHOT1 (life 100) survives a confirmed
// kill at 99 and flies on to kill the next thing in the lane. The arcade rule
// is that an ordinary claw shot is CONSUMED by the kill and only the particle
// LASER punches through -- which is exactly what makes the laser worth having
// (it can open several tankers in one pass).
//
// PLAYER_SHOT2 is the laser. POWERUP_SHOT never damages anything, so it can
// never reach a confirmed kill and does not need excluding here.
// ---------------------------------------------------------------------------
inline bool arcade_kill_consumes_shot(int shot_id) { return shot_id != PLAYER_SHOT2; }

// ---------------------------------------------------------------------------
// 5. THE CUSTOM CORPSE (arcade_tanker.h registration items 2 and 3)
//
// Two shared paths kill an enemy WITHOUT going through its update(): the shot
// death handler (enemies.cpp _handle_death) and the superzapper's lethal arm
// (weapons.cpp move_zapper). A tanker splits on all three of shot / zapped /
// landed, so both of those have to reach the family. Neither is a hot path:
// this runs once per corpse.
//
// It releases the children and NOTHING else -- no removal, no score, no
// enemies_todo[] -- exactly like the container hatch beside it. The caller
// owns all three.
// ---------------------------------------------------------------------------
inline void arcade_enemy_die(GameEngine& engine, const enemyfam::EnemyCtx& ctx,
                          int lane, int idx, const Enemy& e) {
    switch (e.id) {
    case ARCADE_TANKER:
    case ARCADE_FUSE_TANKER:
    case ARCADE_PULSAR_TANKER:
        enemyfam::ArcadeTanker::die(engine, ctx, lane, idx, e);
        break;
    default:
        // The flipper and the spiker have no custom corpse: the shared
        // explosion/score/removal is the whole of their death.
        break;
    }
}

// ---------------------------------------------------------------------------
// 6. THE ONE GLOBAL ALIEN-FIRE TIMER (arcade_enemies.md sec 2.5)
//
// One 16-bit register in the reference: low byte a countdown, high byte a
// reload, 0x7070 = a 112-reference-tick period, never changed for the whole
// game. EVERY RAILING ENEMY DECREMENTS IT, so the fleet-wide fire rate scales
// with the number of descenders (period / N) rather than each enemy carrying
// its own cadence.
//
// EXACTLY THREE CALL SITES, verified by exhaustive search of the reference:
// the flipper's rail, the spiker's climb, and the spiker's descend. Flippers
// never fire while flipping or parked; the tanker never fires at all.
//
// The state is per-SET, so it lives on GameEngine (`arcade_fire_timer`, cleared
// in init_level) and not on any family. The POLICY -- decrement-and-reload
// BEFORE the gates, so a blocked attempt is lost rather than deferred, plus
// the depth gate and the concurrent-bullet cap -- lives in
// ArcadeFlipper::alienFireTick, because the flipper is where it was recovered.
// This wrapper exists so all three sites spell the call the same way and none
// of them has to name the engine field.
// ---------------------------------------------------------------------------
inline void arcade_alien_fire_tick(GameEngine& engine, int lane, float z) {
    enemyfam::ArcadeFlipper::alienFireTick(engine, engine.arcade_fire_timer, lane, z);
}

// ---------------------------------------------------------------------------
// 7. CONSTRUCTION ON THE SHARED SPAWN PATH
//
// `GameEngine::init_enemy` is the one place an embryo becomes a live enemy, and
// it rolls THIS ENGINE'S OWN randoms into `sidestep_freq`, `shoot_freq`,
// `max_animation` and `oo_max_animation`. On an arcade enemy those are not
// animation counters -- they are family state (each family header carries the
// map). Left alone, an arcade release through the embryo path would get:
//
//   * FLIPPER: a random SUB-VARIANT in 1..10, so roughly one flipper in ten
//     would be a super-3 fleeing the player, and -- because pauseReloadFor()
//     returns -1 for every value that is not SUB_PLAIN -- EVERY flipper would
//     have a pause reload of -1 and never pause. The recovered 8-tick pause,
//     which is the whole cadence of the enemy, would silently not exist.
//   * SPIKER: a nonsense build allowance (harmless only because SEEK reloads
//     it) and a mode read from a random-free field.
//   * TANKER: correct, but by luck rather than by construction.
//
// So the family gets to say what a freshly released one of its own looks like.
// One integer compare on the classic path, and it answers "do nothing".
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// 7. THE ARCADE RELEASE -- "replace only the ARRIVAL"
//
// arcade_enemies.md §3.4 decides the split exactly: this port KEEPS its own
// population model (init_level's enemies_todo[], is_level_clear(),
// init_embrio's throttles -- because level length and the measured warp
// reachability depend on them) and REPLACES ONLY THE ARRIVAL: "no embryo, no
// init_embrio throttle -- instead a the later port-style release into the arrival dot,
// gated by the later port budget rule".
//
// THAT IS NOT A STYLE PREFERENCE, IT IS FORCED, and the reason is worth
// stating because it is invisible from either file alone:
//
//   `move_embrios` steps an embryo with `ENEMY_DZ[embryo.id] * 0.5f`, and
//   ENEMY_DZ IS 0.0f FOR EVERY ARCADE ID. That zero is deliberate and
//   load-bearing (constants.h static_asserts it): each arcade family steps its
//   own level-scaled z, and a non-zero descriptor row would make the shared
//   _apply_movement step it a SECOND time -- audit §5/M1, measured as a 98%
//   rail overspeed. But the embryo reads the same table. So an arcade embryo
//   NEVER DESCENDS, never reaches the hatch plane, never becomes an enemy --
//   and because is_level_clear() waits on `num_embrios`, THE LEVEL HANGS
//   FOREVER WITH ONE INVISIBLE EMBRYO ON IT. Measured before this landed:
//   0 enemies and 3 stuck embryos after 800 ticks on level 22.
//
// So an arcade id never takes the embryo path at all. It goes straight to its
// family's own release, which is what those functions were written for and
// which is also the only place three recovered rules are enforced:
//   * the flipper spawns at a UNIFORM-RANDOM lane (the caller's, §2.1 S-1);
//   * the tanker NEVER spawns in the first or last lane (§2.2 -- load-bearing:
//     it is what keeps both of openChildren's end-lane fallthroughs
//     unreachable), and costs 3 of the 12-unit population budget;
//   * the spiker holds the ONE-SPIKER SEMAPHORE and picks its own lane.
//
// Each family owns BOTH halves of the accounting (enemies_nums += 1 and
// enemies_todo -= 1, together, after every refusal test), so the caller must
// not decrement enemies_todo itself. A refusal changes nothing at all and the
// release simply retries on a later tick -- no counter is stranded and no
// level can hang on one.
// ---------------------------------------------------------------------------
inline bool arcade_release(GameEngine& engine, int id, int lane) {
    switch (id) {
    case ARCADE_FLIPPER:
    case ARCADE_SFLIPPER2:
    case ARCADE_SFLIPPER3:
        // Uniform-random lane: the caller's `rand() % lane_count` is exactly
        // the recovered rule, so it is used as given.
        return enemyfam::ArcadeFlipper::spawn(engine, lane, id);
    case ARCADE_TANKER:
    case ARCADE_FUSE_TANKER:
    case ARCADE_PULSAR_TANKER:
        // The caller's lane is DISCARDED on purpose -- see the end-lane rule
        // above. pickSpawnLane returns -1 on a web with no interior lane, and
        // spawn() refuses that before touching anything.
        return enemyfam::ArcadeTanker::spawn(engine, id,
                                          enemyfam::ArcadeTanker::pickSpawnLane(engine));
    case ARCADE_BEAST:
        return enemyfam::ArcadeFlipper::spawn(engine, lane, id);
    case ARCADE_MIRROR:
        return enemyfam::ArcadeMirror::spawn(engine, lane);
    case ARCADE_SPIKER:
        // Also picks its own lane (uniform over the WHOLE web, both ends
        // included -- it has no children to keep apart).
        return enemyfam::ArcadeSpiker::spawn(engine);
    case ARCADE_FUSEBALL:
        // Uniform-random lane over the WHOLE web, both ends included: no
        // end-lane restriction, because a fuseball has no children to keep
        // apart. The caller's lane is the recovered rule; pass it through.
        return enemyfam::ArcadeFuseball::spawn(engine, lane);
    case ARCADE_PULSAR:
        // Same -- the reference's pulsar maker deliberately does NOT exclude
        // the end lanes (only the tanker's does, and there it is load-bearing).
        //
        // ARCADE_PULSAR_SPARK GETS NO CASE, AND MUST NEVER GET ONE. A spark is
        // never released by the generator, only produced by a rim split, so
        // `default: return false;` is the correct answer for it -- and its
        // spawn-mask bit staying clear is what enemy_spawns.h asserts.
        return enemyfam::ArcadePulsar::spawn(engine, lane);
    case ARCADE_ADROID:
        // Uniform-random lane (the caller's), like the Mirror: the saucer has no
        // children to keep off the end lanes, so the caller's lane is the rule.
        return enemyfam::ArcadeAdroid::spawn(engine, lane);
    default:
        // A Wave-2 id declared ahead of its family. Refusing is correct and
        // safe: nothing is released, nothing is charged, and the spawn-table
        // static_asserts already stop such a bit from being set.
        return false;
    }
}

} // namespace ts
