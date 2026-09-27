#pragma once
// ============================================================================
// arcade_pulsar.h -- THE ARCADE PULSAR (`ARCADE_PULSAR`) and the PULSAR SPARK
// (`ARCADE_PULSAR_SPARK`) it becomes at the rim. Two object types, one family,
// one translation unit -- exactly the arcade_spiker.h pattern, where the spiker
// and the SPIKE it builds share a header because one is the product of the
// other and neither can be read without the other.
//
// Implements the enemy_family.h contract; read that header first.
//
// Spec: docs/design/arcade_pulsar_recovery.md (893 lines, every claim carrying
// a file:line citation). Implementation contract: docs/design/arcade_enemies.md
// (units sec 1, shared/per-set split sec 3.4, collision sec 3.5, scoring
// sec 3.6). Shared-path map: docs/design/enemy_pipeline_audit.md.
//
// PROVENANCE, and the IP boundary. Behaviour, orderings and numeric constants
// were READ from the two reference trees -- "the arcade reference", which
// DOCTRINE.md makes the AUTHORITATIVE baseline, cross-checked against "the later
// port" -- and NOTHING is copied: not a line, not a table. Everything below is
// re-authored. Same boundary DOCTRINE.md already applies to the claw, the camera
// and the powerup ladder, and the same explicit exception to "MECHANICS ARE
// GATED": the "original" being matched here is the arcade reference, not this
// engine's own shipped exe, so neither `fidelity-verifier` nor `exe-comparator`
// may correct any of it back toward the earlier PC-port binary.
//
// ============================================================================
// WHAT IT IS, IN ONE PARAGRAPH
// ============================================================================
//
// The pulsar spawns in a random lane, materialises on the far plane, and falls
// down that ONE lane in a dead-straight line at a speed that NEVER changes, on
// any level. It cannot change lane, cannot steer, cannot fire. Its entire
// threat is THE PULSE: a single 16-phase counter shared by the whole fleet,
// which extends and contracts its bolt shape and, for ONE phase in sixteen,
// ELECTRIFIES ITS WHOLE LANE. In that phase it kills the player anywhere in
// that lane, at any depth, with no proximity window of any kind. At the rim it
// does not die -- it becomes TWO SPARKS running opposite ways round the web.
//
// ============================================================================
// THE SIX THINGS THAT ARE EASY TO GET WRONG (recovery sec 8)
// ============================================================================
//
//  1. THE KILL TEST HAS NO DEPTH WINDOW AT ALL. Every other enemy in both
//     rosters catches the player inside a |dz| window; the pulsar uses the
//     lane-only variant of the reference's catch routine, which tests lane
//     equality and the claw's vulnerability and NOTHING ELSE. Adding a
//     proximity window "because every other enemy has one" destroys the
//     mechanic: the electrified lane is lethal along its WHOLE length, and
//     that is why it flashes. See `killsPlayerInLane()`.
//
//  2. THE ONLY ESCAPE IS JUMPING. The reference's third condition is that the
//     claw's own depth is still at or beyond the rim plane ("check if on top
//     (may be jumping!)"). In this engine the claw rests at z = 0 and a jump
//     drives z NEGATIVE (player.cpp's parabola bottoms at -6.25), so the port
//     spelling is `player.z >= RimPlaneZ` -- the same expression player.cpp's
//     own electrocution test already uses. Nothing else grants immunity: not
//     distance, not firing, not the superzapper.
//
//  3. THE PULSE IS ONE GLOBAL COUNTER, NOT PER-ENEMY STATE. Every pulsar,
//     every spark and every pulsar-tanker on screen breathes IN UNISON, and
//     the crackle fires ONCE FOR THE WHOLE FLEET per cycle. A per-object phase
//     would look wrong and would multiply the sound. See "THE GLOBAL PULSE"
//     below for where it lives and why it is not on GameEngine yet.
//
//  4. LETHAL IS PHASE 7 ONLY -- NOT 7 AND 8. The shape is at full extension
//     for two phases (`PULSE_SHAPE[7] == PULSE_SHAPE[8] == 5`) but only the
//     FIRST bites. Getting this wrong doubles the danger window.
//
//  5. A PULSAR ALWAYS LANDS HARMLESS. The touchdown hold parks it at the far
//     plane, adding its approach step straight back, frame after frame, until
//     the global phase drops below `HoldUntilPhase`. Skipping it produces
//     pulsars that materialise ALREADY LETHAL in the player's lane, which
//     neither reference can do. Nothing else in either roster does this.
//
//  6. THE RIM SPLIT IS NOT A DEATH. It scores nothing, plays no explosion,
//     drops no capsule, and produces two live enemies that pay 150 EACH -- so
//     a pulsar allowed to land is worth 300, more than the 200 for shooting it
//     in flight. Two different scores from two different table entries: a
//     single `Score` constant for the family would be wrong.
//
// ============================================================================
// THE ADJUDICATED DIVERGENCES -- do not re-litigate, do not "correct" back
// ============================================================================
//
// (a) THE SECOND SPARK'S DIRECTION. At the rim the pulsar becomes spark A in
//     place and clones spark B. The AUTHORITATIVE source writes the second
//     direction to the WRONG OBJECT (itself, not the clone), so the original
//     ends the frame running -1 and THE CLONE'S DIRECTION IS WHATEVER THE
//     RECYCLED POOL SLOT HAPPENED TO CONTAIN. The later port writes it
//     correctly. DOCTRINE.md records the user's decision: THE LATER PORT WINS
//     for this one case, because reproducing pool-recycling order is noise,
//     not fidelity. So: A runs +1, B runs -1, both written explicitly.
//
// (b) THE SECOND SPARK'S COLOUR, a SECOND instance of the same slip found by
//     this recovery and not previously recorded: the authoritative source
//     never writes the clone's colour byte either (it is not among the copied
//     fields), so the clone also inherits the recycled slot's colour. Same
//     defect class, same ruling: the clone is explicitly yellow. Here that
//     falls out for free -- the clone is a struct copy of spark A, so it
//     cannot inherit anything but spark A's own state.
//
// (c) THE SPARK'S CATCH WINDOW. The spark -- unlike the pulsar -- uses the
//     ORDINARY lane-plus-depth catch routine, and that is the routine
//     DOCTRINE.md's adjudicated conflict 2 is about: the authoritative 2
//     world-z, NOT the later port's ~8 (which comes from scaling that one test
//     by the X/Y factor where every sibling constant in its own file uses the
//     Z factor). This family therefore reads `ArcadeFlipper::ParkedGrabDz`
//     (0.3125f) rather than declaring a second copy -- ONE adjudicated number,
//     ONE definition, so the two families can never drift apart on it.
//
// ============================================================================
// THE GLOBAL PULSE -- where it lives, and why it is not on GameEngine
// ============================================================================
//
// The recovery (sec 6 item 8) is explicit that the counter belongs on
// GameEngine beside `arcade_fire_timer`, cleared in `init_level`. IT IS NOT
// THERE, by decision rather than by omission: the state is a file-local POD in
// arcade_pulsar.cpp, reached only through the `pulse*()` statics below, because
// `pulseShape()` is a no-argument static the RENDERER calls
// (entity_geometry.cpp:438) with no GameEngine in hand -- engine-side storage
// would leave a second copy
// shadowing this one. `init_level` calls `pulseReset()` instead and `game_step`
// drives `pulseTick()`; registration item 6 at the bottom of this header carries
// the recorded reasoning, and the call site in engine.cpp carries it verbatim.
//
// It is a plain struct of three ints and a float: no allocation, no dispatch,
// no STL, nothing per-frame beyond one add and one compare (DOCTRINE.md's
// "C with classes").
//
// `pulseTick()` is IDEMPOTENT PER ENGINE TICK -- it stamps the tick it last
// ran on -- so it is correct whether it is driven once from a game_step slot
// (the end state; see registration item 6) or from the top of every family
// update() (what happens today). Both is fine; the second call is a no-op.
//
// ============================================================================
// TICK RATE: THE PULSE IS CARRIED AS A FRACTIONAL ARCADE-TICK ACCUMULATOR
// ============================================================================
//
// arcade_enemies.md sec 1.1 fixes the rule for this roster: WALL CLOCK IS
// PRESERVED, NOT TICK COUNTS. The reference logic ticks at 70.2617 Hz and this
// engine at 62.5, so an arcade TICK COUNT must be multiplied by 0.88953 to
// keep the same real duration.
//
// For the pulse that conversion cannot be rounded to an integer, and the
// reason is specific rather than fussy: THE PULSE RATE IS THE PULSAR'S ENTIRE
// DIFFICULTY SCALING. It has no per-wave speed table (searched: the sibling
// tables for the flipper, spiker and fuseball have no pulsar counterpart, and
// the one z-speed constant has exactly one definition and one read in each
// tree), so the six reload bands ARE the ramp. Rounded to whole engine ticks
// the two hardest bands (5 and 4 arcade frames per phase) BOTH land on 4, and
// the top of the difficulty curve disappears.
//
// So `pulseTick` accumulates ARCADE ticks (+= `TICK_RATE` per engine tick) and
// fires a phase when the accumulator reaches the band's integer frames-per-
// phase. Wall clock is then exact on every band, all six bands stay distinct,
// and the cost is one float add and one compare PER ENGINE TICK for the whole
// fleet. The same technique the spiker used for its build allowance
// (arcade_spiker.h "A TICK COUNT IS RATE-DEPENDENT AND WE ARE RESCALING THE
// RATE"), applied to the one other place a rate-dependent count is the
// difficulty.
//
// CONSEQUENCE, stated so it is not read as a bug: a phase's length in ENGINE
// ticks jitters by one (8,8,8,...,9 at wave 0; 4,3,4,4,3,... at the hardest
// band). The lethal WINDOW is still exactly one phase; only its tick count
// varies, by up to 16 ms. NB the flipper's `PauseTicks = 8` takes the other
// choice (an arcade count used raw as an engine count) -- the tree is
// inconsistent about this and the report says so; this file states its own
// reasoning rather than silently matching either side.
//
// ============================================================================
// WHERE THE PER-ENEMY STATE LIVES
// ============================================================================
//
// arcade_enemies.md sec 3.3 specifies a named `ArcadeState` union on `Enemy`.
// models.h does not have it and this wave may not edit shared files, so the
// state is carried in existing `Enemy` fields THROUGH THE NAMED ACCESSORS
// below -- never by open-coding a scratch field at a use site. When
// `ArcadeState` lands, these bodies are the only things that move.
//
//   PULSAR
//     Enemy::animation_phase  -> MODE. 0 == ARRIVAL, non-zero == DESCEND.
//          Same encoding, for the same reason, as ArcadeTanker: a
//          ZERO-INITIALISED pulsar is a valid ARRIVAL pulsar, and testing for
//          non-zero rather than == 1 makes it immune to the shared
//          `_transfer_enemy`'s `animation_phase = -animation_phase` (audit
//          sec 7/R3). The pulsar never changes lane, so that is
//          belt-and-braces -- but it costs nothing.
//     everything else          -> UNUSED, left at 0.
//
//   SPARK
//     Enemy::animation_vec    -> STEP DIRECTION, +1 or -1.
//     Enemy::oo_max_animation -> propagation accumulator, in ARCADE ticks.
//     Enemy::max_animation    -> last-updated tick stamp (see below).
//     Enemy::animation_phase  -> UNUSED, left at 0.
//
// !! THE SPARK NEEDS THE TICK STAMP AND IT IS NOT OPTIONAL. `move_enemies`
//    walks lanes 0..n-1, so a spark that steps to a HIGHER lane index is
//    visited AGAIN in the same frame and would take a second step -- at the
//    web's seam that compounds into a spark that laps the ring. Same guard,
//    same field, same reason as ArcadeFlipper::stamp. Delete it the day
//    `move_enemies` grows a transfer guard of its own.
//
// !! CONSEQUENCE FOR THE RENDERING WAVE: `animation_phase`, `animation_vec`,
//    `max_animation` and `oo_max_animation` are NOT animation counters on
//    either of these. entity_geometry.cpp's generic per-enemy animation block
//    must never run for an arcade id. It cannot: `buildEnemies` branches on
//    isArcadeEnemy() BEFORE that block, routes arcade ids through
//    getArcadeEnemyGeometry / enemyModelMatrixArcade and `continue`s, so
//    getEnemyGeometry is never reached for one at all. (The per-enemy
//    isArcadeEnemy() branch predates this Wave-2 family, so it superseded
//    nothing here; the guard that actually skips a rowless arcade id is the
//    null-ArcadeGeometry `continue` inside the arcade branch itself
//    (entity_geometry.cpp:1004). getEnemyGeometry still has no arcade case,
//    so that fallback remains true, it is simply not what protects this.) The
//    pulsar's own builder HAS
//    LANDED -- one case in entity_geometry.cpp covering BOTH ids -- and it
//    honours the rule: the SHAPE it draws is a pure function of the global
//    counter, read through `pulseShape()`, and of nothing on the Enemy.
// ============================================================================

#include "enemy_family.h"
#include "arcade_flipper.h"   // ParkedGrabDz -- the ONE adjudicated catch window
#include "../sfx.h"

namespace ts {
namespace enemyfam {

// ============================================================================
// ArcadePulsar -- the enemy, and the owner of the fleet-wide pulse.
// ============================================================================
struct ArcadePulsar {
    // The ids this family claims (enemy_family.h step 1). The `switch` in
    // enemies.cpp move_enemies is the single dispatch and must agree.
    static constexpr int Ids[] = { ARCADE_PULSAR };

    // ---- MODES -------------------------------------------------------------
    // The pulsar HAS NO STATE MACHINE. The recovery is explicit: it reads no
    // per-object state other than lane, depth and the draw index, and it has
    // exactly two conditions -- inbound collapsed dot, and descending -- with
    // the shared materialisation test (plus the touchdown hold) between them.
    // These two names exist so the encoding above has words rather than
    // magic numbers.
    enum Mode {
        MODE_ARRIVAL = 0,   // the inert collapsed dot falling in from behind
        MODE_DESCEND = 1,   // live: pulsing, electrifying, lethal on phase 7
    };

    // ======================================================================
    // UNIT CONVERSION (arcade_enemies.md sec 1)
    //
    // Deliberately LOCAL to this family rather than in a shared
    // arcade_common.h, matching arcade_spiker.h / arcade_tanker.h /
    // arcade_flipper.h: enemy_family.h step 2 puts a family's constants next
    // to the code that reads them. FOUR families now carry near-identical
    // copies of this block, which is a real (small) duplication -- the report
    // asks for the hoist as its own change, since doing it here would edit
    // three files this wave may not touch.
    // ======================================================================

    // The reference logic tick. The later port reprograms the interval timer
    // with divisor 0x4256, giving 1193181.67/16982 = 70.2617 Hz; the arcade
    // reference's own object runner ticks with it (the shared per-wave tables
    // are bit-identical, and the pulse reload table below is one of them).
    static constexpr float ARCADE_HZ = 70.2617f;
    // This engine's tick: TICK_MS = 16 (game_step.cpp drains dif_time in
    // TICK_MS steps, the same structure the reference main loop has).
    static constexpr float ENGINE_HZ = 62.5f;
    static constexpr float TICK_RATE  = ARCADE_HZ / ENGINE_HZ;  // 1.1241872 : scale a per-tick DELTA
    static constexpr float TICK_COUNT = ENGINE_HZ / ARCADE_HZ;  // 0.8895329 : scale a tick COUNT

    // DEPTH. The reference tube is 160 length ("world-z") units deep and this
    // engine's is GRID_ELEMENT_LENGTH, z = 25 (far) down to z = 0 (rim).
    // PROPORTIONAL, and deliberately NOT DOCTRINE.md's camera law of "1 port
    // unit == 256 reference units" -- they map different things
    // (arcade_enemies.md sec 1.2 says to keep them in separate named constants
    // so nobody folds them).
    static constexpr float UNIT_Z = GRID_ELEMENT_LENGTH / 160.0f;   // 0.15625

    // ======================================================================
    // GEOMETRY
    // ======================================================================

    static constexpr float FarPlaneZ = GRID_ELEMENT_LENGTH;   // 25.0, the materialise plane
    static constexpr float RimPlaneZ = 0.0f;                  // 0.0, the claw's plane

    // The arrival: placed on the far plane, then pushed a further 300 world-z
    // back and flagged "collapsed to a pixel". 25 + 300*0.15625 = 71.875 --
    // BIT-IDENTICAL to ArcadeFlipper::SpawnZ and ArcadeTanker::SpawnZ, because
    // it is the same shared insertion path (static_assert at the bottom).
    static constexpr float SpawnBackWorldZ = 300.0f;
    static constexpr float SpawnZ = FarPlaneZ + SpawnBackWorldZ * UNIT_Z;   // 71.875

    // ---- APPROACH ----------------------------------------------------------
    // The shared collapsed-object head closes exactly 2 world-z per tick. A
    // per-tick DELTA, so it takes the RATE conversion.
    //
    // THE RECOVERY'S sec 6 ITEM 5 IS CLOSED HERE, in this family's favour and
    // the tanker's: it flags `ArcadeTanker::ApproachDz = -0.3513085` as "off
    // by ~12%" against the recovered 2 world-z = 0.3125 engine z. The 12% IS
    // `TICK_RATE`. The recovered number is the pure unit conversion; the port
    // additionally preserves WALL CLOCK (arcade_enemies.md sec 1.1), so
    // 0.3513085 engine z per ENGINE tick is the same real closing speed as
    // 2 world-z per REFERENCE tick. The tanker is right, the flag is resolved,
    // and this family uses the identical number so the two arrival dots close
    // at the same rate (static_assert at the bottom).
    static constexpr float ApproachWorldZPerTick = 2.0f;
    static constexpr float ApproachDz = ApproachWorldZPerTick * UNIT_Z * TICK_RATE;  // 0.3513085
    // Informational: 46.875 / 0.3513085 = 133.4 engine ticks = 2.135 s, plus
    // however long the touchdown hold below detains it.
    static constexpr float ApproachTicks = 133.43f;

    // ---- DESCENT -----------------------------------------------------------
    // THE SPEED NEVER CHANGES, on any level. There is NO per-wave pulsar
    // z-speed table -- searched in both trees, the constant has exactly one
    // definition and one read in each, and the sibling per-wave tables the
    // flipper, spiker and fuseball use have no pulsar counterpart. Difficulty
    // scaling for pulsars is carried ENTIRELY by the pulse rate (and by the
    // spawn period, which is the generator's business, not this family's).
    //
    // 0.5 world-z per reference tick. NB this is numerically the tanker's
    // wave-0 descent (16 raw/tick == 0.5 world-z/tick) -- an independent
    // cross-check that the two conversions agree, not a shared constant.
    static constexpr float DescentWorldZPerTick = 0.5f;
    static constexpr float DescentDz = DescentWorldZPerTick * UNIT_Z * TICK_RATE;   // 0.0878271
    // Full traverse: 25 / 0.0878271 = 284.65 engine ticks = 4.554 s, on EVERY
    // level. (The reference's own 320 ticks / 70.2617 Hz = 4.554 s. Equal, and
    // that equality is the whole point of the rate conversion.)
    static constexpr float DescentTicks = 284.65f;

    // ======================================================================
    // THE PULSE
    // ======================================================================

    // 16 phases, wrapping. The counter is masked, never compared for equality
    // with 16, so this is a mask not a modulus.
    static constexpr int PulsePhases = 16;
    static constexpr int PulseMask   = PulsePhases - 1;

    // LETHAL IS EXACTLY THIS ONE PHASE (recovery sec 1.3 / sec 8.3). NOT 7 and
    // 8 -- the bolt is fully extended for both, only the first kills.
    static constexpr int LethalPhase = 7;

    // THE TOUCHDOWN HOLD (recovery sec 1.2, "the pulsar-only touchdown hold").
    // A pulsar that has reached the far plane while the global phase is at or
    // above this is pushed straight back out by the step it just took, and
    // tries again next tick. The reference's own comment is "makes pulsars
    // wait until innocent before touchdown". Phases 0,1,2 are the rest window
    // AND the only window in which a new pulsar may land.
    static constexpr int HoldUntilPhase = 3;
    // Worst case: 13 of 16 phases held = 117 reference frames at the slowest
    // pulse band. Informational, and the harness proves the invariant it
    // implies (a pulsar is never lethal on the tick it materialises).
    static constexpr int MaxHoldPhases = PulsePhases - HoldUntilPhase;   // 13

    // ---- THE SHAPE PING-PONG ----------------------------------------------
    // 16 phases -> 6 amplitude frames, byte-identical in both sources. Index 0
    // is the flat resting bar, index 5 the fully extended bolt. The renderer
    // reads this through pulseShape() (entity_geometry.cpp, the ARCADE_PULSAR /
    // ARCADE_PULSAR_SPARK case), which is a pure function of the global counter
    // and NEVER of per-object animation state -- that is what makes the fleet
    // breathe in unison. `pulseAmplitude()` is the same frame normalised to
    // 0.0..1.0 and has NO CALLER today: the "brightness rides amplitude" ramp
    // is baked per frame into ARCADE_PULSAR_COLORS instead
    // (enemy_data_arcade.h), so the accessor is there for anyone who later
    // wants the continuous form.
    static constexpr int ShapeFrames = 6;
    static constexpr int ShapeMax    = ShapeFrames - 1;   // 5
    static constexpr int PULSE_SHAPE[PulsePhases] = {
        0, 0, 0, 1, 2, 3, 4, 5, 5, 4, 3, 2, 1, 0, 0, 0
    };

    // ---- THE RATE TABLE ----------------------------------------------------
    // 28 entries, identical byte-for-byte in both sources, selected ONCE per
    // wave by `index = floor(wave / 4)` (the source's own comment is "word
    // every 4 waves"). `wave` is engine.current_level, 0-BASED -- do not pass
    // the HUD's number, it would shift the whole ramp by a table row.
    //
    // The stored value is a RELOAD for a decrement-then-test-sign countdown,
    // so FRAMES PER PHASE = reload + 1 (a reload of 8 permits 9 passes).
    //
    //   idx 0-4   waves  0- 19   reload 8   ->  9 frames/phase, cycle 144
    //   idx 5     waves 20- 23   reload 7   ->  8              , cycle 128
    //   idx 6     waves 24- 27   reload 6   ->  7              , cycle 112
    //   idx 7     waves 28- 31   reload 5   ->  6              , cycle  96
    //   idx 8-27  waves 32-111   6,5,4,3 repeating every 16 waves
    //
    // NB the recovery's prose summarises the tail as "a 4-group ramp 7/6/5/4
    // -> 6/5/4/3"; its own explicit per-wave table (which is complete, has no
    // gaps or overlaps, and totals exactly 28 words) says 7/6/5 then 6/5/4/3
    // repeating. THE EXPLICIT TABLE IS WHAT IS IMPLEMENTED -- flagged in the
    // report as the one place the two halves of the recovery disagree.
    static constexpr int PulseTableLen = 28;
    static constexpr int PULSE_RELOAD[PulseTableLen] = {
        8, 8, 8, 8, 8,          // waves   0- 19
        7, 6, 5,                // waves  20- 31
        6, 5, 4, 3,             // waves  32- 47
        6, 5, 4, 3,             // waves  48- 63
        6, 5, 4, 3,             // waves  64- 79
        6, 5, 4, 3,             // waves  80- 95
        6, 5, 4, 3,             // waves  96-111
    };

    // Frames per phase for this wave, in REFERENCE ticks. Clamped into the
    // table: the reference relies on its own wave wrap to stay in range and
    // would read one word past the table on waves 112-115 (recovery UNCERTAIN
    // sec 7.4 -- benign, and unreachable in this port's 100 levels).
    static int   pulseFramesPerPhase(int level);
    // ...and the same thing in ENGINE ticks, for harnesses and for anyone
    // reasoning about the window in real time. Fractional ON PURPOSE; see the
    // tick-rate block in the header comment.
    static float pulsePhaseTicks(int level);

    // ---- THE GLOBAL COUNTER, read side ------------------------------------
    static int   pulsePhase();       // 0..15
    static bool  pulseLethal();      // phase == LethalPhase, and nothing else
    static int   pulseShape();       // 0..5, PULSE_SHAPE[phase]
    static float pulseAmplitude();   // pulseShape() / ShapeMax, i.e. 0.0 .. 1.0

    // ---- THE GLOBAL COUNTER, write side -----------------------------------
    // Advance the pulse for THIS SIMULATION TICK. Idempotent: the second and
    // later calls with the same `tick_ms` do nothing, so it is correct whether
    // it is driven once from a game_step slot (the end state) or from the top
    // of every family update() (what happens today).
    //
    // !! `tick_ms` MUST BE THE TICK CLOCK -- `EnemyCtx::time`, i.e. the
    //    `tick_ms` game_step.cpp hands move_enemies -- and NOT `engine.time`.
    //    They are different clocks and the difference is a real defect, not a
    //    nicety: `engine.time` is a WALL-CLOCK RENDER stamp written once per
    //    RENDERED FRAME by main.cpp / main_3ds.cpp, while game_advance's fixed
    //    -timestep accumulator batches up to MAX_ACCUM_MS/TICK_MS = 15 sim
    //    ticks into one frame, all of which would read the SAME engine.time.
    //    Driving the pulse off it would make the pulse rate FRAME-RATE
    //    DEPENDENT -- and the 3DS build is GPU-bound at ~29 ms/frame
    //    (DOCTRINE.md), i.e. two sim ticks per frame, so the whole fleet would
    //    breathe at HALF SPEED on the target platform and at full speed on the
    //    desktop oracle. The tick clock advances +TICK_MS per sim tick by
    //    construction, which is the only thing that makes this deterministic.
    //
    // It also owns the wave reset the reference performs at every wave start:
    // the phase, the accumulator and the crackle latch all go to 0 when
    // `engine.current_level` changes, or when the tick clock runs BACKWARDS (a
    // new run, or a fresh GameEngine in a harness)... or when `init_level`
    // calls `pulseReset()` (engine.cpp), which is what covers a death
    // re-entering the same level.
    static void  pulseTick(GameEngine& engine, int tick_ms);
    // Force the counter back to wave-start. Exposed for `init_level` (the
    // proper home) and for harnesses that drive several engines in one process.
    static void  pulseReset();

    // ======================================================================
    // SCORING
    // ======================================================================
    // 200 points. The kill index decodes to two increments of the hundreds
    // digit, and the later port states the number outright as a literal -- so
    // this is a two-source agreement, not a table reading. NOTE IT DIFFERS
    // FROM THE SPARK'S (150): they are two different table entries and a
    // single family-wide constant would be wrong.
    //
    // Passed as init_explosion's `energy`, which is what the arcade roster
    // scores through: the shared `_handle_death` derives score from `energy`
    // and would otherwise pay the arcade life of 1 (audit sec 6/D4). Reached
    // via enemies_shared.h `arcade_kill_score()` -- registration item 2.
    static constexpr int Score = 200;

    // ======================================================================
    // PRESENTATION (free under DOCTRINE.md -- recorded, not gated)
    // ======================================================================
    // THE CRACKLE. On entering the lethal phase the FIRST live pulsar to
    // notice plays it and sets a fleet-wide latch, so it fires ONCE PER PULSE
    // CYCLE FOR THE WHOLE FLEET rather than once per pulsar; the latch clears
    // when the phase wraps to 0. That mechanism is recovered and is
    // reproduced exactly; the SAMPLE is not, because this port has no crackle
    // in its bank. THUNDER is the game's own electricity voice (the zapper
    // beam), pitched up and quiet, pushed as a ONE_SHOT -- which takes the
    // round-robin one-shot channel and cannot disturb the beam's own dedicated
    // looping channel (audio/sfx_3ds.cpp gates on `action != ONE_SHOT`).
    // Flagged for audio review exactly as ArcadeFlipper::FlipSfx is.
    //
    // TRAP, recorded so nobody "fixes" this to the obvious candidate: the
    // effect the archivists' comments name "Pulsar Pulse" is NEVER PLAYED BY A
    // PULSAR -- its only call sites are the mirror reflect and the two shield
    // reflects. Do not wire it here.
    static constexpr SfxId CrackleSfx      = SfxId::THUNDER;
    static constexpr float CrackleSfxPitch = 1.7f;
    static constexpr float CrackleSfxVol   = 0.45f;

    // The player-death popup. The authoritative source shows a short "fried
    // you" message before the shared death routine; the later port shows none.
    // Presentation, therefore free -- and id 14 IS "fried you" (constants.h
    // POWERUP_TEXT, "T2K 32 -> 30 (electrocuted)"), which is also what the game
    // already says when an electrified enemy kills you (player.cpp's el-zapper
    // / rectangle electrocution) -- so this matches the reference's wording
    // rather than departing from it. The spark uses the same one, because it is
    // the same electricity.
    static constexpr int DeathTextId = 14;

    // ======================================================================
    // ENTRY POINTS
    // ======================================================================

    // Per-tick update. RETURN CONTRACT (enemy_family.h): true iff this call
    // REMOVED the enemy from `lane`'s slot `idx`. For this family that is
    // exactly one case -- THE RIM SPLIT, where the slot is left holding a
    // SPARK and a second spark may have been pushed into the same vector
    // (which can reallocate, invalidating `enemy`). The caller must then
    // neither touch `enemy` nor advance `idx`; re-reading the slot gives it
    // spark A, which is what should happen.
    static bool update(GameEngine& engine, const EnemyCtx& ctx,
                       int lane, int idx, Enemy& enemy);

    // The level generator's pulsar (enemy_family.h optional entry point).
    // UNIFORM-RANDOM LANE OVER THE WHOLE WEB, both end lanes included: the
    // reference's pulsar maker deliberately does NOT exclude them (the tanker
    // maker does, and that exclusion is load-bearing there because it keeps
    // the tanker's two children in distinct adjacent lanes -- a pulsar has no
    // children at spawn). So the caller's lane is used as given, exactly like
    // ArcadeFlipper::spawn. Returns false if the lane was full.
    static bool spawn(GameEngine& engine, int lane);

    // THE SHOOTABLE PREDICATE (audit sec 2/C9, sec 9/Z4). The pulsar IS
    // SHOOTABLE IN EVERY PHASE, INCLUDING THE LETHAL ONE -- the bullet test is
    // called unconditionally before any phase test, and nothing in either tree
    // gates damage on the pulse (every read of the counter in both trees was
    // enumerated: the tick, the shape select, the phase test, the touchdown
    // hold, and three draw routines. None gates damage). There is no
    // "invulnerable while pulsing" rule; do not invent one.
    //
    // The ONE exception is the arrival dot, and it is the same rule the
    // flipper and tanker dots follow: the live routine is not dispatched at
    // all while an object is collapsed, and the bullet test lives INSIDE that
    // routine -- so bullets, the superzapper and the tremor all pass straight
    // through an inbound pulsar.
    static bool shootable(const Enemy& e) { return mode(e) != MODE_ARRIVAL; }

    // THE METAMORPHOSIS SEAM (recovery sec 3.2; audit sec 7/R4). A
    // pulsar-tanker's two children are born ORDINARY FLIPPERS, flip exactly
    // one lane, and become pulsars THERE -- so the pulsar-tanker's payload is
    // "flipper, then pulsar", never a pulsar directly. arcade_flipper.h
    // already carries the marker (`MARKER_PULSAR = -2`) and arcade_flipper.cpp
    // already has the branch; both were written as a seam waiting for a
    // DESTINATION. This is that destination, and it is deliberately the whole
    // of it: the recovery adds no new mechanism there.
    //
    // It does what the reference's own conversion does and NOTHING MORE: swap
    // the runner (here, the id), clear the grab marker, re-centre on the lane
    // midpoint. It does not touch depth, life, or the enemy flag -- all
    // inherited -- which is what makes the new pulsar CLAW-LETHAL FROM THAT
    // INSTANT rather than starting a fresh arrival.
    //
    // It also fixes BOTH per-id counters, which is the whole of audit R4 for
    // this one transition: rewriting `enemy.id` in place without it leaves
    // `enemies_nums[old]` never decremented and drives `enemies_nums[new]`
    // NEGATIVE on death, which then lets the level over-spawn that type for
    // the rest of the wave.
    //
    // REACHED FROM `arcade_flipper.cpp`'s MODE_STOPPED marker branch, when a
    // hatchling's first flip lands with `marker(enemy) == MARKER_PULSAR`. The
    // pulsar-tanker is still the only thing that marks -2
    // (`ArcadeTanker::openChildren`), but that path is now LIVE end to end:
    // `ArcadeTanker::PulsarReady` is true, `payloadReady(ARCADE_PULSAR_TANKER)`
    // therefore passes, and the spawn bit is set from wave 33 onward
    // (`enemy_spawns.h _arcHasPulsarTanker`: 33-48, 51, 62, 63, 69-71, 80-98).
    // Do not treat this function as dead.
    static void becomeFromFlipper(GameEngine& engine, int lane, Enemy& enemy);

    // THE KILL TEST, exposed so a harness can prove the two properties that
    // matter without driving a whole tick: it fires at ANY depth, and jumping
    // is the only escape. `lane` is the pulsar's lane. Reads no per-object
    // state at all except the lane -- which is the point (recovery sec 1.4).
    static bool killsPlayerInLane(const GameEngine& engine, int lane);

    // ---- Enemy field mapping (see the header comment) ---------------------
    // MODE reads for non-zero rather than == MODE_DESCEND so that
    // `_transfer_enemy`'s negate cannot corrupt it (audit sec 7/R3).
    static int  mode(const Enemy& e) {
        return (e.animation_phase != 0) ? MODE_DESCEND : MODE_ARRIVAL;
    }
    static void setMode(Enemy& e, int m) {
        e.animation_phase = (m == MODE_DESCEND) ? 1 : 0;
    }
};

// ============================================================================
// ArcadePulsarSpark -- what a pulsar becomes at the rim.
//
// Kept a SEPARATE NAMED SCOPE rather than folded into ArcadePulsar for the
// same reason ArcadeSpike is separate from ArcadeSpiker: it is a different
// object with a different score, a different catch rule and a different
// lifetime, and it outlives the thing that made it. The two only share the
// pulse -- and the pulse is global, so they share it the same way the
// pulsar-tanker does.
//
// It has NO lifetime, NO depth motion and NO self-destruct. It runs round the
// rim until something stops it, and only two things can: being shot (or
// zapped), and killing the player.
// ============================================================================
struct ArcadePulsarSpark {
    static constexpr int Ids[] = { ARCADE_PULSAR_SPARK };

    // ---- PROPAGATION -------------------------------------------------------
    // The step counter is a decrement-then-test-sign countdown with reload 4,
    // so the spark advances ONE LANE EVERY 5 REFERENCE TICKS -- and there is
    // NO per-wave propagation table (searched both trees: the constant has one
    // definition and two reads, both inside the split, in each). Ring speed is
    // constant for the whole game.
    //
    // Carried as a fractional accumulator in REFERENCE ticks for the same
    // reason the pulse is (see the header): 5 reference ticks is 4.45 engine
    // ticks, and rounding either way costs 10-12% of the ring speed on the one
    // enemy whose entire threat is how fast it comes round at you.
    static constexpr int   PropReload = 4;
    static constexpr float PropFrames = (float)(PropReload + 1);   // 5 reference ticks
    // Informational: 5 / 1.1241872 = 4.4477 engine ticks per lane, so a
    // 16-lane ring takes 71.16 engine ticks = 1.139 s per circuit -- the same
    // real time as the reference's 80 ticks at 70.2617 Hz.
    static constexpr float PropEngineTicks = PropFrames / ArcadePulsar::TICK_RATE;

    // ---- CATCH -------------------------------------------------------------
    // Unlike the pulsar, the spark uses the ORDINARY lane-plus-depth catch
    // routine -- the one DOCTRINE.md's adjudicated conflict 2 settles at 2
    // world-z. Read from the flipper rather than re-declared, so the ONE
    // adjudicated number has ONE definition (divergence (c) in the header).
    // In practice both bodies sit at the rim, so this is "same lane" plus a
    // small tolerance that a jumping claw immediately leaves.
    static constexpr float CatchDz = ArcadeFlipper::ParkedGrabDz;   // 0.3125

    // ---- SCORE -------------------------------------------------------------
    // 150 -- the FLIPPER's entry, not the pulsar's 200, because the spark dies
    // through the generic kill routine rather than the pulsar's own. Both
    // sources agree (the later port states it as a literal). A pulsar allowed
    // to reach the rim therefore ultimately pays 300 across its two sparks,
    // MORE than the 200 for shooting it in flight -- that asymmetry is the
    // recovered behaviour, not a mistake to be smoothed out.
    static constexpr int Score = 150;

    // ======================================================================
    // ENTRY POINTS
    // ======================================================================

    // Per-tick update. Returns true iff this call REMOVED the enemy from
    // `lane`'s slot `idx` -- which for this family means it stepped to another
    // lane (a transfer counts as removal: the slot now holds a different
    // enemy).
    static bool update(GameEngine& engine, const EnemyCtx& ctx,
                       int lane, int idx, Enemy& enemy);

    // Always shootable: no arrival state, no invulnerable window, one hit
    // anywhere kills. Spelled out rather than left to a default so a family
    // that later grows a window has an obvious place to say so.
    static bool shootable(const Enemy&) { return true; }

    // Where a spark comes from. THE ONLY CONSTRUCTOR -- nothing else in either
    // tree creates one, and nothing else in this port should either. Converts
    // `pulsar` IN PLACE into spark A (direction +1) and pushes spark B
    // (direction -1) into the SAME lane. Returns the number of sparks now
    // alive from this split (2 normally; 1 if the lane had no room for the
    // clone -- see the .cpp, that cap is this port's, not the reference's).
    //
    // Both are stamped with the current tick so NEITHER acts on the frame it
    // was created: the recovery's UNCERTAIN sec 7.6 asks exactly whether a
    // fresh spark can act on its creation frame and could not resolve it, so
    // this takes the deterministic reading. They ARE shootable immediately.
    static int splitFromPulsar(GameEngine& engine, const EnemyCtx& ctx,
                               int lane, int idx, Enemy& pulsar);

    // ---- Enemy field mapping (see the header comment) ---------------------
    // DIRECTION reads the sign only, so a magnitude that somehow drifted still
    // resolves to a legal step.
    static int   dir  (const Enemy& e)   { return (e.animation_vec >= 0) ? +1 : -1; }
    static void  setDir(Enemy& e, int d) { e.animation_vec = (d >= 0) ? +1 : -1; }
    static float& prop (Enemy& e)        { return e.oo_max_animation; }
    static float  prop (const Enemy& e)  { return e.oo_max_animation; }
    static int&   stamp(Enemy& e)        { return e.max_animation; }
    static int    stamp(const Enemy& e)  { return e.max_animation; }
};

// ============================================================================
// Compile-time bindings. Every value here is exactly representable in binary32
// (they are all n/64ths of GRID_ELEMENT_LENGTH, or exact products of two such),
// so these are equalities, not tolerances. They exist so that editing ONE of a
// coupled pair without the other fails at COMPILE time -- the same discipline
// constants.h's "descriptor shear" asserts use.
// ============================================================================

static_assert(ArcadePulsar::UNIT_Z == 0.15625f,
              "reference->engine depth scale (GRID_ELEMENT_LENGTH/160)");

// The arrival is the SHARED collapsed-object path: three families spawn at the
// same depth and close at the same rate, and if they ever stop agreeing it is
// because someone edited one copy of a shared rule.
static_assert(ArcadePulsar::SpawnZ == ArcadeFlipper::SpawnZ,
              "the arrival dot spawns on the SHARED far-plane-plus-300 depth");
// ...and closes at the same rate. NOT an exact equality, and the reason is
// worth a line: the flipper stores this as the 5-digit ROUNDED LITERAL
// 0.35131f while this family DERIVES it from the same three factors
// (2 world-z * UNIT_Z * TICK_RATE = 0.35130850), so they differ by 1.5e-6 --
// about one part in 230,000, i.e. 0.0002 engine z over a whole 133-tick
// approach. The tolerance is tight enough that a real edit to either side
// (the smallest meaningful change to any factor moves it by >1e-4) fails the
// build. The end state is one derived constant in a shared arcade_common.h --
// see the report's hoist request.
static_assert((ArcadePulsar::ApproachDz > ArcadeFlipper::ArrivalDz
                   ? ArcadePulsar::ApproachDz - ArcadeFlipper::ArrivalDz
                   : ArcadeFlipper::ArrivalDz - ArcadePulsar::ApproachDz) < 1.0e-4f,
              "the arrival dot closes at the SHARED 2 world-z/tick");

// The adjudicated catch window, bound to its single definition AND to its
// value, so neither a re-edit of the flipper nor a stray copy here can move it
// without a build failure. DOCTRINE.md adjudicated conflict 2.
static_assert(ArcadePulsarSpark::CatchDz == 0.3125f,
              "spark catch window: the ADJUDICATED 2 world-z, not the later port's 8");

// The lethal phase must be a FULLY EXTENDED frame, and phase 8 must be the
// other one -- i.e. the shape table and the lethal index have to stay
// consistent with each other. If someone re-authors the ping-pong, this fires.
static_assert(ArcadePulsar::PULSE_SHAPE[ArcadePulsar::LethalPhase] == ArcadePulsar::ShapeMax,
              "the lethal phase IS the fully extended bolt");
static_assert(ArcadePulsar::PULSE_SHAPE[ArcadePulsar::LethalPhase + 1] == ArcadePulsar::ShapeMax,
              "full extension lasts TWO phases; only the FIRST is lethal");
static_assert(ArcadePulsar::PULSE_SHAPE[0] == 0
                  && ArcadePulsar::PULSE_SHAPE[ArcadePulsar::PulsePhases - 1] == 0,
              "the ping-pong rests flat at both ends of the cycle");
// The touchdown window and the rest window are THE SAME WINDOW: a pulsar may
// only land during the phases whose shape is flat and whose predecessor is
// also flat. That is what "always materialises harmless" means, and it is why
// HoldUntilPhase is 3 and not, say, 4.
static_assert(ArcadePulsar::HoldUntilPhase < ArcadePulsar::LethalPhase,
              "a pulsar must land well before the lethal phase");
static_assert(ArcadePulsar::PULSE_SHAPE[ArcadePulsar::HoldUntilPhase - 1] == 0,
              "the last phase in which touchdown is allowed is a FLAT one");

static_assert(ArcadePulsar::PulseTableLen == 28,
              "the pulse reload table is 28 words, covering waves 0-111");

} // namespace enemyfam
} // namespace ts

// ============================================================================
// REGISTRATION -- APPLIED. Every item below has LANDED; the list is kept as
// the record of what the shared-file pass had to do and why, not as work
// outstanding. Where the implementation differs from what an item asked for,
// the difference is called out in the item itself. Do not re-apply any of it.
//
// REGISTRATION -- what the shared-file pass must apply. Listed here so the
// requirement travels with the family instead of only in a report. Every one
// of these is an edit this wave is forbidden to make, and the family is INERT
// until they land: an unregistered id falls to `default:` in move_enemies and
// does nothing at all (audit sec 1).
//
//  1. game/enemies.cpp move_enemies -- the dispatch:
//         case ARCADE_PULSAR:
//             deleted = enemyfam::ArcadePulsar::update(engine, ctx, v, v2, enemy);
//             break;
//         case ARCADE_PULSAR_SPARK:
//             deleted = enemyfam::ArcadePulsarSpark::update(engine, ctx, v, v2, enemy);
//             break;
//     WITHOUT THIS neither moves, neither pulses and neither can ever die,
//     which also means a level containing one CAN NEVER CLEAR.
//
//  2. game/enemies/enemies_shared.h -- three of the six seams:
//       a. `enemy_shootable()`:
//              case ARCADE_PULSAR:       return ArcadePulsar::shootable(e);
//              case ARCADE_PULSAR_SPARK: return ArcadePulsarSpark::shootable(e);
//          Without (a) the inbound pulsar dot is shootable, zappable and
//          tremor-killable for its whole ~133-tick approach.
//       b. `arcade_kill_score()`:
//              case ARCADE_PULSAR:       return ArcadePulsar::Score;        // 200
//              case ARCADE_PULSAR_SPARK: return ArcadePulsarSpark::Score;   // 150
//          Without (b) both pay 0 (the switch's default), and it will read as
//          a scoring bug rather than a missing case. THEY ARE DIFFERENT
//          NUMBERS; one shared row would be wrong.
//       c. `arcade_release()`:
//              case ARCADE_PULSAR: return ArcadePulsar::spawn(engine, lane);
//          The caller's uniform-random lane is used AS GIVEN (contrast the
//          tanker, whose lane is discarded). ARCADE_PULSAR_SPARK gets NO case:
//          it is never released by the generator, only by a split, so the
//          `default: return false;` arm is the correct answer for it.
//     `enemy_uses_shared_player_collision()` and `arcade_enemy_die()` need NO
//     change: the first already excludes every arcade id (both families own
//     their player test inside update()), and neither of these has a custom
//     corpse -- the shared explosion/score/removal is the whole of their death.
//
//  3. game/enemy_spawns.h -- the arcade spawn table. Bit `i` == EnemyId `i`,
//     so the pulsar is `1 << ARCADE_PULSAR` (14 + 8 = 22). THE SPARK'S BIT
//     MUST STAY CLEAR FOREVER: it is not a spawnable type, and setting it
//     would put sparks on the far plane with nothing to walk to.
//     The recovered wave schedule, for whoever curates the table: the first
//     pulsar is on wave 17 (1-based) and they run through wave 48, return at
//     58-61 and 64, and are present on most waves from 76 up; waves 64, 70 and
//     71 are the deliberate saturation waves. This port's level ORDER is its
//     own curation (DOCTRINE.md "One level format"), so that schedule is
//     evidence for the shape of the ramp, not a table to transplant.
//
//  4. rendering/entity_geometry.cpp getEnemyGeometry (audit sec 10/G1) -- an
//     id with no geometry row returns all-null and buildEnemies `continue`s,
//     so the enemy is FULLY SIMULATED, FULLY LETHAL AND COMPLETELY INVISIBLE
//     on both backends. That is exactly how SP_ZAPPER1 shipped for months, and
//     for THIS enemy it is far worse than usual: the pulsar kills from any
//     depth in its lane, so an invisible one is an unavoidable death.
//     What it needs (recovery sec 1.6, the authoritative shaded form):
//       - ONE body, six amplitude frames, shared by the pulsar AND the spark
//         (the spark reuses the pulsar's shape and animation outright).
//       - A thick ribbon: 10 vertices / 6 triangles as a strip, spanning the
//         full lane width, symmetric about the lane midpoint. Between the two
//         endpoints are THREE extremes -- one central apex on one side and two
//         flanking troughs on the other -- each extreme a PAIR of vertices a
//         little apart, which is what gives the ribbon its thickness. The
//         silhouette is a symmetric chevron.
//       - The six frames differ ONLY in the amplitude of those extremes, from
//         a near-flat bar at rest to a bold bolt at full extension. Read the
//         frame from `ArcadePulsar::pulseShape()` / `pulseAmplitude()` -- the
//         GLOBAL counter, never per-object state, or the fleet stops breathing
//         in unison.
//       - BRIGHTNESS RIDES AMPLITUDE (per-face colour and per-vertex intensity
//         step down at rest and reach maximum at full extension). That is
//         DOCTRINE.md's "hue is identity, intensity is event" already; implement
//         it as an intensity ramp, NEVER a hue cycle.
//
//  5. game/constants.h -- the lane-fill pair the shared builder needs, beside
//     the flipper's and the tanker's:
//         constexpr float ARCADE_PULSAR_MODEL_FULL_WIDTH = 18.0f;   // recovered
//         constexpr float ARCADE_PULSAR_LANE_FILL        = 1.0f;    // recovered
//     ONE LANE WIDE IS RECOVERED, not chosen: the body spans the lane exactly,
//     which is what makes an electrified lane read as electrified. Sizing it
//     to the lane rather than to a fixed model scale is mandatory here for the
//     same reason it was for the claw (DOCTRINE.md: the fixed 0.09 scale made
//     the claw 1.44 lanes wide) -- this port's webs include rows whose lane
//     vectors run 0.90-1.25.
//
//  6. game/engine.h + game/engine.cpp -- move the pulse onto GameEngine
//     (recovery sec 6 item 8), beside `arcade_fire_timer`:
//         int   arcade_pulse_phase = 0;
//         int   arcade_pulse_last  = 0;      // SIM TICK stamp, not engine.time
//         float arcade_pulse_acc   = 0.0f;   // reference ticks
//         bool  arcade_pulse_said  = false;  // crackle latch
//     and clear all four in `init_level`, which is the ONE case the interim
//     file-local copy cannot cover: a DEATH re-enters the same level, so
//     `current_level` does not change and the phase carries across instead of
//     restarting at 0. (Everything else IS covered -- a level change and a
//     backwards clock both reset it.) The accessor bodies in arcade_pulsar.cpp
//     are the only things that then move.
//     Driving `ArcadePulsar::pulseTick(engine, tick_ms)` from a game_step slot
//     as well is optional and free (the call is idempotent per tick); it buys
//     exact fidelity for the case where the counter should keep running with no
//     pulsar alive. PASS game_step's `tick_ms`, NEVER `engine.time` -- see the
//     note on pulseTick: engine.time is a per-rendered-frame stamp and the
//     accumulator batches up to 15 sim ticks behind one frame, which on the
//     GPU-bound 3DS would halve the pulse rate.
//     LANDED DIFFERENTLY -- this is the difference the preamble promises to
//     call out. THE FOUR `arcade_pulse_*` FIELDS WERE NOT ADDED to engine.h,
//     deliberately: `pulseShape()` is a NO-ARGUMENT static the RENDERER calls
//     (entity_geometry.cpp picks the pulsar's shape frame from it with no
//     GameEngine in hand); `pulsePhase()` is family-internal (arcade_pulsar.cpp)
//     and `pulseAmplitude()` has no caller today. So moving the storage onto
//     the engine while keeping those
//     signatures would leave a second copy shadowing the first -- strictly
//     worse than one owner. The family keeps SOLE OWNERSHIP of the file-local
//     POD in arcade_pulsar.cpp. What landed instead:
//       - `enemyfam::ArcadePulsar::pulseReset()` called from `init_level`
//         (engine.cpp), which supplies exactly the death-re-enters-the-same-
//         level trigger this item was written for. The judgement is recorded
//         verbatim at that call site.
//       - the optional game_step slot, which WAS taken: game_step.cpp drives
//         `pulseTick(e, t)` with the TICK clock under
//         `enemy_set == ENEMY_SET_ARCADE`, so the counter keeps running with
//         no pulsar alive.
//     Do not add the fields.
//
//  7. game/enemies/arcade_flipper.cpp -- close the metamorphosis seam. Its
//     MODE_STOPPED marker branch currently lets a -2 hatchling fall through to
//     the -1 path with a comment saying the pulsar is Wave 2; the destination
//     now exists:
//         if (marker(enemy) == MARKER_PULSAR) {
//             ArcadePulsar::becomeFromFlipper(engine, lane, enemy);
//             return false;
//         }
//     and then `ArcadeTanker::PulsarReady` may flip to true and the
//     pulsar-tanker's spawn bit may be set. Do those together: the tanker's
//     own static_asserts fail the BUILD if a spawn-mask row asks for a variant
//     whose payload gate is false, which is what stops a half-wired pulsar
//     tanker from ever reaching a play-test.
//
//  8. CMakeLists.txt SOURCES -- add `src/game/enemies/arcade_pulsar.cpp` beside
//     the other three families. Makefile.3ds picks it up by wildcard already
//     (its globs are three directory levels deep), so the 3DS side needs no
//     edit -- which also means THE 3DS BUILD COMPILES THIS FILE TODAY AND THE
//     DESKTOP BUILD DOES NOT. A family that lands on one target only is
//     INCOMPLETE, not "phase one" (DOCTRINE.md, TARGET PARITY IS POLICY).
//
//  9. NOT NEEDED, checked and recorded so nobody adds them "for completeness":
//     - `_apply_movement` already returns early for every arcade id, so the
//       double-step (audit sec 5/M1) cannot happen here.
//     - `ENEMY_DESC`'s two rows already exist with `dz = 0` and `life = 1`,
//       and constants.h already static_asserts the zero. Life 1 is
//       load-bearing: it is what makes "one hit kills, the laser pierces" fall
//       out of the shared mutual-subtraction exchange for free.
//     - The shot-consumption rule (`arcade_kill_consumes_shot`) is id-agnostic
//       and already correct for both.
// ============================================================================
