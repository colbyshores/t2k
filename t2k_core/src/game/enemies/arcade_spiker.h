#pragma once
// ============================================================================
// arcade_spiker.h -- ARCADE_SPIKER (arcade roster, generator id 2) and the SPIKE it
// builds. Implements the enemy_family.h contract; see that header first.
//
// PROVENANCE, and the IP boundary. Behaviour, orderings and numeric constants
// were READ from the arcade reference trees (`the arcade reference/` = the arcade reference build,
// which DOCTRINE.md makes the AUTHORITATIVE baseline, cross-checked against
// `the later port/`); both are gitignored and NOTHING is copied -- not a line, not a
// table. Everything below is re-authored. Same boundary DOCTRINE.md already
// applies to the claw, the camera and the powerup ladder, and the same
// explicit exception to "MECHANICS ARE GATED": the "original" being matched
// here is the arcade reference, not this engine's own shipped exe, so neither
// `fidelity-verifier` nor `exe-comparator` may correct any of it back toward
// the reference source / the shipped PC port.exe.
//
// The written contract is docs/design/arcade_enemies.md §2.3. That document's
// spiker recovery was flagged "recovery truncated at the tail" and its §8.8
// left four questions open. FOUR of them are CLOSED here off the arcade reference
// source, and the divergences are called out at each constant:
//
//   1. WHAT ENDS THE CLIMB when the build counter runs out. The truncated
//      recovery never said. The arcade reference decrements the counter and, when it
//      goes NEGATIVE, sets the descend mode -- the spiker TURNS AROUND. It
//      does NOT keep climbing to the turnaround plane building nothing.
//   2. THE BUILD COUNTER IS 51 ELIGIBLE TICKS, NOT 50. It is decrement-then-
//      test-sign, so a reload of 50 permits 51 passes, and the growth for the
//      51st has already been applied when the counter underflows. The spec's
//      "50 x 26 raw" is one tick short.
//   3. THE SHORTEST-SPIKE SENTINEL IS A LENGTH, NOT A DEPTH. The spec reads
//      the arcade's 6400 raw as "z = 24.7" by mapping it onto the depth axis;
//      it is 6400/32 = 200 LENGTH units, an ordinary min-search initialiser
//      sitting above the 150-unit cap. (The arcade reference spells the same sentinel
//      as the bare literal 200, which settles it.) Read as a depth it would
//      make the search select nothing.
//   4. THE GROWTH FRACTION IS CARRIED, not discarded. The spec records the
//      the later port build re-deriving the fraction every tick and calls float
//      accumulation a deliberate bug-fix; the arcade reference accumulates in 16.16
//      properly, so float accumulation here is FIDELITY to the baseline, not
//      a fix. (arcade_enemies.md §8.8(c) can be closed as "not a defect on the
//      authoritative source".)
//
// ---- WHAT IS SHARED WITH THIS ENGINE'S OWN SPIKER, AND WHAT IS NOT ---------
//
// SHARED, unchanged: the spike STORAGE. `GridElement::spike` is a per-lane
// height in engine z with the tip at `GRID_ELEMENT_LENGTH - spike`, which is
// exactly the arcade's own model (a spike is a lane property, not an object --
// arcade_enemies.md §2.3). Also shared: linegeom::buildSpikeSegs and SPIKE_COLOR
// (one lethal hazard, one colour, both backends), the player-side lethality
// test in player.cpp, note_powerup_kill()'s capsule cadence, init_explosion's
// score/multiplier/SFX economy, and the shot sweep that finds the hit.
//
// DUPLICATED (i.e. this family has its own): every RULE about that number --
// who grows it, how fast, to what ceiling, how much a hit takes off, and what
// a hit is worth. The classic spiker (_ai_spiker, enemies.cpp) grows the spike
// by one ENEMY_DZ step while it is ahead of the tip, caps at 0.85/1.15 x the
// tube by enemy id, retreats at z < 0.666 x the tube and re-picks a lane at
// random on retreat; SPIKE_EROSION_PER_HIT chips 1/15th of the tube. NONE of
// those numbers survives here, and they must not be unified: the classic set
// is ported-gameplay and gated, this set is the arcade, and DOCTRINE.md's
// TARGET-PARITY rule is about the two BUILDS agreeing, not about the two
// ROSTERS agreeing.
// ============================================================================

#include "enemy_family.h"

namespace ts {
namespace enemyfam {

// ============================================================================
// ArcadeSpiker -- the enemy. Three modes, exactly as the arcade dispatch table
// orders them: SEEK a spike, CLIMB it while building, DESCEND back out.
// ============================================================================
struct ArcadeSpiker {
    // The ids this family claims (enemy_family.h step 1). The `switch` in
    // enemies.cpp move_enemies is the single dispatch and must agree.
    static constexpr int Ids[] = { ARCADE_SPIKER };

    // ---- MODES -------------------------------------------------------------
    // The arcade dispatches these through a 3-entry vector table in exactly
    // this order; the numbering is load-bearing only in that mode 0 is the
    // state a freshly built spiker is in.
    enum Mode {
        SEEK    = 0,   // choose a lane: a clear one if any, else the shortest spike
        CLIMB   = 1,   // travel toward the rim, extending the spike behind us
        DESCEND = 2,   // travel back out to the far plane, building nothing
    };

    // ======================================================================
    // UNIT CONVERSION (docs/design/arcade_enemies.md §1)
    //
    // Deliberately LOCAL to this family rather than in a shared arcade_common.h:
    // enemy_family.h step 2 puts a family's constants next to the code that
    // reads them, and three arcade families are being authored in parallel --
    // a shared header would be a merge conflict, not a shared truth. Hoist
    // these only once a SECOND family is in the tree and needs them.
    // ======================================================================

    // The arcade logic tick. The later port build reprograms the PIT with divisor
    // 0x4256, giving 1193181.67/16982 = 70.2617 Hz; the arcade reference's own object
    // runner ticks with it (the shared per-wave tables are bit-identical).
    static constexpr float ARCADE_HZ = 70.2617f;
    // This engine's tick: TICK_MS = 16 (game_step.cpp drains dif_time in
    // TICK_MS steps, the same structure the arcade main loop has).
    static constexpr float ENGINE_HZ = 62.5f;
    // WALL CLOCK IS WHAT IS PRESERVED, NOT TICK COUNTS (arcade_enemies.md §1.1):
    // everything else in this port (claw ramp, camera OMEGA, droid glide) is
    // tuned in engine ticks against real seconds, so a roster running 12% slow
    // would read as sluggish rather than as faithful.
    static constexpr float TICK_RATE  = ARCADE_HZ / ENGINE_HZ;  // 1.1241872 : scale a per-tick DELTA
    static constexpr float TICK_COUNT = ENGINE_HZ / ARCADE_HZ;  // 0.8895328 : scale a tick COUNT

    // DEPTH. The arcade tube is 160 length units deep -- the arcade reference: the spiker's
    // base plane is webz+80 and the rim is webz-80; the later port: 6464 raw down to 1344
    // raw at 32 raw per length unit, = 5120 raw = 160. This engine's tube is
    // GRID_ELEMENT_LENGTH deep, z = 25 (far) down to z = 0 (rim).
    //
    // This is a PROPORTIONAL law and it deliberately differs from DOCTRINE.md's
    // camera law of "1 port unit == 256 the later port units" -- they map different
    // things, and arcade_enemies.md §1.2 says to keep them in separate named
    // constants so nobody folds them. An enemy must spawn on the far plane and
    // arrive exactly at the rim; proportional is the only law under which that
    // is true on every one of this port's 100 webs.
    static constexpr float UNIT_Z = GRID_ELEMENT_LENGTH / 160.0f;      // 0.15625 engine z per arcade length unit
    // The speed table below is stored in the later port build's raw 1/32-unit steps,
    // which is how both sources spell it (the arcade reference $d000 in 16.16 == 0.8125 ==
    // 26/32). This is engine z per raw unit.
    static constexpr float RAW_Z  = GRID_ELEMENT_LENGTH / 5120.0f;     // 0.0048828125

    // ======================================================================
    // GEOMETRY (arcade_enemies.md §2.3; re-derived from the arcade reference's own symbols)
    // ======================================================================

    // Where a spiker begins and returns to: the far plane. Arcade webz+80.
    static constexpr float BaseZ = GRID_ELEMENT_LENGTH;                // 25.0

    // Where the climb reverses. Arcade webz-70, i.e. 10 length units short of
    // the rim -- which is ALSO exactly where a maximum-length spike's tip sits
    // (150 units below the base), so the spiker turns around at the ceiling of
    // its own work rather than at an arbitrary plane. STRICT less-than, and it
    // is evaluated AFTER the move but BEFORE any growth, so the turnaround
    // tick grows nothing and spends no build allowance.
    static constexpr float TurnaroundZ = 10.0f * UNIT_Z;               // 1.5625

    // ---- ROLL --------------------------------------------------------------
    // The body spins about the lane axis, every tick, in every mode. The
    // arcade adds 4 of its 256 units per revolution = 5.625 deg per arcade
    // tick. (arcade_enemies.md flags that the extractor reported the bare addend
    // "16" from the later port build's 1024-unit circle, which transplanted into a
    // port that assumes degrees is a 3x error. Both sources agree on 5.625.)
    static constexpr float RollDegPerArcadeTick = 5.625f;
    static constexpr float RollDegPerTick = RollDegPerArcadeTick * TICK_RATE;  // 6.32355 deg
    // => one revolution per 56.93 engine ticks / 0.911 s.

    // ---- SCORE FOR KILLING THE SPIKER --------------------------------------
    // 150 points, the same as a flipper: the spiker's death path enters the
    // generic kill routine with score-table index 0, which is the 150 entry.
    // (The table is (digit position, repeat count) pairs; index 0 is 15 x tens,
    // index 1 is 1 x hundreds = the tanker's 100, index 2 is 3 x units = the
    // spike's 3 -- three known values decoding consistently, which is what
    // pins the reading.)
    //
    // NOT USED BY THIS TU, and that is audit finding D4: _handle_death passes
    // `enemy.life` as init_explosion's `energy`, and init_explosion turns
    // `energy` into SCORE -- so with the arcade life of 1, every arcade kill
    // would score ONE POINT and the whole roster would read as a scoring bug.
    // arcade_enemies.md §3.6 names the fix (the arcade death path passes the
    // arcade SCORE as energy); this constant is that number for this family.
    static constexpr int KillScore = 150;

    // ---- BUILD ALLOWANCE ---------------------------------------------------
    // The arcade reloads a build counter of 50 every time the spiker ADOPTS a
    // lane, and decrements it once per tick on which the spiker is ahead of
    // the spike's tip -- INCLUDING ticks where the cap blocked the growth, so
    // the predicate is "the spiker is ahead of the tip", not "growth
    // occurred". When the counter goes NEGATIVE the spiker turns around.
    //
    // It is decrement-then-test-sign, so a reload of 50 permits 51 passes
    // (50, 49 ... 0 all continue; the 51st decrement underflows) and the 51st
    // pass has ALREADY grown before the underflow. Hence 51, not the spec's 50.
    // A TICK COUNT IS RATE-DEPENDENT AND WE ARE RESCALING THE RATE, so the
    // allowance is carried as a DISTANCE instead (buildAllowance() below) --
    // rate-independent, and equivalent because the speed is constant within a
    // level. It is spent by the same step the spike grows by, so a cap-blocked
    // tick still spends it. At wave 0-1: 51 x 26/32 = 41.4375 arcade units =
    // 6.4746 engine units.
    static constexpr int BuildTicks = 51;

    // ---- THE SHORTEST-SPIKE SENTINEL ---------------------------------------
    // The min-search initialiser, in SPIKE LENGTH units -- the arcade reference spells
    // it as the literal 200, the later port build as 6400 raw (= 200 x 32). It sits
    // above the 150-unit growth cap so any real spike replaces it, and its
    // only other job is to answer "were there any spikes at all?".
    // NB arcade_enemies.md maps it onto the depth axis as "z = 24.7"; that
    // reading is wrong and would select nothing (see the header note).
    static constexpr float ShortestSentinel = 200.0f * UNIT_Z;         // 31.25 engine units

    // ---- CLIMB / DESCEND SPEED --------------------------------------------
    // 56 entries indexed by floor(wave / 2) -- consecutive wave PAIRS share a
    // value. Stored in the later port build's raw 1/32-unit steps, which is the form
    // both sources agree on bit-for-bit (the arcade reference 16.16 $d000 == 26/32 == 0.8125
    // length units per tick). DESCENT USES THE SAME VALUE as the climb.
    //
    // The table covers waves 0..111. The arcade relies on its own wave-wrap to
    // stay in range; this port clamps instead -- see speedRaw().
    static constexpr int SpeedTableLen = 56;
    static constexpr int SpeedRaw[SpeedTableLen] = {
        // floor(wave/2) 0..31 -- the same eight values, four times over
        26, 26, 28, 30, 32, 34, 36, 38,
        26, 26, 28, 30, 32, 34, 36, 38,
        26, 26, 28, 30, 32, 34, 36, 38,
        26, 26, 28, 30, 32, 34, 36, 38,
        // 32..39
        40, 40, 42, 42, 44, 44, 44, 44,
        // 40..47
        42, 44, 44, 46, 46, 48, 48, 48,
        // 48..55 -- the previous row repeated
        42, 44, 44, 46, 46, 48, 48, 48,
    };

    // This level's raw step, clamped into the table. `level` is
    // engine.current_level (0-based; the HUD adds one -- do not pass the HUD's
    // number, it would shift the whole ramp by half a table row).
    static int   speedRaw(int level);
    // ... the same step in engine z per ENGINE tick (the rate conversion).
    // 26 raw -> 0.142720 engine z/tick, matching arcade_enemies.md §1.4.
    static float stepZ(int level);
    // ... and the per-visit growth allowance in engine z (a DISTANCE, so NO
    // rate conversion). 26 raw -> 6.474609 engine z.
    static float buildAllowance(int level);

    // ======================================================================
    // ENTRY POINTS
    // ======================================================================

    // Per-tick update. RETURN CONTRACT (enemy_family.h): true iff this call
    // REMOVED the enemy from `lane`'s slot `idx` -- which for this family
    // means it adopted a different lane. The caller must then neither touch
    // `enemy` nor advance `idx`.
    static bool update(GameEngine& engine, const EnemyCtx& ctx,
                       int lane, int idx, Enemy& enemy);

    // Custom construction (enemy_family.h optional entry point). There is NO
    // arrival dot: unlike the flipper and the tanker, the arcade spiker is
    // built directly on the far plane, fully live, drawn normally. Picks its
    // lane uniformly over the whole web, as the arcade does.
    // Returns false if refused (semaphore held, or the lane is full).
    static bool spawn(GameEngine& engine);

    // THE ONE-SPIKER SEMAPHORE. The arcade zeroes a counter at level setup,
    // grants on ">= 0" and decrements, and returns one per confirmed kill --
    // i.e. exactly one spiker alive at a time, unlimited over the level.
    // This port already maintains that number: `enemies_nums[ARCADE_SPIKER]` is
    // incremented at spawn and decremented by _remove_enemy, so no new engine
    // state is needed and the "kill returns the token" half is automatic.
    //
    // (The arcade's own self-delete path gives back the object budget but NOT
    // this semaphore, permanently starving the spiker generator for the rest
    // of the level -- arcade_enemies.md §8.8(b) recommends FIXING it. Here the
    // path does not exist at all: it is only reachable when no object slot is
    // free for a new spike, and in this port a spike costs no slot, so the
    // defect is unreachable rather than fixed.)
    static bool canSpawn(const GameEngine& engine);

    // ---- Enemy field mapping ----------------------------------------------
    // models.h has no per-family state union (arcade_enemies.md §3.3 proposes
    // one; Wave A did not add it and this wave may not edit models.h), so the
    // three values this family carries live in generic Enemy fields. The
    // mapping is stated ONCE, here, and every access in the .cpp goes through
    // these accessors -- never a raw field name. If the union lands later,
    // only these lines change.
    //
    // MODE reads through abs() on purpose: enemies.cpp's shared
    // _transfer_enemy NEGATES animation_phase on every transfer (audit §7 R3,
    // a classic-family lane-crossing convention). This family does its own
    // lane move and never goes through it, but reading |phase| makes the
    // family immune if it ever does, for one compare.
    static int  mode(const Enemy& e)      { return e.animation_phase < 0 ? -e.animation_phase
                                                                        :  e.animation_phase; }
    static void setMode(Enemy& e, int m)  { e.animation_phase = m; }
    static float& roll(Enemy& e)          { return e.anchor_rot; }        // degrees since adopting the lane
    static float& budget(Enemy& e)        { return e.oo_max_animation; }  // engine z of growth left this visit
};

// ============================================================================
// ArcadeSpike -- the arcade rules for the LANE PROPERTY `GridElement::spike`.
//
// The storage is shared and unchanged; only the numbers are this family's.
// Kept a separate named scope from ArcadeSpiker because a spike outlives the
// spiker that built it and is acted on by code (collision.cpp, player.cpp)
// that has no spiker in hand.
// ============================================================================
struct ArcadeSpike {
    // Height a freshly created spike starts at: 30 arcade length units, 18.75%
    // of the tube. Written by the spiker at the moment it adopts a clear lane.
    static constexpr float Init = 30.0f * ArcadeSpiker::UNIT_Z;           // 4.6875

    // The growth ceiling, 150 arcade units = 93.75% of the tube. THE CAP IS A
    // SKIP, NOT A CLAMP: when the next step WOULD REACH it the store is
    // abandoned entirely, so there is no partial growth up to the cap and the
    // attained maximum is always one step short (see growStep()). A near-capped
    // spike still yields positive slack for one tick, grows nothing, and still
    // spends build allowance.
    static constexpr float Cap = 150.0f * ArcadeSpiker::UNIT_Z;           // 23.4375

    // ---- THE ADJUDICATED PAIR ---------------------------------------------
    // DOCTRINE.md, "ENEMY BEHAVIOUR: THE THE ARCADE REFERENCE IS THE BASELINE", adjudicated
    // conflict 1 (user decision 2026-08-20, do not re-litigate):
    //
    //     SPIKES: THE ARCADE REFERENCE. 3 units shrunk per hit, 3 points per hit.
    //     The later port's 2 and 10 are NOT to be used.
    //
    // The reason is recorded there: every other enemy is being tuned against
    // the arcade reference's numbers, and tripling one income stream would shift the
    // extra-life pacing the the arcade reference powerup-ladder work already tuned. If spikes
    // should pay better, that is an isolated, visible tuning change -- never
    // something inherited as a side effect of picking a source.
    //
    // arcade_enemies.md quotes the later port 2 and 10 in its body (its §2.3 table, and
    // the "~75 ordinary hits" balance flag in §3.5); the header's adjudication
    // table OVERRIDES that, and the arithmetic it changes is spelled out at
    // ShotsToClear below.
    static constexpr float Chip  = 3.0f * ArcadeSpiker::UNIT_Z;           // 0.46875 engine z per hit
    static constexpr int   Score = 3;                                  // points per hit

    // init_explosion() pays an EXPLOSION_SPIKE as
    //   round(energy * player.multiplier * 0.05)      (engine.cpp)
    // so the energy that pays exactly `Score` at multiplier 1.0 is Score/0.05.
    // Routing through init_explosion rather than award_score() is deliberate:
    // it is the one place the "tink" SFX, the +0.001 multiplier bump and the
    // explosion particles are raised, and arcade_enemies.md §3.6 is explicit that
    // the arcade set uses this engine's scoring economy unchanged.
    static constexpr float ScoreRate = 0.05f;                          // engine.cpp init_explosion
    static constexpr int   HitEnergy = (int)(Score / ScoreRate);       // 60

    // Shots needed to clear a spike, and the arithmetic behind the number:
    //   a FULL spike is one step short of Cap (the skip rule), 23.3838 engine
    //   units at wave 0-1; 23.3838 / 0.46875 = 49.885, and the hit that drives
    //   the height BELOW zero is the one that destroys it, so 50.
    //   a FRESH spike is 4.6875 / 0.46875 = 10 EXACTLY, and the 10th hit lands
    //   it on zero. The arcade tests the sign AFTER the subtraction, so zero
    //   length is still a spike there and an 11th hit is needed; here zero
    //   means absent to every consumer of the shared lane float, so 10 --
    //   the one deliberate divergence, spelled out at shotHit().
    // Under the later port numbers this would have been 75 and 15/16.
    // Under this engine's own classic rule (SPIKE_EROSION_PER_HIT, 1/15th of
    // the tube) it is 13 for a full SPIKER1 spike.
    static constexpr int ShotsToClearFull  = 50;
    static constexpr int ShotsToClearFresh = 10;

    // ---- THE FLASHING ("SUPER") SPIKE --------------------------------------
    // arcade_enemies.md §2.3 records this as MISSING from the extractor's model
    // entirely; the arcade reference carries it as a per-spike flag rolled at creation
    // under the SAME per-wave probability that promotes a flipper to super-2.
    // A flagged spike renders in the game's cycling flash colour instead of
    // green, and an ORDINARY claw shot cannot shrink it: the collision helper
    // spatters the shot into a spark and returns NO HIT. The particle laser
    // damages it normally (and IS consumed by it, unlike on a plain spike).
    //
    // Probability: 2 * min(wave, 127) out of 256, i.e. min(wave,127)/128 --
    // about 0.8% on wave 1, 50% around wave 64, near-certain past 127.
    //
    // NOT WIRED YET: it needs one bool per lane on GridElement, which is a
    // shared file this wave may not edit. rollSuper() is the roll; the two
    // predicates below take the flag as an explicit parameter (no default --
    // a default would silently ship every spike as non-super). See the report.
    static bool rollSuper(int level);

    // Does this shot remove height from this spike at all?
    static bool damages(int shot_id, bool superSpike);
    // Is this shot consumed by the hit? An ordinary shot always is (it
    // spatters into a spark even on a super spike it cannot hurt). THE LASER
    // IS NOT, on a plain spike -- it stays alive and keeps hitting once per
    // tick as it travels down the lane, and THAT PERSISTENCE, not a bigger
    // per-hit number, is what makes the laser eat spikes. It IS consumed by a
    // super spike.
    static bool consumesShot(int shot_id, bool superSpike);

    // Apply one shot's worth of interaction to lane `lane`'s spike: damage,
    // score, explosion, powerup cadence, and the shot's own fate. This is the
    // whole arcade replacement for collision.cpp's classic spike block, in one
    // call, so the integration there WOULD BE a two-way branch and nothing
    // else -- but THAT BRANCH DOES NOT EXIST YET. Nothing calls shotHit(),
    // nor damages()/consumesShot(), which are reachable only from it:
    // collision.cpp's "Shot vs Spike Collision" block is still unconditional
    // and chips SPIKE_EROSION_PER_HIT for BOTH rosters. So the adjudicated
    // Chip/score pair above is NOT in effect on a spike this family built --
    // a fresh one clears in 3 shots there, not the ShotsToClearFresh 10 below.
    // It cannot be wired from here in any case: `superSpike` has no default by
    // design and there is no per-lane flag to pass until GridElement carries
    // one (see NOT WIRED YET above). So this is shipped ahead of the
    // shared-file pass on purpose -- do NOT delete it, and do NOT wire it in a
    // comment pass: adding the branch changes spike erosion for the arcade
    // roster, which is a behaviour change.
    // Returns true iff the spike was destroyed by this hit.
    static bool shotHit(GameEngine& engine, int time, int lane, Shot& shot,
                        bool superSpike);

    // Create a spike on a clear lane at its starting height. Returns the
    // super-spike roll so the caller can store it once GridElement carries the
    // flag.
    static bool create(GameEngine& engine, int lane);

    // Add one growth step, honouring the SKIP-not-clamp cap. Returns true iff
    // the store happened (false = cap-blocked, which still spends allowance).
    static bool growStep(GameEngine& engine, int lane, float step);
};

// ---- Compile-time binding of the adjudicated pair to the geometry ----------
// Every value here is exactly representable in binary32 (they are all n/64ths
// of GRID_ELEMENT_LENGTH), so these are exact equalities, not tolerances. They
// exist so that editing ONE of {Chip, Cap, Init, the shot counts} without the
// others fails at COMPILE time -- the only place a silent shear in an
// adjudicated number is still cheap to catch. Same discipline as
// constants.h's "descriptor shear" asserts.
static_assert(ArcadeSpiker::UNIT_Z == 0.15625f,
              "arcade->engine depth scale (GRID_ELEMENT_LENGTH/160)");
static_assert(ArcadeSpike::Chip == 0.46875f,
              "spike chip: THE ARCADE REFERENCE 3 units/hit -- DOCTRINE.md adjudicated conflict 1");
static_assert(ArcadeSpike::Cap == ArcadeSpike::Chip * (float)ArcadeSpike::ShotsToClearFull,
              "the nominal cap IS exactly ShotsToClearFull chips");
static_assert(ArcadeSpike::Init == ArcadeSpike::Chip * (float)ArcadeSpike::ShotsToClearFresh,
              "a fresh spike IS exactly ShotsToClearFresh chips");
static_assert(ArcadeSpike::HitEnergy == 60,
              "energy that pays exactly Score through init_explosion's 0.05 rate");
static_assert(ArcadeSpiker::TurnaroundZ + ArcadeSpike::Cap == ArcadeSpiker::BaseZ,
              "the turnaround plane IS where a maximum-length spike's tip sits");

} // namespace enemyfam
} // namespace ts
