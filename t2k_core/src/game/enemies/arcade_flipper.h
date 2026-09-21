#pragma once
// ============================================================================
// arcade_flipper.h -- THE ARCADE FLIPPER. Ids ARCADE_FLIPPER / ARCADE_SFLIPPER2 /
// ARCADE_SFLIPPER3 / ARCADE_BEAST (one family, four sub-variants -- the Beast
// is a flipper, see SUB_BEAST).
//
// Spec: docs/design/arcade_enemies.md sec 2.1 for the flipper proper and
// docs/design/arcade_mirror_beast_recovery.md sec 5 for the Beast, both read
// against the header's
// adjudication table (the arcade reference is the baseline; the later port is the
// cross-check). Shared-path map: docs/design/enemy_pipeline_audit.md.
// Shape contract: enemy_family.h. Worked example: reflector.{h,cpp}.
//
// The signature enemy: owns one lane, walks down it, then HINGES from lane to
// lane along the rim hunting the claw. Everything below is in THIS engine's
// units (engine z, degrees, 62.5 Hz ticks); every constant carries its the later port
// value and the sec of the spec it came from, so nobody has to re-derive the
// conversion table (arcade_enemies.md sec 1) to read the code.
//
// ---- THE TWO VERIFIER CORRECTIONS, because both are easy to get backwards --
//
//  (a) MID-FLIP VULNERABILITY IS NOT MONOTONIC. The original reduces the
//      remaining rotation to a SHORTEST SIGNED ANGLE before testing it, so the
//      flipper is shootable when the remaining rotation is SMALL *or* VERY
//      LARGE and invulnerable in between:
//          VULNERABLE iff remaining <= 73.125 deg OR remaining >= 286.875 deg.
//      A flip longer than 286.875 deg therefore OPENS with a brief shootable
//      window before locking out. That happens exactly at REFLEX vertices,
//      where the web's interior angle exceeds 286.875 deg -- i.e. the lane
//      direction turns 107..180 deg. This port's 100 webs have plenty.
//      Do not "simplify" shootable() into a single `remaining <= 73.125`.
//
//  (b) DURING ITS INVULNERABLE PHASE THE FLIPPER GRABS THE PLAYER IN THE LANE
//      IT IS FLIPPING *OUT OF*, not the one it is entering. The lane index
//      steps to the destination at flip-setup; the SOURCE lane is stashed in
//      the grab field first, and that is what the mid-flip grab test reads.
//      It threatens the destination lane only once the flip resolves and the
//      parked test runs on the following tick. So: a flipper closing on you
//      cannot take you mid-flip -- it takes you on arrival, or it takes you if
//      you step into the lane it is vacating. That IS the feel of the enemy.
//      threatLane() is the single place this is expressed.
//
// ---- WHERE THE PER-ENEMY STATE LIVES, AND WHY IT IS NOT A UNION -----------
//
// arcade_enemies.md sec 3.3 specifies a named `ArcadeState` union on `Enemy`. That
// is a models.h change, and three arcade families are being authored in
// PARALLEL -- three waves each adding a member to the same union is a
// guaranteed collision. So this family maps its state onto Enemy's existing
// generic scratch fields through the NAMED ACCESSORS below. They are the only
// place the mapping is spelled out; when `ArcadeState` lands, the accessor bodies
// change and not one line of arcade_flipper.cpp moves.
//
//     Enemy::current_lane      -> mode           (Mode)
//     Enemy::sidestep_freq     -> sub-variant    (Sub)
//     Enemy::shoot_freq        -> pause countdown (may be -1)
//     Enemy::animation_vec     -> forced-spin counter (SIGNED: sign = fixed
//                                 flip direction, magnitude = flips left)
//     Enemy::animation_phase   -> grab marker: SOURCE lane, or MARKER_HATCHLING
//                                 / MARKER_PULSAR
//     Enemy::max_animation     -> last-updated tick stamp (see the transfer
//                                 note below)
//     Enemy::px                -> flip TOTAL rotation, signed degrees
//     Enemy::py                -> flip SIGNED RATE, degrees/tick, CAPTURED AT
//                                 FLIP-SETUP (not re-read per tick -- that is
//                                 what makes a tanker-born super's FIRST flip
//                                 run at the plain rate, which is observable)
//     Enemy::anchor_rot        -> flip PROGRESS rotation, signed degrees
//                                 (this one IS models.h's own contract:
//                                  "degrees already rotated about the pivot")
//     Enemy::anchor            -> ANCHOR_LANE_MID at rest / ANCHOR_PIVOT
//                                 mid-flip (models.h)
//     Enemy::pivot_side        -> which border of the CURRENT (destination)
//                                 lane is the hinge (models.h)
//     Enemy::cross_t           -> the BEAST's horn counter (2, 1, 0); zero
//                                 on every other sub-variant
//     Enemy::oo_max_animation  -> UNUSED. Never assigned by this family, so
//                                 it keeps models.h's default of 0.01f --
//                                 NOT 0 -- exactly the value the tanker sets
//                                 by hand (arcade_tanker.cpp). Harmless only
//                                 because the arcade draw path
//                                 (enemyModelMatrixArcade) never reads it;
//                                 see the note just below.
//
// !! CONSEQUENCE FOR THE RENDERING WAVE: `animation_phase`, `animation_vec`,
//    `max_animation` and `oo_max_animation` are NOT animation counters on a
//    flipper. entity_geometry.cpp's generic per-enemy animation block (the
//    `animation_phase * oo_max_animation` wobble and the
//    `animation_phase < max_animation` test) must never run for an arcade id.
//    It cannot: buildEnemies branches on isArcadeEnemy() BEFORE that block,
//    routes arcade ids through getArcadeEnemyGeometry / enemyModelMatrixArcade
//    and `continue`s, so getEnemyGeometry is never reached for one at all.
//    That dedicated builder is what audit G4/G5 required and it has landed --
//    lane-relative size from ArcadeGeometry::laneFill, and TWO draws for the
//    mirrored half-polygon rather than the classic 8x loop. entity_geometry.cpp
//    carries the matching note and cites this block back.
//
// ---- WHY THIS FAMILY DOES ITS OWN LANE TRANSFER ---------------------------
//
// enemies.cpp's `_transfer_enemy` is `static` (not reachable) and it NEGATES
// `animation_phase` on every transfer (audit R3) -- which here is the grab
// marker. So the flip's lane change is done inline in arcade_flipper.cpp with the
// same swap-down semantics and the same `enemies_nums` invariant (a transfer
// is not a removal, so the counter is untouched).
//
// The tick stamp exists because `move_enemies` walks lanes 0..n-1: an enemy
// transferred to a HIGHER lane index is visited AGAIN in the same frame and
// would take a second flip step. The stamp makes each flipper act exactly once
// per tick. Delete it the day `move_enemies` grows a transfer guard of its own.
// ============================================================================

#include "enemy_family.h"
#include "../sfx.h"

namespace ts {
namespace enemyfam {

struct ArcadeFlipper {
    // The ids this family claims. The `switch` in enemies.cpp is the single
    // dispatch and must agree with this list (enemy_family.h step 3).
    //
    // ARCADE_BEAST is in this list because THE BEAST IS A FLIPPER, not a
    // fourth enemy -- see the block above SUB_BEAST below.
    static constexpr int Ids[] = { ARCADE_FLIPPER, ARCADE_SFLIPPER2,
                                   ARCADE_SFLIPPER3, ARCADE_BEAST };

    // ---------------------------------------------------------------------
    // STATES. `mode` in the original; a small dense enum here.
    // ---------------------------------------------------------------------
    enum Mode {
        MODE_ARRIVAL = 0,   // S-1: the inert 2x2 dot falling in from behind
        MODE_RAIL    = 1,   // S0 : walking one lane down to the rim
        MODE_FLIP    = 2,   // S1 : hinging about a shared border vertex
        MODE_STOPPED = 3,   // S2 : parked at the rim, counting down to a flip
    };

    // SUB-VARIANTS. Deliberately NOT the same axis as Enemy::id: the id says
    // which of the three roster entries this is (and therefore its colour
    // count and its spawn table bit); `sub` says which BEHAVIOUR is live.
    // ARCADE_SFLIPPER3 runs as SUB_SUPER3_DIVE and then, at the rim, promotes
    // itself to SUB_SUPER3_SEEK -- without changing its id, because nothing in
    // the shared pipeline supports an enemy changing its id while alive
    // (audit R4: `enemies_nums[old]` would never decrement).
    //
    // ---- AND SLOT 5 IS THE BEAST ("the demon head") ---------------------
    //
    // CONFIRMED by the recovery (docs/design/arcade_mirror_beast_recovery.md
    // sec 5.1), and more strongly than "it is a bit like a flipper": the
    // reference's Beast maker CALLS THE SHARED FLIPPER MAKER and then
    // overwrites five fields, its updater IS the flipper updater, and it
    // reaches its own behaviour through ONE TABLE SLOT -- rail sub-variant 5.
    // It uses the flipper mode machine, the flipper stop machine, the flipper
    // turn code, the flipper grab, the flipper death and the flipper score.
    //
    // The COMPLETE delta against a plain flipper, all fourteen rows of the
    // recovery's table, and where each one lives in this port:
    //
    //   1  rail sub-variant   0 -> 5                    SUB_BEAST, dispatched
    //                                                   in update()'s RAIL arm
    //   2  horn counter       unused -> 2               BeastHorns / horns()
    //   3  flip pause         8 -> none                 pauseReloadFor() ALREADY
    //                                                   returns PauseNever for
    //                                                   every non-plain sub --
    //                                                   the Beast INHERITS this
    //                                                   rather than introducing
    //                                                   it (both supers set it
    //                                                   too)
    //   4  drawn shape        flipper -> demon head     RENDERING WAVE, see the
    //                                                   report
    //   5  forced spin        -> cleared to 0           initCommon writes 0
    //   6  hit points         1 -> 3                    horns 2 + the death hit
    //   7  a non-fatal hit    dies -> SHEDS A HORN      beastRail(), and the
    //                                                   horn IS the projectile
    //   8  rotation rate      unchanged                 rateFor() gates the x2
    //                                                   on sub == EXACTLY 2
    //   9  turn direction     unchanged                 chooseDir() gates the
    //                                                   inversion on EXACTLY 3
    //  10  stopped behaviour  unchanged (stdstop)       spin() is 0, so the
    //                                                   forced-spin branch is
    //                                                   never taken
    //  11  initial mode       unchanged (rail)          MODE_ARRIVAL -> RAIL
    //  12  fires alien shots  unchanged                 the shared rail's own
    //                                                   arcade_alien_fire_tick
    //  13  superzapper        DIES INSTANTLY, horns      shootable() is true, so
    //                        irrelevant                 the beam locks on and
    //                                                   kills it -- and this is
    //                                                   the sharpest contrast
    //                                                   with the MIRROR, which
    //                                                   is immune
    //  14  colour             NOT level-banded          RENDERING WAVE
    //
    // Rows 8 and 9 are the two easiest to get wrong and the recovery says so
    // in as many words (its sec 10 item 10): both the doubled rotation and the
    // inverted turn are gated on an EXACT sub-variant match in the reference,
    // and the Beast is neither of those values. This port already expresses
    // both as exact compares in rateFor() and chooseDir(), so adding slot 5
    // gets both right by construction -- do not "generalise" either gate to
    // ">= SUB_SUPER2" or the Beast starts spinning twice as fast.
    enum Sub {
        SUB_PLAIN       = 0,   // ARCADE_FLIPPER
        SUB_SUPER2      = 2,   // ARCADE_SFLIPPER2 -- DOUBLE flip rate, no pause
        SUB_SUPER3_DIVE = 3,   // ARCADE_SFLIPPER3 while descending: flees, and
                               // descends INSIDE the flip handler
        SUB_SUPER3_SEEK = 4,   // ARCADE_SFLIPPER3 after the rim: hunts normally
        SUB_BEAST       = 5,   // ARCADE_BEAST -- a plain flipper with a bullet
                               // test in front of its rail; three hits, two
                               // sheds, and each shed hands your bullet back
    };

    // GRAB-FIELD SENTINELS (arcade_enemies.md "Tanker-hatched flipper").
    // A non-negative value is the SOURCE lane -- see correction (b).
    static constexpr int MARKER_HATCHLING = -1;  // cannot grab; drop into RAIL
                                                 // when this first flip lands
    static constexpr int MARKER_PULSAR    = -2;  // ...and metamorphose into a
                                                 // pulsar instead (Wave 2)

    // =====================================================================
    // CONSTANTS. Every one carries its the later port value, its unit and its source.
    // The later port->engine conversions are arcade_enemies.md sec 1:
    //     ARCADE_HZ 70.2617, TS_HZ 62.5  -> ARCADE_RATE  1.12419 (per-tick deltas)
    //                                 -> ARCADE_TICKS 0.88953 (tick counts)
    //     tube 5120 raw = 160 world-z = GRID_ELEMENT_LENGTH 25
    //                                 -> ARCADE_Z     0.15625 engine z / world-z
    //                                 -> ARCADE_ZRATE 0.175655 engine z/tick per
    //                                              world-z/the later port tick
    // WALL-CLOCK is preserved, not tick counts (sec 1.1).
    // =====================================================================

    // ---- geometry of the tube -------------------------------------------
    // Far plane. the later port 6464 raw. sec 1.2 / sec 2.3 geometry table.
    static constexpr float FarPlaneZ = GRID_ELEMENT_LENGTH;          // 25.0
    // Rim plane. the later port 1344 raw.
    static constexpr float RimPlaneZ = 0.0f;

    // ---- S-1 ARRIVAL -----------------------------------------------------
    // Spawn depth: the far plane pushed a further 300 world-z back.
    // 25 + 300*0.15625 = 71.875 engine z. sec 1.4 / sec 2.1 S-1.
    static constexpr float SpawnZ = 71.875f;
    // Arrival closing rate. the later port 2 world-z/tick -> 2 * 0.175655. sec 1.4.
    static constexpr float ArrivalDz = 0.35131f;                     // z/tick
    // Nominal arrival duration, for the harness: 46.875 / 0.35131 = 133.4
    // ticks = 2.135 s (the later port: 150 ticks). sec 1.4.
    static constexpr float ArrivalTicks = 133.42f;

    // ---- S0 RAIL: the ONLY level-scaled quantity -------------------------
    // A 56-entry table indexed by `wave >> 1`. sec 2.1 S0 recovered only its
    // ENDPOINTS -- 0.625 world-z/tick at wave 0-1 rising to 0.867 at wave
    // 110+ -- so the interior of the table is RE-AUTHORED here as a monotone
    // linear ramp that is EXACT at both ends. Flagged for the verifier: it is
    // the one place in this file that carries numbers the recovery did not.
    static constexpr int   RailTableSize = 56;
    static constexpr float RailDzWave0   = 0.10979f;   // z/tick, wave 0-1
    static constexpr float RailDzWaveMax = 0.15231f;   // z/tick, wave 110+
    // CORRECTED (sec 2.1 S0): the flipper's ROTATION RATE and PAUSE do NOT
    // scale with the level. In the arcade reference mode the per-wave setup forces the table
    // index to 16 before indexing the rotation-speed / cross-delay / pause
    // tables, so rate and pause are constant for the whole game. Do not build
    // per-wave tables for them.

    // ---- S1 FLIP ---------------------------------------------------------
    // 16/1024 of a turn per the later port tick = 5.625 deg, * ARCADE_RATE. sec 1.4.
    static constexpr float FlipDegPerTick = 6.3236f;
    // Doubled for sub-variant EXACTLY 2 (sec 2.1 S1: super-flipper-3 does NOT
    // get it).
    static constexpr float SuperTwoRateMul = 2.0f;
    // A flip whose remaining rotation is at or below one step SNAPS and
    // completes. RE-AUTHOR, MANDATORY (sec 2.1 S1 "Termination"): the original
    // tests `err == 0` with no tolerance, which only ever terminates because
    // every the later port web orientation is a multiple of 1/64 turn. This port's webs
    // have arbitrary turning angles (Typhoon-derived rows are not multiples of
    // 5.625 deg) and an exact-equality test WOULD HANG.

    // ---- S1 vulnerability (CORRECTED, non-monotonic -- see (a) above) ----
    // 52/256 of a turn. The original divides the remaining rotation by 4 and
    // truncates to a SIGNED BYTE, which is exactly "reduce to the shortest
    // signed angle", then treats |x| > 52 as invulnerable. sec 2.1
    // "S1 VULNERABILITY".
    static constexpr float VulnDeg    = 73.125f;          // 52/256 turn
    static constexpr float VulnHiDeg  = 360.0f - VulnDeg; // 286.875

    // ---- S2 STOPPED ------------------------------------------------------
    // CORRECTED (sec 2.1 S2): the pause constant is 8, not 9. The counter is
    // decremented on entry and the flip fires on the tick the decrement
    // underflows -- 8 full waiting ticks plus the flipping tick = 9 wall
    // ticks. 8 engine ticks = 0.128 s.
    static constexpr int PauseTicks = 8;
    // Super-2 and level-spawned super-3 carry a pause RELOAD of -1: the reload
    // byte is the high half of the same word, so -1 reloads as -1 and the
    // countdown underflows every tick. They never pause. sec 2.1 FORCED SPIN.
    static constexpr int PauseNever = -1;
    // Parked grab window -- THE THE ARCADE REFERENCE'S, 2 world-z.
    //
    // This shipped as the later port value (8 world-z -> 1.25) and that was a straight
    // MISS, not a judgement call: arcade_enemies.md's own adjudication table
    // records "Enemy-catches-player depth window | the arcade reference: 2 units | the later port's ~8
    // comes from scaling that one test by the X/Y factor" as a settled USER
    // DECISION, while its implementation table further down still carried the
    // the later port number -- and the implementation followed the wrong row of the same
    // document. Enemies were grabbing from 4x further down the tube than
    // adjudicated.
    //
    // 2 world-z / 6.4 == 0.3125, the same world->engine conversion that turned
    // the later port 8 into 1.25, so only the SOURCE constant changed here.
    //
    // Why the arcade reference wins (recorded so it is not re-litigated): the later port scales this
    // ONE test by the X/Y factor where every other depth constant in its own
    // file -- the bullet test included -- uses the Z factor. That is an
    // inconsistency inside the later port, not a deliberate retune.
    static constexpr float ParkedGrabDz = 0.3125f;

    // ---- SUB-VARIANT 5: THE BEAST ---------------------------------------
    // docs/design/arcade_mirror_beast_recovery.md sec 5. Four constants, and
    // that plus one branch in the RAIL arm is the WHOLE of the Beast -- which
    // is the recovery's own point (its sec 10 item 7: "everything else --
    // descent, rim clamp, hinge rotation, grab, death, score -- must be the
    // code the plain flipper already runs").

    // THE HORN COUNTER STARTS AT 2 AND THE DEATH TEST IS `== 0` BEFORE THE
    // DECREMENT, so the sequence is: hit with 2 -> shed, hit with 1 -> shed,
    // hit with 0 -> DIE. THREE HITS, TWO SHEDS. Not three sheds, not two hit
    // points -- the recovery calls this out as an easy mis-read (sec 10 item
    // 8), and the drawn part count follows it exactly: 3 parts, then 2, then
    // 1 (head plus horns), so the player SEES which hit they are on.
    static constexpr int BeastHorns = 2;

    // The bullet-notice window: same lane, |dz| < 6 world-z. It is the SAME
    // recovered constant the Mirror uses (one reference helper, called by
    // both) and the same one the Beast's shed horn uses to absorb an ordinary
    // player bullet -- ArcadeReflectedShot::AbsorbDz. Spelled here rather than
    // included so that arcade_flipper.h does not pull arcade_mirror.h into
    // enemies_shared.h; the static_assert below is what keeps the two copies
    // honest, and arcade_mirror.h carries the matching one.
    static constexpr float BeastHitDz = GRID_ELEMENT_LENGTH * 6.0f / 160.0f;  // 0.9375

    // WHICH HORN IS SHED, and it is a genuine cosmetic quirk of the
    // authoritative source rather than a mistake in this port: at 2 horns the
    // PROJECTILE is drawn as the up-LEFT horn while what visually vanishes
    // from the creature is the up-RIGHT one, and at 1 horn the pair swap. The
    // later port shed them the other way round -- arguably the more visually
    // consistent order -- and doctrine takes the arcade reference (recovery
    // sec 5.5, sec 8 row 6). Carried as a bit on the reflected shot so the
    // rendering wave can honour it; if the port's design language later
    // prefers "shed the horn that vanished", that is an INTENTIONAL DEVIATION
    // to record in DOCTRINE.md, not a silent swap here.
    static constexpr int BeastHornFirst  = 0;   // shed when it still had 2
    static constexpr int BeastHornSecond = 1;   // shed when it had 1

    // NO SOUND ON A SHED. The Mirror plays a sample on every non-fatal hit and
    // the Beast plays NONE (recovery sec 5.3, stated as an explicit contrast).
    // Recorded as a constant-free note so nobody "fixes" the silence: the
    // visible loss of a horn is the feedback.

    // ---- forced spin (tanker hatchlings only) ---------------------------
    // A random 1..4 written ONLY by the super-promotion routine, which is
    // called ONLY from the flip-tanker hatch. Its SIGN is the fixed flip
    // direction the hatchling keeps, which is what fans the pair 2-5 lanes
    // apart. Level-spawned supers explicitly zero it. sec 2.1 FORCED SPIN.
    static constexpr int ForcedSpinMin = 1;
    static constexpr int ForcedSpinMax = 4;

    // ---- promotion probabilities (random source returns 0..255) ----------
    // sec 2.1 "Variants and promotion" (CORRECTED: the supers are NOT
    // unconditional). The plain spawner can only ever produce super-2;
    // super-3 is reachable only from the generator's own id-11 entry or from
    // the tanker's second roll.
    //   level-spawned  -> super-2 : 2 * max(wave-16, 0) / 256   (0 below w16)
    //   tanker hatchling-> super-2: 4 * min(wave, 63)  / 256
    //   ...then super-2 -> super-3: 2 * min(wave, 127) / 256
    static int promoteLevelSuper2Num(int wave);   // numerator over 256
    static int promoteHatchSuper2Num(int wave);
    static int promoteSuper3Num(int wave);

    // ---- S4 DEATH --------------------------------------------------------
    // 150 points (Beastly doubling is out of scope with Beastly mode).
    //
    // Passed as init_explosion's `energy`, which is what the arcade roster
    // scores through: the shared `_handle_death` derives score from `energy`
    // and would otherwise pay the arcade life of 1 (audit sec 6/D4,
    // arcade_enemies.md sec 3.6). Reached via enemies_shared.h
    // `arcade_kill_score()`, which maps ARCADE_FLIPPER / SFLIPPER2 /
    // SFLIPPER3 / BEAST to this constant. NB the shot-hit sites (collision.cpp,
    // enemies.cpp) deliberately do NOT score an arcade kill -- it is paid
    // ONCE, here.
    static constexpr int Score = 150;

    // ---- sec 2.5 alien fire (SHARED per-set timer, NOT this family's) ----
    // One global 16-bit register, low byte countdown / high byte reload,
    // 0x7070 = a 112-the later port-tick period, never changed for the whole game.
    // 112 * 0.88953 = 99.6 -> 100 engine ticks.
    static constexpr int AlienFirePeriod = 100;
    // Depth gate: the shooter's z must be strictly DEEPER than 3264 raw =
    // the deepest 62.5% of the tube. sec 1.4 / sec 2.5.
    static constexpr float AlienFireDepthGate = 9.375f;
    // Concurrent budget `min(wave >> 3, 3)`, tested as >= 0, so the real cap
    // is budget + 1: 1 bullet on waves 1-8 rising to 4. sec 2.5.
    static constexpr int AlienFireCapBase = 1;
    static constexpr int AlienFireCapMax  = 4;

    // ---- presentation ----------------------------------------------------
    // "flip sound plays unless the level is ending" (sec 2.1 S1). There is no
    // flipper sample in this port's bank, so the crawl is re-used pitched
    // down. PRESENTATION, therefore free (DOCTRINE.md) -- but flagged for
    // review, because CRAWL is also the claw's own lane-move sound.
    static constexpr SfxId FlipSfx      = SfxId::CRAWL;
    static constexpr float FlipSfxPitch = 0.55f;
    // UNCERTAIN, sec 2.1: super-flipper-3's 4-tick down-counter has no proven
    // consumer; the proposed resolution is that it is the five-tone palette
    // index. 4 the later port ticks = 3.56 engine ticks. Exposed for the rendering wave;
    // this family does not step a colour.
    static constexpr float Super3ColorPeriodTicks = 3.56f;

    // ---- degenerate-web guard -------------------------------------------
    // A lane pair that doubles straight back on itself (Tsunami web 31 lanes
    // 0/1 are (0,+1) then (0,-1)) has an interior wedge of 0 or 360 degrees.
    // 0 would complete the flip with no rotation at all; 360 -- a full tumble
    // in place -- is the readable answer and is what the reflex branch gives
    // anyway, so a sub-epsilon wedge is promoted to a full turn.
    static constexpr float MinFlipDeg = 1e-3f;

    // =====================================================================
    // ENTRY POINTS
    // =====================================================================

    // The per-tick update. Returns true iff this call REMOVED the enemy from
    // `lane` -- and a lane transfer COUNTS as removal (enemy_family.h): the
    // slot now holds a different enemy, so the caller must neither touch
    // `enemy` nor advance `idx`. A flip-setup transfers, so this returns true
    // on flip-setup ticks.
    static bool update(GameEngine& engine, const EnemyCtx& ctx,
                       int lane, int idx, Enemy& enemy);

    // The level generator's flipper. `id` is ARCADE_FLIPPER (which rolls the
    // super-2 promotion), ARCADE_SFLIPPER2 or ARCADE_SFLIPPER3. Spawned at a
    // UNIFORM-RANDOM lane -- the caller picks it; contrast the tanker, which
    // excludes both end lanes (sec 2.2). Returns false if the lane was full.
    static bool spawn(GameEngine& engine, int lane, int id);

    // The flip-tanker's pair (sec 2.1 "Tanker-hatched flipper"). Emitted at
    // the tanker's own position, depth and lane; one given an immediate LEFT
    // flip (dir -1) and one RIGHT (dir +1); each rolled for super promotion;
    // each marked MARKER_HATCHLING (or MARKER_PULSAR by a pulsar-tanker).
    //
    // ORDERING IS OBSERVABLE (sec 2.1): the initial flip setup runs BEFORE the
    // promotion writes the sub-variant, and the rate doubling is read from
    // that field at setup time -- so THE FIRST FLIP OF A TANKER-BORN SUPER
    // RUNS AT THE PLAIN RATE. A port that reorders the two calls ships a
    // visibly faster first flip. That order is preserved inside this function.
    static bool spawnHatchling(GameEngine& engine, const EnemyCtx& ctx,
                               int lane, float z, int dir, int marker);

    // THE VULNERABILITY PREDICATE (arcade_enemies.md sec 3.5). This is the
    // `ArcadeFlipper::shootable(e)` the shared `enemy_shootable()` calls from BOTH shot
    // sweeps and from the tremor (audit C9 / Z4). Correction (a) lives here.
    static bool shootable(const Enemy& enemy);

    // THE SUPERZAPPER / TREMOR PREDICATE for this family. The original's
    // run_flipper tests the superzap flag (_sz) BEFORE its mode dispatch and
    // zappits the flipper in ANY mode -- rail, mid-flip, or parked (yak.s:11017;
    // collie/xzcollie likewise route to zappit instead of the bullet check when
    // _sz is set, yak.s:12925/12945). So the non-monotonic shootable() lockout
    // is a BULLET rule only and must NOT gate the area weapons. The one exception
    // is the inert arrival dot, which stays inert to everything.
    static bool zappable(const Enemy& enemy);

    // THE LANE THIS FLIPPER CURRENTLY THREATENS, or -1 for none.
    // Correction (b) lives here: mid-flip it is the SOURCE lane, parked it is
    // the current lane, and a hatchling still carrying its -1/-2 marker
    // threatens nothing at all.
    static int threatLane(const Enemy& enemy, int lane);

    // sec 2.5's shared alien-fire tick, implemented here because the flipper's
    // rail is one of only THREE call sites (the other two are the spiker's
    // climb and descend). The timer is per-SET state and lives on GameEngine
    // as `arcade_fire_timer` (engine.h, cleared in init_level). It is still
    // taken BY REFERENCE so this family names no engine field; all three sites
    // -- including this family's own RAIL arm in update() -- reach it through
    // enemies_shared.h's `arcade_alien_fire_tick` wrapper, which supplies it.
    static void alienFireTick(GameEngine& engine, int& fireTimer,
                              int lane, float z);

    // Level-scaled rail descent, |z| per tick. `wave` is engine.current_level.
    static float railDescent(int wave);

    // Winding sign of the web: +1 when the ring is wound counter-clockwise
    // (so grid_level_normal, the LEFT normal, points INTO the tube), -1 when
    // clockwise, +1 for open and for self-crossing webs. This is
    // entity_geometry.cpp `clawNormalSign` re-derived -- the flipper and the
    // claw MUST agree about which side is "inside", and it is duplicated only
    // because that copy is `static` in a rendering TU. Hoist both to
    // web_geometry.h in a follow-up.
    static float webWindingSign(const GameEngine& engine);

    // The signed rotation of one flip, in degrees: the web's INTERIOR wedge at
    // the hinge vertex, signed by which way the body sweeps. Exposed so the
    // harness can prove the three quoted magnitudes without driving a tick.
    static float flipRotation(const GameEngine& engine, int srcLane,
                              int dstLane, int dir);

    // Direction the flipper would choose right now: -1 lower index, +1 higher,
    // 0 = park without flipping. Exposed for the harness.
    static int chooseDir(const GameEngine& engine, int lane, int sub);

    // =====================================================================
    // FIELD ACCESSORS -- the ONLY place the scratch-field mapping is spelled
    // out (see the header comment). When arcade_enemies.md sec 3.3's `ArcadeState`
    // union lands, only these bodies change.
    // =====================================================================
    static int&   mode  (Enemy& e)       { return e.current_lane; }
    static int    mode  (const Enemy& e) { return e.current_lane; }
    static int&   sub   (Enemy& e)       { return e.sidestep_freq; }
    static int    sub   (const Enemy& e) { return e.sidestep_freq; }
    static int&   pause (Enemy& e)       { return e.shoot_freq; }
    static int    pause (const Enemy& e) { return e.shoot_freq; }
    static int&   spin  (Enemy& e)       { return e.animation_vec; }
    static int    spin  (const Enemy& e) { return e.animation_vec; }
    static int&   marker(Enemy& e)       { return e.animation_phase; }
    static int    marker(const Enemy& e) { return e.animation_phase; }
    static int&   stamp (Enemy& e)       { return e.max_animation; }
    static int    stamp (const Enemy& e) { return e.max_animation; }
    static float& total (Enemy& e)       { return e.px; }
    static float  total (const Enemy& e) { return e.px; }
    static float& rate  (Enemy& e)       { return e.py; }
    static float  rate  (const Enemy& e) { return e.py; }
    static float& turned(Enemy& e)       { return e.anchor_rot; }
    static float  turned(const Enemy& e) { return e.anchor_rot; }
    // THE BEAST'S HORN COUNTER, 2 down to 0. Zero on every other sub-variant.
    //
    // It goes in `cross_t` because that is one of the only two fields the map
    // above leaves genuinely unused, and because the recovery names it (its
    // sec 7.3): NOT `spin()`/animation_vec, which the Beast's own maker
    // explicitly clears and which a promotion path would collide with. Stored
    // as a float in an int-valued field, in the same style as
    // ArcadeSpiker::mode(), so the mapping stays a two-line accessor pair and
    // models.h is untouched.
    static int  horns(const Enemy& e)   { return (int)e.cross_t; }
    static void setHorns(Enemy& e, int n) { e.cross_t = (float)n; }

    // Remaining rotation of the current flip, in degrees, always >= 0.
    static float remaining(const Enemy& e);

    // Per-sub-variant derived values, so no second table has to be kept in
    // step with `Sub`.
    //
    // BOTH ARE `constexpr` SO THE BEAST'S TWO "unchanged" DELTAS CAN BE PINNED
    // AT COMPILE TIME (see the static_asserts below the struct). Constexpr on
    // an already-inline one-expression body changes no generated code and no
    // behaviour -- it only lets a static_assert read them.
    static constexpr float rateFor(int subVariant) {
        return (subVariant == SUB_SUPER2) ? FlipDegPerTick * SuperTwoRateMul
                                          : FlipDegPerTick;
    }
    static constexpr int pauseReloadFor(int subVariant) {
        return (subVariant == SUB_PLAIN) ? PauseTicks : PauseNever;
    }
};

// ---- Compile-time binding of the Beast's shared constant -------------------
// BeastHitDz and ArcadeReflectedShot::AbsorbDz / ArcadeMirror::HitDz are ONE
// recovered number (the reference's non-destructive collision helper, called
// by both families) written down in two headers so that neither has to include
// the other. 6 of 160 world-z is exactly representable in binary32, so this is
// an exact equality and not a tolerance; arcade_mirror.h carries the matching
// assert on its side, and between them a one-sided edit fails to compile.
static_assert(ArcadeFlipper::BeastHitDz == 0.9375f,
              "the Beast's bullet-notice window is 6 world-z -- the same "
              "recovered constant the Mirror uses; edit both or neither");
static_assert(ArcadeFlipper::pauseReloadFor(ArcadeFlipper::SUB_BEAST)
                  == ArcadeFlipper::PauseNever,
              "the Beast's 'no pause' is INHERITED from pauseReloadFor's "
              "non-plain answer (recovery delta 3), not written separately -- "
              "if that function ever grows a per-sub table this must be "
              "re-checked rather than assumed");
static_assert(ArcadeFlipper::rateFor(ArcadeFlipper::SUB_BEAST)
                  == ArcadeFlipper::FlipDegPerTick,
              "the Beast must NOT get the x2 rotation: it is gated on "
              "sub-variant EXACTLY 2 (recovery delta 8 / item 10)");

} // namespace enemyfam
} // namespace ts
