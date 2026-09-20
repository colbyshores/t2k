#pragma once
// ============================================================================
// arcade_fuseball.h -- THE FUSEBALL. Id ARCADE_FUSEBALL, one family, no
// sub-variants. Implements the enemy_family.h contract; read that header first.
//
// SPEC: docs/design/arcade_fuseball_recovery.md (129 file:line citations, and
// the ONLY authority for this family). Shared-path map:
// docs/design/enemy_pipeline_audit.md. Unit conversion and the shared/per-set
// split: docs/design/arcade_enemies.md §1 / §3.4 / §3.5 / §3.6.
//
// PROVENANCE AND THE IP BOUNDARY. Behaviour, orderings and numeric constants
// were READ from the two reference trees -- the arcade reference, which
// DOCTRINE.md makes the AUTHORITATIVE baseline, cross-checked against the later
// port -- both of which are gitignored, and NOTHING is copied: not a line, not
// a table, not a vertex. Everything below is re-authored in this engine's
// units. Same boundary DOCTRINE.md already applies to the claw, the camera and
// the powerup ladder, and the same explicit exception to "MECHANICS ARE
// GATED": the "original" being matched here is the arcade reference, not this
// engine's own shipped exe, so neither `fidelity-verifier` nor
// `exe-comparator` may correct any of it back toward the reference source /
// the shipped PC port.exe.
//
// ============================================================================
// THE FIVE THINGS AN IMPLEMENTER MUST NOT GET WRONG
// (recovery §7; each is expressed by exactly one thing in this file)
// ============================================================================
//
//  1. IT RIDES RAILS, NOT LANES. Its resting position is a lane's LEFT BORDER
//     VERTEX -- every other ground enemy in either roster sits on the lane
//     MIDPOINT. Expressed as `Enemy::anchor = ANCHOR_RAIL` with
//     `Enemy::cross_t` as the position across the lane (models.h names both
//     for exactly this family). Put it on a midpoint and you have built a
//     slow flipper.
//
//  2. DESCENT AND LATERAL MOTION NEVER OVERLAP. It is a two-state ALTERNATION,
//     never a diagonal: 49 reference ticks descending IN PLACE on a rail, then
//     32 reference ticks crossing ONE lane at FROZEN depth, repeat. The
//     stop-start IS the silhouette of the enemy, and it means the effective
//     descent rate is only 60.5% of the table speed. Do not smooth it.
//
//  3. PARKED = INVULNERABLE *AND* HARMLESS. This is the single most important
//     fact in the recovery and it REFUTES an earlier claim in this project
//     that a parked fuseball threatens both adjacent lanes. The climb routine
//     contains NO collision call of ANY kind -- no shot test, no player test.
//     Both live only in the crossing routine. So a parked fuseball threatens
//     NEITHER adjacent lane and CANNOT BE SHOT, and nothing about its
//     animation changes to say so, which is precisely what makes fuseballs
//     frightening. See shootable() and the CROSS half of update().
//
//  4. THE SHOT WINDOW IS THE MIDDLE 9/16 OF THE CROSSING, AND IT IS
//     ASYMMETRIC: step counter 4..12 of a counter that runs 15..0, i.e. the
//     fuseball occupying 3/16 through 11/16 of the lane. Not "the middle 8".
//     The PLAYER-KILL test is NOT windowed -- it is live for the whole
//     crossing. Lane position changes the shot answer, never the kill answer.
//
//  5. THE 16 CROSSING STEPS ARE EXACTLY THE LANE'S BORDER DELTA / 16. Do not
//     normalise and do not re-derive from an angle. This port's webs have
//     deliberately non-unit lane vectors (DOCTRINE.md: "NEVER re-normalize these
//     vectors at runtime") and the exact-division property is what makes a
//     crossing land dead on the next rail on every one of the 100 shipped
//     webs. Here that is structural rather than arithmetic: the position is
//     `cross_t = k/16` resolved through web_geometry.h's ONE lane accessor, so
//     there is no accumulation to drift and no second definition of where a
//     lane's borders are.
//
// AND THE SIXTH, WHICH IS NOT ABOUT GEOMETRY: THERE IS NO RANDOMNESS IN THE
// MOVEMENT AT ALL. Direction is pure lane-index arithmetic against the
// player's lane; the timers are compile-time constants; the step vector is
// exact geometry. The only rolls anywhere in the family are the spawn lane
// (the caller's), the per-frame leg flicker (presentation, and seeded from the
// frame counter rather than the shared stream) and the death bonus. A
// fuseball's path is fully determined by WHERE THE PLAYER STANDS. It is not
// erratic, it is relentless -- and that is the character of the enemy.
//
// ============================================================================
// THE CONFLICT WITH docs/design/arcade_enemies.md -- FOLLOW THE RECOVERY
// ============================================================================
//
// arcade_enemies.md §2.6 and §S2b state the cross delay is 4, giving 5 ticks
// per step and 71.2 engine ticks (1.14 s) per lane. THAT IS WRONG, and the
// recovery (§4.1) both corrects it and explains the cause: the level-init
// block chains THREE table lookups WITHOUT RELOADING THE REGISTER between
// them, so the cross delay is `crossdels[rospeeds[1]] = crossdels[4] = 1`, not
// `crossdels[1] = 4`. Corroborated by an already-shipped value: the first link
// of the same chain is what makes `ArcadeFlipper::FlipDegPerTick` 6.3236, a
// number this codebase already accepted.
//
//     fuse_crossdelay             doc says 4        recovered 1
//     reference ticks per step    doc says 5        recovered 2
//     ticks per lane crossing     doc says 80 ref   recovered 32 ref
//                                 (71.2 engine)     (28.5 engine, 0.455 s)
//
// The fuseball therefore crosses a lane 2.5x FASTER than the older document
// says, and because it then parks for 49 ticks, that document's number also
// makes the park look like a minor pause when it is in fact 60% of the cycle.
// arcade_enemies.md §2.6's "shootable during steps 4..11" is likewise the
// later port's window; the recovery and that document's own header table both
// give the authoritative 4..12.
//
// ============================================================================
// THE RAIL MODEL -- and the ONE place this port deliberately improves on the
// reference (recovery §3.2, which explicitly asks for this to be RECORDED)
// ============================================================================
//
// The web is a VERTEX RING: lane `i` spans vertex `i` -> vertex `i+1`, so a
// lane index and a RAIL index are the same number (models.h, web_geometry.h).
// A closed web has `n` rails; an OPEN web has `n+1` -- and the last one,
// rail `n`, is not any lane's left vertex. That single rail is where both
// reference trees go wrong, in different ways:
//
//   * the later port INCREMENTS THE LANE INDEX OUT OF RANGE and then reads the
//     endpoint table one entry past its end -- a confirmed defect, DO NOT PORT;
//   * the authoritative tree returns early instead: no increment, but also NO
//     RE-SNAP, so the fuseball is left standing on rail `n` while its index
//     still says `n-1`. The next left cross re-snaps it with a one-lane
//     visual pop, and a further right cross accumulates its motion vector from
//     rail `n` and walks it another lane OFF THE WEB ENTIRELY. Reachable (the
//     direction test sends a fuseball right on a tie), rare, cosmetic --- and
//     the recovery's own advice is "an implementer should clamp".
//
// THE CLAMP, and it costs nothing: `(lane, cross_t)` can already NAME rail `n`
// exactly -- it is `(n-1, 1.0)`, which web_geometry.h resolves to the web's
// last vertex with no special case. So this port lets a fuseball PARK there,
// and every rule below is written in terms of the RAIL it is on rather than
// the lane index it happens to be stored under:
//
//     parked on rail r <= n-1   ==   (lane r,   cross_t 0)
//     parked on rail n          ==   (lane n-1, cross_t 1)   [open web only]
//     crossing right from r     ==   (lane r,   cross_t 0 -> 1)
//     crossing left  from r     ==   (lane r-1, cross_t 1 -> 0)
//
// The last two lines are EXACTLY the reference's own rule ("right-cross keeps
// index i and traverses lane i; left-cross pre-decrements to i-1 and traverses
// lane i-1"), and for every rail the reference can name, rail index == lane
// index -- so this is a strict generalisation, identical everywhere the
// reference is defined and defined where the reference is not.
//
// Three consequences worth stating, because each removes a defect rather than
// adding a behaviour:
//   (a) a right cross INTO rail n still happens, so the crossing's threat and
//       its shot window are unchanged -- refusing the cross outright would
//       have made a fuseball at the right edge of an open web permanently
//       harmless, which is a worse divergence than the one being fixed;
//   (b) a left cross OUT of rail n traverses lane n-1 with the index already
//       correct, so the reference's one-lane pop cannot happen;
//   (c) the direction test evaluated on the rail index makes a fuseball parked
//       at rail n with the player in lane n-1 choose LEFT, so it PING-PONGS
//       across that lane exactly as the recovery describes a rimmed fuseball
//       doing ("stays there indefinitely, ping-ponging along the rails, until
//       shot or zapped") instead of parking harmlessly forever.
//
// The same state also absorbs the one case the reference never has to think
// about: a landing whose destination lane is at this port's own MAX_ENEMIES
// cap. It stays on the rail it reached, which is geometrically exact, and the
// next crossing re-normalises it.
//
// ---- THE ONE PLACE THE RE-SNAP IS NOT EXACT, AND IT IS PRE-EXISTING -------
//
// A right-cross landing re-expresses "lane i's right border" as "lane i+1's
// left border", which is the same ring vertex -- MEASURED over all 100 shipped
// webs / 1513 lanes: worst 9.5e-07, i.e. float noise, on every lane that does
// not touch the web's closure lane.
//
// It is NOT float noise on the two pairs that do. web_geometry.h's own
// "CLOSURE-LANE CAVEAT" records why: the last lane of a closed web is built by
// transformLevel as ring[0] - ring[n-1] while the +-half-step convention this
// port has always used takes the STORED step, and five Tsunami-provenance webs
// do not quite close. The error lands on BOTH borders of that lane, so both
// the (n-2, n-1) pair and the (n-1, 0) pair are affected: worst 0.327 world
// units, on level 31. So on those five webs a fuseball's landing re-snap will
// visibly POP by up to a third of a lane at the closure rail -- exactly as the
// claw's feet already sit up to a third of a lane off the ring there, and
// exactly the hazard web_geometry.h flags for the flipper's hinge. This family
// does not fix it: switching webLane's borders to the ring on the closure lane
// is a behaviour change to the CLAW and the SPIKES and must be gated as one,
// not slipped in behind a new enemy.
//
// ============================================================================
// WHERE THE PER-ENEMY STATE LIVES, AND WHY IT IS NOT A UNION
// ============================================================================
//
// arcade_enemies.md §3.3 specifies a named `ArcadeState` union on `Enemy`.
// That is a models.h change and models.h is a shared file this wave may not
// edit -- several arcade families are being authored in PARALLEL and each
// adding a member to the same union is a guaranteed collision. So this family
// maps its state onto `Enemy`'s existing generic fields through the NAMED
// ACCESSORS at the bottom of this struct. They are the ONLY place the mapping
// is spelled out; when the union lands, the accessor bodies change and not one
// line of arcade_fuseball.cpp moves.
//
//     Enemy::sidestep_freq     -> mode      (Mode)
//     Enemy::shoot_freq        -> step      (the 15..-1 crossing counter)
//     Enemy::animation_vec     -> dir       (+1 right / -1 left; the
//                                            reference's "change lanes after
//                                            crossing" flag, as a sign)
//     Enemy::oo_max_animation  -> timer     (FLOAT countdown, engine ticks)
//     Enemy::max_animation     -> stamp     (last-updated tick; see below)
//     Enemy::anchor_rot        -> spin      (body angle, degrees)
//     Enemy::cross_t           -> the position across `lane`  (models.h's own
//                                 contract for this family; used directly, not
//                                 through an accessor, because models.h
//                                 already names it)
//     Enemy::anchor            -> ANCHOR_RAIL, ALWAYS -- parked and crossing
//     Enemy::px, py, pivot_side, animation_phase, current_lane -> UNUSED, 0
//
// !! CONSEQUENCE FOR THE RENDERING WAVE, and it is the same one arcade_flipper.h
//    records: `oo_max_animation`, `max_animation` and `animation_vec` are NOT
//    animation counters on a fuseball. entity_geometry.cpp's GENERIC per-enemy
//    animation block (the `animation_phase * oo_max_animation` wobble and the
//    `animation_phase < max_animation` test) must never run for an arcade id.
//    It cannot: `buildEnemies` branches on isArcadeEnemy() BEFORE that block,
//    routes arcade ids through getArcadeEnemyGeometry / enemyModelMatrixArcade
//    and `continue`s, so getEnemyGeometry is never reached for one at all.
//    (The per-enemy isArcadeEnemy() branch predates this family entirely, so
//    it superseded nothing here -- an arcade id has never reached
//    getEnemyGeometry. The guard that actually skips a rowless arcade id is
//    the null-ArcadeGeometry `continue` inside the arcade branch itself
//    (entity_geometry.cpp:1004). getEnemyGeometry still has no arcade case,
//    so that fallback remains true, it is simply not what protects this.) That
//    routing has LANDED for this family: entity_geometry.cpp carries the
//    fuseball's geometry row -- five blades via ArcadeGeometry::copies /
//    copyStepDeg -- and its `enemyModelMatrixArcade` case, which already reads
//    `enemyAnchorPos()` and therefore already handles ANCHOR_RAIL correctly
//    with no new code.
//
//    NB `animation_phase` is deliberately left at 0 and unused: enemies.cpp's
//    shared `_transfer_enemy` NEGATES it on every transfer (audit §7/R3), so
//    it is the one field an arcade family must not store meaning in. This
//    family does its own lane move anyway (see arcade_fuseball.cpp), but
//    storing nothing there makes the family immune for free.
//
// WHY THE TICK STAMP EXISTS. `move_enemies` walks lanes 0..n-1 in order, so an
// enemy this family moves to a HIGHER lane index is visited AGAIN in the same
// frame and would take a second action. A right cross does exactly that when
// it lands. The stamp makes each fuseball act exactly once per tick. Delete it
// the day `move_enemies` grows a transfer guard of its own -- and note the
// flipper carries the identical mechanism for the identical reason.
// ============================================================================

#include "enemy_family.h"
// For ParkedGrabDz ONLY. The 2-world-z enemy-catches-player window is a
// ROSTER-WIDE adjudication (DOCTRINE.md, "ENEMY BEHAVIOUR", conflict 2), not a
// flipper property, and this project has already shipped it wrong once by
// keeping two copies of it. Binding to the flipper's constant means the two
// families cannot drift; the static_assert at the bottom of this header means
// neither can be edited silently.
#include "arcade_flipper.h"

#include <cstdint>

namespace ts {
namespace enemyfam {

struct ArcadeFuseball {
    // The ids this family claims (enemy_family.h step 1). The `switch` in
    // enemies.cpp move_enemies is the single dispatch and must agree.
    static constexpr int Ids[] = { ARCADE_FUSEBALL };

    // =====================================================================
    // MODES. The reference dispatches through a 3-entry vector table; its
    // third entry is the SHARED bonus-object update the corpse is converted
    // INTO, not a fuseball behaviour, so there are exactly two live states
    // here plus this port's representation of the far-dot approach.
    // =====================================================================
    enum Mode {
        // The inert far dot. The reference does not run the fuseball's update
        // at all during this phase -- the shared object runner closes the
        // distance and returns -- so the fuseball does not think, does not
        // spin, and has no collision test of any kind. Same shape as
        // ArcadeFlipper::MODE_ARRIVAL and ArcadeTanker::MODE_APPROACH.
        MODE_ARRIVAL = 0,
        // Descend IN PLACE on a rail. The ONLY state in which depth changes,
        // and the state with NO collision call of any kind (headline fact 3).
        MODE_CLIMB   = 1,
        // Traverse ONE lane at FROZEN depth, in 16 exact steps. The ONLY
        // state in which the fuseball can be shot or can take the player.
        MODE_CROSS   = 2,
    };

    // =====================================================================
    // UNITS (arcade_enemies.md §1). Deliberately LOCAL to this family rather
    // than in a shared arcade_common.h, for the same reason arcade_spiker.h
    // states: enemy_family.h step 2 puts a family's constants next to the code
    // that reads them, and several arcade families are being authored in
    // parallel -- a shared header would be a merge conflict, not a shared
    // truth. Hoist them only in a change that does nothing else.
    // =====================================================================

    // The reference logic tick. Not vsync-locked: the timer is reprogrammed
    // with divisor 0x4256, giving 1193181.67/16982 = 70.2617 Hz.
    static constexpr float ARCADE_HZ = 70.2617f;
    // This engine's tick (TICK_MS = 16; game_step.cpp drains dif_time in
    // TICK_MS steps, the same structure the reference main loop has).
    static constexpr float ENGINE_HZ = 62.5f;
    // WALL CLOCK IS WHAT IS PRESERVED, NOT TICK COUNTS (arcade_enemies.md
    // §1.1). Everything else in this port -- the claw ramp, the camera spring,
    // the droid glide, the shatter lifetimes -- is tuned in engine ticks
    // against real seconds, so a roster running 12% slow would read as
    // sluggish rather than as faithful.
    static constexpr float TICK_RATE  = ARCADE_HZ / ENGINE_HZ;   // 1.1241872: scale a per-tick DELTA
    static constexpr float TICK_COUNT = ENGINE_HZ / ARCADE_HZ;   // 0.8895328: scale a tick COUNT

    // DEPTH. The reference tube is 160 length units ("world-z") deep, stored
    // as 5120 raw at 32 raw per unit; this engine's is GRID_ELEMENT_LENGTH,
    // z = 25 (far) down to z = 0 (rim). PROPORTIONAL, and deliberately NOT
    // DOCTRINE.md's camera law of "1 port unit == 256 reference units" -- they
    // map different things and arcade_enemies.md §1.2 says to keep them in
    // separate named constants so nobody folds them.
    static constexpr float UNIT_Z = GRID_ELEMENT_LENGTH / 160.0f;    // 0.15625 engine z per world-z
    static constexpr float RAW_Z  = GRID_ELEMENT_LENGTH / 5120.0f;   // 0.0048828125 engine z per raw unit

    // =====================================================================
    // GEOMETRY OF THE TUBE
    // =====================================================================
    static constexpr float FarPlaneZ = GRID_ELEMENT_LENGTH;   // 25.0
    static constexpr float RimPlaneZ = 0.0f;

    // ---- the far dot (recovery §1.1(a)) ---------------------------------
    // Spawned at the far plane pushed a further 300 world-z back, with the
    // fractional half explicitly cleared: 25 + 300*0.15625 = 71.875. IDENTICAL
    // to the flipper and the tanker, which is the point -- the arrival dot is
    // the roster's signature and every family that has one uses the same one.
    static constexpr float SpawnZ = 71.875f;
    // The shared object runner closes 2 world-z per reference tick.
    static constexpr float ArrivalDz = 2.0f * UNIT_Z * TICK_RATE;   // 0.3513085 z/tick
    // Informational, and the harness checks it: 46.875 / 0.3513085 = 133.4
    // engine ticks = 2.135 s (the reference: 150 of its own ticks).
    static constexpr float ArrivalTicks = 133.42f;

    // !! THE LATER PORT'S SPAWN-DEPTH DEFECT IS *NOT* PORTED (recovery §3.1).
    // That build writes the far-plane value as a DWORD two bytes high, so the
    // integer half of the depth field receives the value's low word (zero) and
    // the high word lands on the LANE index. Consequences, exactly, because it
    // is easy to mis-describe: the LANE CLOBBER IS HARMLESS (the random lane
    // overwrites it two instructions later) and THE DEPTH IS THE REAL DAMAGE
    // -- the fuseball materialises at 300 world-z instead of 502, cutting the
    // approach from ~150 reference ticks to ~49, so it appears about 1.4 s
    // early and much closer in. SpawnZ above is the authoritative tree's
    // correct value. Do not "simplify" it toward the other one.

    // =====================================================================
    // STATE 0 -- CLIMB (descend, parked on a rail)
    // =====================================================================
    // The dwell counter is a compile-time word, both of whose bytes are 0x30,
    // and it is NEVER recomputed at runtime (verified: the only readers are
    // the two construction paths). High byte -> counter, low byte -> reload,
    // so both are 48; the countdown runs 48 -> -1, which is 49 ticks, and the
    // 49th tick is the one that both descends AND leaves for the crossing.
    static constexpr int   RiseArcadeTicks = 49;
    static constexpr float RiseTicks = RiseArcadeTicks * TICK_COUNT;   // 43.587 engine ticks / 0.697 s

    // THE RIM CLAMP IS ONE-SIDED, and that is load-bearing: the descent is
    // gated on `z >= rim` BEFORE the step, so z may UNDERSHOOT the rim plane
    // by at most one step and then stops. That undershoot is exactly what
    // makes the player-kill test (`z <= rim`) live once the fuseball has
    // arrived. A two-sided clamp to 0 would leave the kill test on a knife
    // edge of float equality.

    // =====================================================================
    // STATE 1 -- CROSS (traverse one lane at frozen depth)
    // =====================================================================
    // 16 steps, a step counter that starts at 15 and ends the crossing when it
    // underflows past 0, and a per-step delay of `fuse_crossdelay`.
    static constexpr int CrossSteps = 16;
    static constexpr int StepStart  = CrossSteps - 1;   // 15

    // THE CORRECTED CROSS DELAY -- 1, NOT 4. See "THE CONFLICT WITH
    // arcade_enemies.md" at the top of this file for the derivation and for
    // the already-shipped value that corroborates the index chain. The
    // countdown pattern is 1, 0, -1, i.e. ONE STEP EVERY TWO REFERENCE TICKS.
    static constexpr int   CrossDelay              = 1;
    static constexpr int   CrossArcadeTicksPerStep = CrossDelay + 1;                     // 2
    static constexpr float CrossStepTicks = CrossArcadeTicksPerStep * TICK_COUNT;        // 1.779 engine ticks
    static constexpr float CrossTicks     = CrossSteps * CrossStepTicks;                 // 28.465 / 0.455 s
    // The full alternation: 49 + 32 = 81 reference ticks, of which only 60.5%
    // is spent descending. Exposed so the harness can assert the duty cycle
    // rather than re-deriving it.
    static constexpr float CycleTicks   = RiseTicks + CrossTicks;         // 72.05 / 1.153 s
    static constexpr float DescentDuty  = RiseTicks / CycleTicks;         // 0.6049

    // ---- THE SHOT WINDOW (recovery §1.3, §7.4) ---------------------------
    // Tested at the TOP of the crossing routine, on the step counter BEFORE
    // this tick's decrement, and the position is advanced BEFORE the decrement
    // -- so a counter value of `s` means the fuseball occupies (15 - s)/16 of
    // the lane, and the window 4..12 is 3/16 through 11/16: the middle NINE of
    // sixteen positions. The source's own comment on it is "only kill if in
    // lane centre".
    //
    // The later port's window is 4..11 (one step narrower, 8 of 16) because it
    // compares with a >= where the authoritative tree uses a >. Adjudicated in
    // the authoritative tree's favour by both the recovery §3.3 and
    // arcade_enemies.md's own header table.
    static constexpr int ShotStepLo = 4;
    static constexpr int ShotStepHi = 12;

    // ---- THE PLAYER-KILL TEST (recovery §1.5, §7.9) ----------------------
    // NOT windowed. It sits after the shot window's skip label, so it is live
    // for the WHOLE crossing and dead for the whole park. It is not a distance
    // test: it needs THREE conditions plus a suppression --
    //     * the same LANE INDEX, exact integer compare, no lateral geometry;
    //     * |enemy.z - claw.z| <= 2 world-z, INCLUSIVE;
    //     * enemy.z at or past the rim plane;
    //     * and the level-end zoom not already running.
    //
    // THE ADJUDICATED DEPTH WINDOW. 2 world-z is DOCTRINE.md's already-settled
    // conflict 2, independently re-confirmed by this recovery: the later port
    // scales this ONE test by the X/Y factor where every other depth constant
    // in its own file -- its bullet test included -- uses the Z factor,
    // landing at 8 world-z and letting enemies grab from FOUR TIMES further
    // down the tube. Bound to the flipper's constant (see the include note) so
    // the two families cannot disagree about a number this project has already
    // shipped wrong once.
    static constexpr float GrabDz = ArcadeFlipper::ParkedGrabDz;   // 0.3125 engine z

    // =====================================================================
    // DESCENT SPEED -- the ONLY level-scaled quantity on this family
    // =====================================================================
    // 56 entries indexed by `level / 2` (consecutive level PAIRS share a
    // value), covering levels 0..111; the reference relies on its own wave
    // wrap to stay in range and this port CLAMPS instead, which is a real
    // difference only above level 111 where the reference would read past the
    // table. Stored in the later port's raw 1/32-unit steps because that is
    // the form both trees agree on bit-for-bit (the authoritative tree spells
    // the first entry as a 16.16 fixed 0.5, which is 16/32), and because it is
    // how arcade_spiker.h already spells its own table.
    //
    // THE TABLE IS NOT MONOTONIC. It ramps up within a row and then STEPS DOWN
    // at the start of FIVE of its seven rows, and the last row additionally
    // skips a rung outright (26 then 29, with 27 and 28 absent). That is the
    // shipped data in both trees -- do not "fix" it into a monotone ramp, and
    // do not be surprised that level 65 is slower than level 64.
    //
    // NB the recovery's prose (sec 1.2 "Depth rate") says the table steps down
    // at "the start of the 3rd, 4th and 5th rows and again at the 7th" -- FOUR.
    // The rows below are the source data and they step down FIVE times: the
    // 6th row's 27 -> 24 is missing from that sentence. Harness-measured, and
    // recorded here so the next reader does not "correct" the table to match
    // the prose.
    static constexpr int SpeedTableLen = 56;
    static constexpr int SpeedRaw[SpeedTableLen] = {
        // idx  0.. 7  -- levels  1..16
        16, 16, 16, 16, 16, 16, 17, 18,
        // idx  8..15  -- levels 17..32
        18, 19, 20, 21, 22, 23, 24, 25,
        // idx 16..23  -- levels 33..48   (steps DOWN from 25 to 20)
        20, 21, 22, 23, 24, 25, 26, 27,
        // idx 24..31  -- levels 49..64   (steps DOWN from 27 to 20)
        20, 21, 22, 23, 24, 25, 26, 27,
        // idx 32..39  -- levels 65..80   (steps DOWN from 27 to 20)
        20, 21, 22, 23, 24, 25, 26, 27,
        // idx 40..47  -- levels 81..96
        24, 25, 26, 27, 28, 29, 30, 31,
        // idx 48..55  -- levels 97..112  (steps DOWN, then skips a rung)
        26, 29, 30, 31, 32, 33, 34, 35,
    };

    // This level's raw step. `level` is engine.current_level (0-BASED; the HUD
    // adds one -- passing the HUD's number would shift the whole ramp by half
    // a table row).
    static int   speedRaw(int level);
    // ... the same step as engine z per ENGINE tick (a per-tick DELTA, so it
    // takes the rate conversion). 16 raw -> 0.087827; 35 raw -> 0.192122.
    static float descentDz(int level);
    // ... and the EFFECTIVE descent, which is what actually matters for how
    // long a fuseball takes to reach the rim: the table speed times the 60.5%
    // duty cycle, because a crossing fuseball does not descend at all.
    static float effectiveDz(int level);

    // =====================================================================
    // SCORING (recovery §1.6, §7.10)
    // =====================================================================
    // Killing one pays 250, 500 or 750 UNIFORMLY AT RANDOM -- confirmed three
    // independent ways (the score-table indices, the three bonus-graphic
    // objects, and the later port's own explicit three-word table). It is NOT
    // depth-dependent and it is NOT a fixed number, which makes this the one
    // arcade family whose kill score cannot be a constant.
    static constexpr int ScoreMin     = 250;
    static constexpr int ScoreStep    = 250;
    static constexpr int ScoreChoices = 3;
    // Rolls one of the three. Called ONCE per corpse, from the shared
    // `arcade_kill_score()` seam (enemies_shared.h) -- see the REGISTRATION
    // list at the bottom of this header, and note that it makes that
    // otherwise-pure switch consume one draw from the shared rand() stream for
    // this id and this id only.
    static int rollScore();

    // A KILL CONSUMES THE SHOT, except the particle laser, which passes
    // through and can chain several fuseballs in one pass. That is already the
    // shared rule (`arcade_kill_consumes_shot`, enemies_shared.h §4) and it
    // needs nothing from this family -- stated so a reader does not go looking
    // for it here.

    // =====================================================================
    // PRESENTATION -- the numbers the RENDERER needs, kept here so it cannot
    // invent a second copy (DOCTRINE.md: put the math in a shared builder and
    // leave only submission in the backend file; the HUD lives icon is what
    // happens when that rule is skipped).
    // =====================================================================

    // BODY SPIN. A constant 2/256 of a revolution per reference tick, added in
    // EVERY live mode -- so a fuseball looks identical parked and crossing,
    // which is what makes fact 3 invisible to the player. 2.8125 deg per
    // reference tick x TICK_RATE = 3.1618 deg/engine tick, one revolution per
    // 113.9 ticks / 1.822 s. (Identical to the rate the later port's tanker
    // spins at, which is a coincidence of the same constant, not a shared
    // mechanism.)
    static constexpr float SpinDegPerTick = 2.8125f * TICK_RATE;   // 3.16177

    // THE ROSETTE. Five copies of ONE curved blade about the body centre,
    // stepped by 51/256 of a revolution between copies -- the integer floor of
    // 72 degrees, so the five legs span 358.594 deg and leave a deliberate
    // ~1.4 deg seam. Both trees use the same angle to five decimal places.
    static constexpr int   LegCount   = 5;
    static constexpr float LegStepDeg = 51.0f * 360.0f / 256.0f;   // 71.71875

    // THE LEG FLICKER. Each leg independently picks one of TWO forms that are
    // exact mirrors of each other about the blade's baseline -- that mirror
    // pair is the WHOLE animation. The generator is reseeded from the frame
    // counter at the top of the draw routine and restored afterwards, so the
    // pattern is a PURE FUNCTION of the frame counter and HOLDS STILL between
    // reseeds. Do not roll it per frame from a free-running RNG: it would read
    // as noise instead of crackle, and it would also perturb the shared rand()
    // stream from the render path, which is not allowed to affect simulation.
    //
    // The authoritative tree reseeds every 4 video frames (15 Hz); the later
    // port every 8 of its ticks (8.78 Hz, visibly slower). Authoritative wins:
    // every 4 engine ticks = 64 ms = 15.625 Hz.
    static constexpr int FlickerPeriodTicks = 4;
    // The engine tick in ms. game_step.cpp's own `TICK_MS` is a file-static
    // there and not reachable from a header, so this restates it -- and the
    // static_assert at the bottom of this file pins it to ENGINE_HZ, which is
    // the number the rest of this family converts against, so the two cannot
    // drift apart silently.
    static constexpr int EngineTickMs    = 16;
    static constexpr int FlickerPeriodMs = FlickerPeriodTicks * EngineTickMs;   // 64 ms = 15.625 Hz

    // Which of the mirrored blade forms leg `leg` (0..LegCount-1) shows at
    // engine time `time_ms`. Pure, allocation-free, no RNG state, no per-enemy
    // storage -- and identical on both backends by construction, which is the
    // point of it living here rather than in either renderer.
    static bool legMirrored(int time_ms, int leg);

    // COLOUR. A fuseball is NOT one colour: it is a five-colour rosette, one
    // FIXED colour per angular slot, and there is no cycling at all -- the
    // BODY rotates, so an observer sees the five colours sweep round, and that
    // is the entire colour animation. Slot order from the base angle upward is
    // RED, GREEN, PURPLE, YELLOW, CYAN; both trees store the table in opposite
    // orders and walk it in opposite directions, so the colour-to-angle
    // assignment is identical. The object's own stored colour (cyan) is
    // essentially never on screen -- the solid draw routine ignores it
    // entirely. The actual RGB values belong in data/enemy_data_arcade.h with
    // the other arcade palettes; see the REGISTRATION list.

    // =====================================================================
    // ENTRY POINTS
    // =====================================================================

    // The per-tick update. RETURN CONTRACT (enemy_family.h): true iff this
    // call REMOVED the enemy from `lane`'s slot `idx` -- and a lane transfer
    // COUNTS as removal, because the slot now holds a different enemy. The
    // caller must then neither touch `enemy` nor advance `idx`.
    //
    // This family transfers on a LEFT-cross setup and on a RIGHT-cross
    // landing, so it returns true on exactly those two kinds of tick.
    static bool update(GameEngine& engine, const EnemyCtx& ctx,
                       int lane, int idx, Enemy& enemy);

    // THE WAVE RELEASE (recovery §1.1(a)). Costs ONE object slot and takes a
    // UNIFORM-RANDOM lane over the WHOLE web -- unlike the tanker, there is NO
    // end-lane restriction, because a fuseball has no children to keep apart.
    // The caller's `rand() % lane_count` is exactly that rule, so it is used
    // as given. Returns false, having changed NOTHING, if it refuses.
    static bool spawn(GameEngine& engine, int lane);

    // THE FUSE-TANKER HATCH (recovery §2.2, §2.3). Two children, and the
    // geometry is fully determined by the parent's lane:
    //     dir +1 -> parked on the parent lane's RIGHT border, crossing further
    //               RIGHT  (the reference reaches this by TEMPORARILY
    //               incrementing the parent's lane index around the call)
    //     dir -1 -> parked on the parent lane's LEFT border, crossing further
    //               LEFT
    // so the pair appears on the two boundary lines of the tanker's lane and
    // moves APART -- the same silhouette as the flipper-tanker hatch by a
    // completely different mechanism.
    //
    // What a hatchling does NOT get, all four confirmed absences: no far dot
    // and no approach (the depth is copied from the parent VERBATIM), no
    // promotion roll, no "stop after one move" mark, and no colour
    // inheritance. It begins in MODE_CROSS on its first tick, which is why
    // this function performs the crossing setup itself rather than leaving the
    // child parked.
    //
    // NOT charged against enemies_todo[] -- a hatchling is the tanker's
    // payload, not one of the level's planned releases -- but it DOES
    // increment enemies_nums[], which is what balances the shared
    // `_remove_enemy` (audit §7/R1).
    //
    // `ctx.time` is used ONLY for the once-per-tick stamp: the tanker calls
    // this from inside its own update, so the child must not also act on that
    // tick.
    static bool spawnHatchling(GameEngine& engine, const EnemyCtx& ctx,
                               int parentLane, float z, int dir);

    // THE VULNERABILITY PREDICATE (arcade_enemies.md §3.5, audit §2/C9).
    // This is what the shared `enemy_shootable()` must call, and headline fact
    // 3 lives here: false in ARRIVAL, false in CLIMB *at any depth*, and true
    // during a crossing only within the middle 9/16.
    //
    // !! IT IS ALSO WRONG FOR THE SUPERZAPPER, AND THAT IS WHY THIS PORT HAS
    //    TWO PREDICATES. The reference's fuseball update runs an UNCONDITIONAL
    //    collision call on any superzap frame, BEFORE dispatching to the climb
    //    or cross routine -- so the superzapper kills a fuseball in ANY mode,
    //    parked included. Answering that correctly for bullets answers it
    //    INCORRECTLY for the zapper, so this is the BULLET answer only: it
    //    serves the two shot sweeps (collision.cpp, enemies.cpp) through the
    //    shared `enemy_shootable()`. REGISTRATION item 6 HAS LANDED -- the
    //    tremor kill zone and the zapper target search (both weapons.cpp) ask
    //    `enemy_zappable()` instead, which routes this id to zappable() below.
    //    Do not re-merge them: a fuseball is unshootable ~75% of its life and
    //    NEVER lands and NEVER expires, so one predicate would let a single
    //    parked ball hold `is_level_clear()` open forever.
    static bool shootable(const Enemy& e);

    // What the SUPERZAPPER (and, by the same argument, the tremor) asks
    // instead: a fuseball is zappable in every mode except the inert far dot.
    // This is the live seam split -- enemies_shared.h's `enemy_zappable()`
    // dispatches ARCADE_FUSEBALL here and defers every other id to
    // `enemy_shootable()`.
    static bool zappable(const Enemy& e);

    // THE LANE THIS FUSEBALL THREATENS, or -1 for none. Parked: NONE, at any
    // depth, on either side (headline fact 3, and the refutation of the
    // two-lane claim -- see the note on the abandoned hook in
    // arcade_fuseball.cpp). Crossing: the lane it is physically INSIDE, which
    // is the lane index it is stored under, for the whole crossing.
    //
    // NO CALLER TODAY, and it is kept deliberately -- unlike
    // ArcadeFlipper::threatLane, which enemies_shared.h names as the flipper's
    // owner of the enemy-vs-player test. This family expresses the same rule
    // INLINE instead: the file-local `tryGrab` in arcade_fuseball.cpp runs
    // only from `crossTick` and compares `lane` against the claw's lane
    // directly, which IS "parked threatens nothing, crossing threatens the
    // lane it is stored under". What this query is retained for is the
    // refutation record in its body, not as a hook waiting to be wired; if the
    // two ever disagree, tryGrab is the live one.
    static int threatLane(const Enemy& e, int lane);

    // The direction this fuseball would choose from rail `rail` right now:
    // -1 = toward a lower rail index, +1 = toward a higher one. NEVER 0 --
    // the reference's test is a two-way branch with the tie going RIGHT, so a
    // fuseball always commits to a side. Exposed so the harness can prove
    // every branch without driving a tick.
    static int chooseDir(const GameEngine& engine, int rail);

    // The RAIL a PARKED fuseball is standing on. Equals `lane` in every
    // ordinary case; equals `lane + 1` only for the clamped last rail of an
    // open web (see "THE RAIL MODEL"). Meaningless mid-crossing.
    static int parkedRail(const GameEngine& engine, int lane, const Enemy& e);

    // The position across `lane` implied by the crossing step counter, exact
    // by construction: k/16 for integer k in 0..16, every one of which is
    // exactly representable in binary32. Exposed for the harness.
    static float crossTForStep(int stepCounter, int direction);

    // =====================================================================
    // FIELD ACCESSORS -- the ONLY place the scratch-field mapping is spelled
    // out (see the header comment). When arcade_enemies.md §3.3's `ArcadeState`
    // union lands, only these bodies change.
    // =====================================================================
    static int&   mode  (Enemy& e)       { return e.sidestep_freq; }
    static int    mode  (const Enemy& e) { return e.sidestep_freq; }
    static int&   step  (Enemy& e)       { return e.shoot_freq; }
    static int    step  (const Enemy& e) { return e.shoot_freq; }
    static int&   dir   (Enemy& e)       { return e.animation_vec; }
    static int    dir   (const Enemy& e) { return e.animation_vec; }
    static float& timer (Enemy& e)       { return e.oo_max_animation; }
    static float  timer (const Enemy& e) { return e.oo_max_animation; }
    static int&   stamp (Enemy& e)       { return e.max_animation; }
    static int    stamp (const Enemy& e) { return e.max_animation; }
    static float& spin  (Enemy& e)       { return e.anchor_rot; }
    static float  spin  (const Enemy& e) { return e.anchor_rot; }
};

// ---- Compile-time binding of the recovered numbers to the derived ones -----
// These exist so that editing ONE of a pair without the other fails at COMPILE
// time -- the only place a silent shear in a recovered constant is still cheap
// to catch. Same discipline as constants.h's "descriptor shear" asserts and
// arcade_spiker.h's adjudicated pair.
namespace fuseball_detail {
constexpr bool nearly(float a, float b, float eps) {
    return (a - b) < eps && (b - a) < eps;
}
}  // namespace fuseball_detail

static_assert(ArcadeFuseball::UNIT_Z == 0.15625f,
              "arcade->engine depth scale (GRID_ELEMENT_LENGTH / 160)");
static_assert(fuseball_detail::nearly(ArcadeFuseball::TICK_RATE, 1.12419f, 1e-5f),
              "tick conversion (a per-tick DELTA scale)");
static_assert(fuseball_detail::nearly(ArcadeFuseball::TICK_COUNT, 0.88953f, 1e-5f),
              "tick conversion (a tick COUNT scale)");
static_assert(fuseball_detail::nearly(ArcadeFuseball::SpawnZ, 71.875f, 1e-4f),
              "spawn depth: the far plane plus 300 world-z");
static_assert(fuseball_detail::nearly(ArcadeFuseball::ArrivalDz, 0.35131f, 1e-5f),
              "approach rate: 2 world-z per reference tick");
// The two approach numbers must agree, so neither can be 'corrected' alone.
static_assert(fuseball_detail::nearly(
                  (ArcadeFuseball::SpawnZ - ArcadeFuseball::FarPlaneZ) / ArcadeFuseball::ArrivalDz,
                  ArcadeFuseball::ArrivalTicks, 0.05f),
              "approach duration");
// THE CORRECTED CROSS DELAY, as a compile-time statement rather than a
// comment. If someone 'restores' arcade_enemies.md's 4, the build stops here
// and points at the derivation.
static_assert(ArcadeFuseball::CrossDelay == 1,
              "fuse_crossdelay is 1, NOT the 4 that arcade_enemies.md prints: the "
              "level-init block chains three table lookups without reloading the "
              "register, so the value is crossdels[rospeeds[1]] = crossdels[4] = 1. "
              "See docs/design/arcade_fuseball_recovery.md sec 4.1, and note that the "
              "FIRST link of the same chain is what makes ArcadeFlipper::FlipDegPerTick "
              "6.3236 -- a number this codebase already ships.");
static_assert(fuseball_detail::nearly(ArcadeFuseball::CrossTicks, 28.47f, 0.05f),
              "a lane crossing is 32 reference ticks / 28.5 engine ticks / 0.455 s");
static_assert(fuseball_detail::nearly(ArcadeFuseball::RiseTicks, 43.59f, 0.05f),
              "a rail dwell is 49 reference ticks / 43.6 engine ticks / 0.697 s");
static_assert(fuseball_detail::nearly(ArcadeFuseball::DescentDuty, 0.605f, 0.005f),
              "only 60.5% of the cycle is spent descending -- the park is the "
              "MAJORITY of a fuseball's life, not a pause in it");
static_assert(ArcadeFuseball::ShotStepHi - ArcadeFuseball::ShotStepLo + 1 == 9,
              "the shot window is NINE of sixteen steps (4..12), not the later "
              "port's eight (4..11)");
static_assert(ArcadeFuseball::GrabDz == 0.3125f,
              "enemy-catches-player depth window: 2 world-z, DOCTRINE.md adjudicated "
              "conflict 2. If this fails, ArcadeFlipper::ParkedGrabDz moved -- and "
              "it is the same adjudicated number for both families.");
static_assert(fuseball_detail::nearly(ArcadeFuseball::SpinDegPerTick, 3.1618f, 1e-4f),
              "body spin: 2/256 of a revolution per reference tick");
static_assert(fuseball_detail::nearly(ArcadeFuseball::LegStepDeg, 71.71875f, 1e-4f),
              "leg step: 51/256 of a revolution, the integer floor of 72 degrees");
static_assert(ArcadeFuseball::EngineTickMs * ArcadeFuseball::ENGINE_HZ == 1000.0f,
              "the restated engine tick must agree with the rate everything else in "
              "this family converts against (game_step.cpp TICK_MS = 16 -> 62.5 Hz)");
static_assert(ENEMY_LIFE[ARCADE_FUSEBALL] == 1,
              "life 1 is load-bearing, not a placeholder: the shared shot/enemy "
              "exchange is a mutual hit-point subtraction, so one hit kills and the "
              "laser pierces for free (arcade_enemies.md sec 3.5)");
static_assert(ENEMY_DZ[ARCADE_FUSEBALL] == 0.0f,
              "this family steps its own level-scaled z; a non-zero descriptor row "
              "would be a SECOND step per tick (audit sec 5/M1) and would also hand "
              "the id to move_embrios, which hangs the level (audit sec 8)");

} // namespace enemyfam
} // namespace ts

// ============================================================================
// REGISTRATION -- APPLIED. Every item below has LANDED; the list is kept as
// the record of what the shared-file pass had to do and why, not as work
// outstanding. Do not re-apply any of it.
//
// REGISTRATION -- what the SHARED-FILE pass must apply. Listed here so the
// requirement travels with the family instead of only in a report. Every one
// of these is an edit this wave is forbidden to make, and the family is
// INERT (declared, compiled, unreachable) until they land.
//
//  1. game/enemies.cpp move_enemies -- the dispatch:
//         case ARCADE_FUSEBALL:
//             deleted = enemyfam::ArcadeFuseball::update(engine, ctx, v, v2, enemy);
//             break;
//     WITHOUT THIS the fuseball falls to `default:` and never moves.
//
//  2. game/enemies/enemies_shared.h `enemy_shootable()`:
//         case ARCADE_FUSEBALL:
//             return enemyfam::ArcadeFuseball::shootable(e);
//     WITHOUT THIS a parked fuseball is shootable, which deletes the single
//     most important fact about the enemy.
//
//  3. game/enemies/enemies_shared.h `arcade_kill_score()`:
//         case ARCADE_FUSEBALL: return enemyfam::ArcadeFuseball::rollScore();
//     NB this is the first case in that switch that is NOT pure -- it consumes
//     one draw from the shared rand() stream, once per corpse, because the
//     recovered score is a uniform 250/500/750 roll rather than a constant.
//     Without it a fuseball kill scores 1 point (audit sec 6/D4).
//
//  4. game/enemies/enemies_shared.h `arcade_release()`:
//         case ARCADE_FUSEBALL:
//             return enemyfam::ArcadeFuseball::spawn(engine, lane);
//     The caller's uniform-random lane IS the recovered rule; pass it through.
//
//  5. game/enemies/arcade_tanker.{h,cpp} -- the fuse-tanker's payload:
//         * flip `ArcadeTanker::FuseballReady` to true (its static_assert in
//           arcade_tanker.cpp is what currently holds the ARCADE_FUSE_TANKER
//           spawn bit clear, and it becomes a BUILD ERROR the moment a level
//           asks for that variant without this);
//         * `openChildren()` must branch on the parent id and, for
//           ARCADE_FUSE_TANKER, call
//               ArcadeFuseball::spawnHatchling(engine, ctx, lane, z, +1);
//               ArcadeFuseball::spawnHatchling(engine, ctx, lane, z, -1);
//           in that order (+1 first), matching the reference's own order --
//           which matters only because it fixes the order of any rand() draws
//           the two children make.
//     The fuse-tanker differs from the plain tanker by EXACTLY ONE FIELD
//     (which contents routine it opens into); everything else -- the far-plane
//     spawn, the end-lane restriction, the 3-slot reservation, the straight
//     descent, "shot and landed are identical" -- is the shared carrier
//     arcade_tanker.{h,cpp} already implements.
//
//  6. !! THE SUPERZAPPER SEAM SPLIT (recovery sec 1.3). `enemy_shootable()` is
//     currently ONE predicate for all four damage sites. The fuseball is the
//     first enemy for which the bullet answer and the superzapper answer
//     DIFFER: the reference runs an unconditional collision call on any
//     superzap frame before dispatching to a mode routine, so the zapper kills
//     a parked fuseball. Split the two call sites in weapons.cpp (the tremor
//     kill zone and the zapper target search) onto a sibling predicate that
//     asks `ArcadeFuseball::zappable()` for this id and `enemy_shootable()`
//     for every other. THIS IS NOT COSMETIC: a fuseball is unshootable for
//     ~75% of its life and NEVER LANDS AND NEVER EXPIRES, so with the zapper
//     also blocked the player's only clear-the-screen fallback is gone and
//     `is_level_clear()` can be held open for a long time by one parked ball.
//
//  7. rendering/entity_geometry.cpp -- geometry, or the fuseball is FULLY
//     SIMULATED, FULLY LETHAL AND COMPLETELY INVISIBLE (audit sec 10/G1; this
//     is exactly how SP_ZAPPER1 shipped invisible for months). Two of the four
//     things needed are already free:
//         * `buildEnemies` ALREADY branches per enemy on `isArcadeEnemy()`, so
//           this id never touches the generic animation block that would read
//           `oo_max_animation` / `max_animation` as counters. Nothing to do.
//         * the ANCHOR needs NO new code either: `enemyAnchorPos()` already
//           resolves ANCHOR_RAIL as `left + (right - left) * cross_t`, which
//           IS this family's whole position model.
//     What must be added:
//         * a `case ARCADE_FUSEBALL` in `getArcadeEnemyGeometry()` -- WITHOUT
//           it the accessor returns a null ArcadeGeometry and the body draws
//           nothing;
//         * a `case ARCADE_FUSEBALL` in `enemyModelMatrixArcade()` that adds
//           `ArcadeFuseball::spin(enemy)` to `rBase`, and applies
//           ARCADE_ARRIVAL_FILL_SCALE while `mode(enemy) == MODE_ARRIVAL`.
//     AND ONE STRUCTURAL EXTENSION, flagged rather than resolved here:
//     `ArcadeGeometry` currently describes ONE vert/face/colour set plus an
//     optional X-mirror, i.e. at most 2 draws and 2 outline rings. THE ROSETTE
//     IS FIVE draws of one blade at `LegStepDeg` apart, each in its own fixed
//     slot colour, each mirrored or not per
//     `ArcadeFuseball::legMirrored(engine.time, leg)`. So that struct needs a
//     "repeat N at a fixed angular step, one colour per slot" notion, and the
//     result is 5 draws per body against arcade_enemies.md sec 6.2's budget of
//     2 -- a real per-frame cost decision for the rendering pass on the 3DS,
//     not something this family can make.
//
//  8. game/constants.h -- `ARCADE_FUSEBALL_MODEL_FULL_WIDTH` and
//     `ARCADE_FUSEBALL_LANE_FILL`, beside the flipper/tanker/spiker pairs.
//     They live there rather than here because the RENDERER reads them and the
//     family does not, exactly like CLAW_MODEL_FULL_WIDTH. NB the fuseball is
//     the one arcade body that is NOT lane-wide: it is a ball on a RAIL, not a
//     body filling a lane, so its fill should be well under 1.0.
//
//  9. data/enemy_data_arcade.h -- the blade shape and the five slot colours.
//     THE SHAPE, described proportionally so it can be re-authored (no table
//     is copied): a 4-vertex sliver drawn as two triangles sharing their
//     mid-span edge -- a straight baseline of length L between the two end
//     vertices (inner end at the rosette hub, outer end at the tip), and two
//     interior vertices at the 50% point of that baseline offset
//     perpendicular by 0.375 L and 0.125 L to the SAME side. That is a curved
//     blade, thickest at mid-span and pinched to zero at both ends; the TIP
//     vertex is full brightness and the rest are mid, which is what gives each
//     arc a hot outer point. The second form is the EXACT MIRROR of the first
//     about the baseline -- that mirror pair is the entire animation.
//     THE COLOURS, by angular slot from the base angle upward: RED, GREEN,
//     PURPLE, YELLOW, CYAN. Fixed to slots, never cycled.
//
// 10. game/enemy_spawns.h -- the arcade spawn table. Bit `i` == EnemyId `i`,
//     so the fuseball is `1 << ARCADE_FUSEBALL`. RECOVERED CADENCE, for
//     whoever authors those rows: there is NO probability roll anywhere in
//     fuseball spawning -- it is a pure per-level PERIOD, and lower means more
//     often. First fuseball at level 11; the period tightens 1000 -> 500
//     across levels 11-16, relaxes to 900 for 17-24, and settles to a flat 300
//     (the densest setting shipped) from level 70 on. Levels with NO fuseballs
//     at all: 1-10, 49, 51, 57, 58, 64-66, 69, 71, 72, 99, 100. First
//     fuse-tanker at level 25. This port's population model is its own
//     (arcade_enemies.md sec 3.4 keeps enemies_todo[]), so those numbers are a
//     shape to fit, not a table to transplant.
//
// 11. CMakeLists.txt SOURCES -- add `src/game/enemies/arcade_fuseball.cpp`
//     beside the other families. Makefile.3ds picks it up by wildcard already
//     (its globs are three directory levels deep), so the 3DS side needs no
//     edit -- but a family that lands on one target only is INCOMPLETE, not
//     "phase one" (DOCTRINE.md, TARGET PARITY IS POLICY).
// ============================================================================
