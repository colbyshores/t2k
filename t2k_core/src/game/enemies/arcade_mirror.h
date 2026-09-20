#pragma once
// ============================================================================
// arcade_mirror.h -- ARCADE_MIRROR (the arcade roster's generator id 8) and
// THE REFLECTED-SHOT RAIL both it and the BEAST hand your own bullets to.
//
// Implements the enemy_family.h contract; read that header first, then
// arcade_flipper.h / arcade_spiker.h for the two finished families this one is
// shaped after.
//
// Spec: docs/design/arcade_mirror_beast_recovery.md (every claim there carries
// a file:line citation). Shared-path map: docs/design/enemy_pipeline_audit.md.
// Implementation contract: docs/design/arcade_enemies.md sec 1 (units), sec 3
// (architecture), sec 3.5 / sec 3.6 (collision + scoring).
//
// PROVENANCE AND THE IP BOUNDARY, same terms arcade_spiker.h states: behaviour,
// orderings and numeric constants were READ from the arcade reference (the
// AUTHORITATIVE baseline per DOCTRINE.md) cross-checked against the later port;
// both trees are gitignored and NOTHING is copied -- not a line, not a table.
// Everything below is re-authored. This is the same explicit exception to
// "MECHANICS ARE GATED" that the claw, the camera and the powerup ladder
// already carry: the "original" being matched is the ARCADE reference, not
// this engine's own shipped exe, so neither `fidelity-verifier` nor
// `exe-comparator` may correct any of it back toward the reference source.
//
// ---- THE ONE SENTENCE ------------------------------------------------------
//
//   A Mirror or a Beast does not fire at you. It TAKES YOUR OWN BULLET AWAY
//   FROM YOU, TURNS IT ROUND, AND SENDS IT BACK DOWN THE LANE YOU SHOT IT UP.
//
// That is not a figure of speech and it is the single thing an implementer can
// most easily get wrong (recovery sec 10 item 1). The bullet OBJECT is
// re-tasked in place -- five writes, no allocation -- so it inherits its lane,
// its depth, its owner and the "this is not an enemy" classification. Three
// consequences fall straight out of that and all three are gameplay:
//
//   * it always comes back down YOUR OWN LANE, from exactly where it struck;
//   * it COSTS YOU A SHOT SLOT for as long as it lives (the slot is returned
//     only when it unlinks -- see releaseOwnerSlot below);
//   * it is NOT an enemy, so it is invisible to the level-clear test and can
//     never hold a level open.
//
// ---- NAMING TRAP, and it has already caught one reader ---------------------
//
// `src/game/enemies/reflector.{h,cpp}` and `REFLECT_SHOT1` (constants.h:420)
// are THIS ENGINE'S OWN classic-roster mechanism with a colliding name: a
// the reference source REFLECTOR1 parks short of the rim and its bounce SPAWNS a fresh
// REFLECT_SHOT1 through init_shot. That is a different thing -- an allocation,
// not a take-over -- and none of it may be reused or extended to carry this
// mechanism (recovery sec 7.1). The two must stay separate types.
// ============================================================================

#include "enemy_family.h"
#include "../sfx.h"

namespace ts {
namespace enemyfam {

// ============================================================================
// ArcadeReflectedShot -- THE SHARED RAIL.
//
// The recovery's headline architectural finding (sec 1.6, sec 10 item 3): the
// Mirror's reflected shot and the Beast's are ONE MECHANISM WITH THREE
// PARAMETERS, not two mechanisms. Their creation paths perform the same five
// writes and their flight/kill/expiry cores are a LITERAL SHARED CODE TAIL in
// the reference. They differ in exactly four things:
//
//     speed          full vs HALF
//     spin           none vs 4 units/tick
//     player shots   ignored entirely vs ABSORBED AND DESTROYED
//     shape          a glowing ring vs the horn the Beast just shed
//
// So this is one type with a `Kind` selector, and "two unrelated types is a
// mis-port" is stated in the recovery in those words.
// ============================================================================
struct ArcadeReflectedShot {

    // ---- WHICH SOURCE TURNED THE BULLET ROUND ------------------------------
    // The reference reaches these through two adjacent update-table slots (37
    // and 38) that share their tail; here it is one branch on one bit.
    enum Kind {
        KIND_MIRROR = 0,   // full speed, no spin, ignores player shots
        KIND_BEAST  = 1,   // half speed, spins, absorbs ordinary player shots
    };

    // ---- THE SHOT ID ------------------------------------------------------
    // Slot 9 of constants.h's ShotId enum, and it is now OURS BY NAME: the
    // wave-2 registration added `ARCADE_REFLECT_SHOT = 9` and gave it a real
    // SHOT_COLORS row (hot amber). SHOT_DZ / SHOT_LIFE / SHOT_MAX keep a ZERO
    // row for it deliberately -- see the SHOT_DZ note below.
    // It was chosen BECAUSE it was the one genuinely free row, and that is
    // still exactly what it bought: claiming it moved NO existing id
    // (ENEMY_SHOT1 is still 10, REFLECT_SHOT1 still 11), widened NO array
    // (SHOTS_NUM_IDS is still 12), and could not shift the classic roster by
    // one byte.
    //
    // THE NUMBER IS LOAD-BEARING, in two directions, and both were checked:
    //
    //   * 9 < ENEMY_SHOT1, and the superzapper's shot-target scan and the
    //     tremor's shot-destroy sweep are BOTH gated on `id >= ENEMY_SHOT1`
    //     (weapons.cpp). So "a reflected shot is not killed by the
    //     superzapper" (recovery sec 1.3, both rows NO) falls out for free,
    //     with no new gate anywhere.
    //   * ...but the same compare is what the two shot-vs-enemy sweeps call
    //     `is_player_shot`, so WITHOUT an explicit exclusion a reflected shot
    //     would damage enemies -- which neither reference routine does (they
    //     run no enemy test at all). THAT EXCLUSION IS APPLIED, in both
    //     sweeps, through `shot_is_arcade_reflected` (enemies_shared.h):
    //     collision.cpp branches the WHOLE pipeline out before its sweep is
    //     reached, and enemies.cpp skips explicitly inside its own. Do not
    //     re-apply it, and do not delete either one -- it is belt-and-braces
    //     rather than the only defence:
    //     SHOT_DZ[9] is 0, and with a zero dz on both sides the two sweep
    //     windows degenerate to exact float equality, which is the same
    //     property the arcade ENEMY rows already rely on (constants.h's
    //     "arcade descriptor dz must be 0" static_assert).
    //
    // SHOT_DZ[9] == 0 IS THE DESIGN, NOT AN OVERSIGHT -- exactly like
    // ENEMY_DZ being 0 for every arcade enemy. The reflected shot steps its
    // OWN speed (it has two, and they are derived from the bullet it took
    // over), so a non-zero row would be a silent second step per tick, which
    // is audit sec 5/M1 measured at 98% overspeed on the flipper's rail.
    // TWO MORE RECOVERED PROPERTIES FALL OUT OF BEING ITS OWN ID, with no
    // code anywhere, and both are worth knowing before they are reported as
    // bugs (recovery sec 1.1):
    //   * A REFLECTED SHOT CANNOT HOLD UP LEVEL COMPLETION. It inherits the
    //     "not an enemy" classification, which here means it stays a Shot and
    //     never becomes an Enemy -- and is_level_clear() counts only
    //     enemies_todo[] plus per-lane enemy/embryo populations.
    //   * IT IS NOT CHARGED AGAINST THE ALIEN-SHOT BUDGET. The one global
    //     fire cap counts shots_nums[ENEMY_SHOT1] (ArcadeFlipper::
    //     alienFireTick), and a reflected shot is counted under this id
    //     instead. So a screen full of Mirrors really can put more incoming
    //     fire in the air than the per-level cap would ever allow -- which is
    //     the reference's behaviour and, read as design, is the point of the
    //     enemy: THE PLAYER is the one raising the fire rate.
    static constexpr int SHOT_ID = 9;

    // The predicate every shared site needs. One integer compare; `false` for
    // every id the shipped game uses today.
    static constexpr bool is(int shot_id) { return shot_id == SHOT_ID; }

    // ======================================================================
    // UNIT CONVERSION (docs/design/arcade_enemies.md sec 1)
    //
    // LOCAL to this file, exactly as arcade_spiker.h keeps its own copy and
    // for the same stated reason: enemy_family.h step 2 puts a family's
    // constants next to the code that reads them. This is the FIFTH copy of
    // these four numbers in src/game/enemies/ -- arcade_spiker.h,
    // arcade_tanker.h (the same numbers spelled ArcadeHz / TsHz / ArcadeRate /
    // ArcadeZPerWorld, which is why no ARCADE_HZ grep finds it),
    // arcade_pulsar.h, arcade_fuseball.h and this block. arcade_flipper.h is
    // NOT a sixth copy: it restates the conversions in prose and bakes the
    // products as literals (SpawnZ 71.875f, ArrivalDz 0.35131f). Hoist them
    // into an arcade_units.h only in a change that does nothing else, exactly
    // as arcade_fuseball.h already says.
    // ======================================================================
    static constexpr float ARCADE_HZ  = 70.2617f;
    static constexpr float ENGINE_HZ  = 62.5f;
    // Scale a per-tick DELTA (a RATE). Wall clock is what is preserved, not
    // tick counts (arcade_enemies.md sec 1.1).
    static constexpr float TICK_RATE  = ARCADE_HZ / ENGINE_HZ;          // 1.1241872
    // engine z per arcade world-z. The tube is 160 world-z deep in the
    // reference and GRID_ELEMENT_LENGTH deep here; proportional, deliberately
    // NOT DOCTRINE.md's camera law of 256 (arcade_enemies.md sec 1.2).
    static constexpr float UNIT_Z     = GRID_ELEMENT_LENGTH / 160.0f;   // 0.15625

    // ======================================================================
    // THE PARAMETERS
    // ======================================================================

    // ---- SPEED -------------------------------------------------------------
    // RE-AUTHORED, and this is the one place this file departs from a
    // recovered number rather than converting one. The reference has exactly
    // ONE player-bullet speed (a global `shotspeed`, 3.5 world-z per arcade
    // tick, halved in its post-completion hard mode), so "the Mirror's shot
    // comes back at exactly the speed your own shot went out at" is trivially
    // true there and needs no rule. This engine has THREE outgoing speeds
    // (PLAYER_SHOT1 0.5, PLAYER_SHOT2 0.75757, AI_DROID_SHOT 0.375 engine
    // z/tick), so the sentence has to be turned into one.
    //
    // The rule taken here is the one that keeps the sentence literally true:
    // THE REFLECTED SHOT INHERITS THE SPEED OF THE BULLET IT TOOK OVER. That
    // is also the reading most consistent with the rest of the take-over,
    // which inherits everything it possibly can (sec 1.1) and re-derives only
    // what the reference re-derives.
    //
    // For scale, the recovered global converts to
    //     3.5 * 0.15625 * 1.1241872 = 0.6148 engine z/tick,
    // i.e. between this engine's laser and 1.23x its primary; the port's own
    // spread straddles it. Recorded so a later reader can see the size of the
    // choice rather than having to re-derive it.
    static constexpr float MirrorSpeedMul = 1.0f;
    // `asr.l #1` -- an exact halving, so the Beast's horn drifts back at half
    // the speed you fired. The slowest thing on screen and the hardest to
    // dodge in a crowd, which is the point of it.
    static constexpr float BeastSpeedMul  = 0.5f;

    // ---- SPIN --------------------------------------------------------------
    // The Mirror's ring never spins (its routine never touches the field); the
    // Beast's horn spins 4 of the reference's 256 angular units per tick =
    // 5.625 deg, scaled by the tick rate. Identical arithmetic to the spiker's
    // roll and the flipper's rotation, which is not a coincidence -- 4/256 per
    // tick is the reference's house rotation rate.
    static constexpr float MirrorSpinDegPerTick = 0.0f;
    static constexpr float BeastSpinDegPerTick  = 5.625f * TICK_RATE;   // 6.32355

    // ---- THE LETHAL TEST ---------------------------------------------------
    // Same lane AND the claw is vulnerable AND |dz| <= 2 world-z. This is the
    // ALREADY-ADJUDICATED enemy-catches-player depth window (DOCTRINE.md
    // conflict 2, user decision 2026-08-20: the arcade reference's 2, never
    // the later port's ~8, which scales this ONE test by the X/Y factor while
    // every other depth constant in its own file uses the Z factor). The
    // recovery confirms it independently in the reflected-shot path
    // (sec 1.2, sec 8 row 1) -- under the later port's number a reflected shot
    // would kill from FOUR TIMES further down the tube.
    //
    // Deliberately the same number as ArcadeFlipper::ParkedGrabDz. If one of
    // them is ever re-tuned the other must move with it: they are one
    // adjudicated constant, not two.
    static constexpr float LethalDz = 2.0f * UNIT_Z;                    // 0.3125

    // ---- EXPIRY ------------------------------------------------------------
    // The reference expires the shot 29 world-z PAST the rim plane, so it
    // visibly flies through and behind the player before vanishing rather than
    // popping out at the claw. (The later port uses 53; cosmetic, and the
    // arcade reference wins -- recovery sec 8 row 8.)
    //
    // NB this is INSIDE the shared shot-removal bound of
    // -GRID_ELEMENT_LENGTH * 0.25 = -6.25, so it always fires first and the
    // shared bound can never be the thing that removes a reflected shot. That
    // matters, because the shared bound would not return the player's slot.
    static constexpr float ExpiryZ = -29.0f * UNIT_Z;                   // -4.53125

    // ---- THE BEAST'S ABSORPTION -------------------------------------------
    // Only KIND_BEAST runs it. It finds an ordinary player bullet in its own
    // lane within the same window the Mirror and the Beast use to notice they
    // have been shot, and DESTROYS IT. The reflected shot is NOT destroyed in
    // return -- the reference discards the helper's return value, so this is a
    // one-way trade and the player cannot shoot a horn out of the air.
    //
    // THE PARTICLE LASER PASSES THROUGH AND IS NOT CONSUMED (recovery sec 1.3,
    // the "particle laser" row): the reference's helper lets its
    // laser-class bullets through the test entirely. So the laser is the one
    // answer to a screen full of horns, which is exactly the kind of thing the
    // powerup ladder is for.
    static constexpr float AbsorbDz = 6.0f * UNIT_Z;                    // 0.9375

    // ======================================================================
    // ENTRY POINTS
    // ======================================================================

    // ---- THE TAKE-OVER -----------------------------------------------------
    // The five writes, in place, on the bullet the Mirror or Beast was just
    // hit by (recovery sec 1.1). NOTHING IS ALLOCATED and nothing else is
    // touched: z, the lane it sits in, its `life`, and its slot in
    // GridElement::shots are all inherited exactly as they were.
    //
    // INLINE ON PURPOSE, and for two reasons that agree:
    //   * it is five stores called from the two hit paths -- exactly the kind
    //     of thing the perf doctrine wants the compiler to see through; and
    //   * HISTORY, no longer load-bearing: it was written when the desktop
    //     CMake list did not yet carry arcade_mirror.cpp, so inlining was
    //     what let arcade_flipper.cpp (the Beast) call it at all. The
    //     registration pass added the TU (t2k_pc/CMakeLists.txt's SOURCES
    //     list carries it today, and t2k_3ds/Makefile has always globbed it),
    //     so the perf reason above now stands alone -- which is enough.
    //
    // `ownerShotId` is the id the bullet had -- the reference's "owner tag",
    // which it inherits untouched and which is what unlink code 3 dispatches
    // on to give the right player his shot back.
    static void takeOver(GameEngine& engine, Shot& shot, Kind kind,
                         int hornVariant = 0) {
        // ---- ATTRACT MODE NEVER SHOWS A SHOT FLYING AT THE PILOT ----------
        // This is the ONE hostile projectile that does not come from
        // init_shot: it re-tasks a LIVE PLAYER BULLET IN PLACE, so the demo
        // guard in engine.cpp's init_shot (`shot_id >= ARCADE_REFLECT_SHOT`)
        // cannot see it. Substituting the family away is not enough either --
        // demoSubstituteEnemy maps ARCADE_MIRROR to ARCADE_FLIPPER, but the
        // BEAST is flipper SUB_BEAST (arcade_flipper.h), not ARCADE_MIRROR, so
        // it survives substitution and kept re-tasking bullets.
        //
        // Consume the bullet instead of converting it: the enemy still
        // absorbed the shot, which is what the demo viewer sees, and no
        // hostile shot exists. `life = 0` is the tree's own reap convention
        // (move_shots collects it; nothing is removed mid-scan).
        //
        // Found by tools/demo_audit.sh only AFTER that harness was fixed to
        // call mathLutInit() and advance its clock -- with zeroed trig tables
        // no enemy ever reached the depth gate, so the audit reported zero
        // hostile shots and PASSED. A gate that cannot reach the situation it
        // audits reports the answer you wanted.
        if (engine.demo_mode) { shot.life = 0; return; }

        const int ownerShotId = shot.id;
        // 1-2. the drawn payload becomes the reflected shape, and the object
        //      is marked drawable. Here that is one id plus the tag below,
        //      which is what the renderer reads to choose ring vs horn.
        shot.id = SHOT_ID;
        // 3. the update routine becomes refsht (Mirror) or refsht2 (Beast) --
        //    here, the Kind bit of the tag.
        setTag(shot, ownerShotId, kind, hornVariant);
        // The two derived parameters, CAPTURED NOW rather than looked up per
        // tick: the reflected shot outlives nothing that could change them,
        // and capturing keeps the per-tick path free of table reads.
        shot.py = speedFor(ownerShotId, kind);
        shot.px = 0.0f;                       // spin starts at the bullet's own
                                              // orientation, i.e. unrotated
        // 4. clear the bullet's slot in the player-bullet table, so the engine
        //    stops treating the object as a player bullet. In this engine the
        //    id IS that classification, so write 1 did it -- but the SHOT
        //    BUDGET is deliberately NOT returned here: `shots_nums[owner]`
        //    stays charged for as long as the reflected shot lives, which is
        //    the reference's "it costs you a shot slot" (sec 1.1) exactly.
        //    releaseOwnerSlot() below is the other half.
        engine.shots_nums[SHOT_ID] += 1;
        // 5. return. Position, lane, depth, life and owner: untouched.
    }

    // ---- THE BULLET SEARCH -------------------------------------------------
    // The reference's NON-DESTRUCTIVE collision helper: same lane, |dz| below
    // the window, first match in slot order. The entire difference between it
    // and the ordinary helper is one flag byte that says "do not kill what you
    // find" -- and that is precisely what leaves the bullet available to be
    // taken over. Both the Mirror and the Beast call it, which is why it lives
    // here and not in either family.
    //
    // WHAT COUNTS AS A BULLET is "everything in the reference's player-bullet
    // table", which is wider than it first looks:
    //   * the claw's ordinary shot and its particle laser -- obviously;
    //   * THE AI DROID'S SHOTS TOO. The reference's droid fires the second
    //     player's bullets off the second player's counter rather than
    //     allocating its own (constants.h says so at AI_DROID_SHOT), so a
    //     droid parked in a Mirror's lane feeds it, and its shots come back at
    //     you. Emergent, faithful, and worth knowing before it is reported as
    //     a bug;
    //   * NOT the powerup capsule (not a bullet), NOT enemy fire, and NOT
    //     another reflected shot -- that last one matters, because SHOT_ID is
    //     numerically below ENEMY_SHOT1 and would otherwise pass the first
    //     test and let two Mirrors bat one shot back and forth forever.
    //
    // INLINE for the same reason takeOver() is -- the perf one: it is called
    // from arcade_flipper.cpp (the Beast) as well as from this family, and is
    // exactly the kind of thing the perf doctrine wants the compiler to see
    // through. (The old link-time reason is stale; see takeOver().)
    //
    // The returned pointer is into GridElement::shots and is valid only until
    // that vector changes. Every caller uses it immediately and none of them
    // pushes a shot in between.
    static Shot* findBullet(GameEngine& engine, int lane, float z, float window) {
        GridElement& elem = engine.grid[lane];
        for (int i = 0; i < elem.num_shots; ++i) {
            Shot& s = elem.shots[i];
            if (s.id >= ENEMY_SHOT1) continue;      // enemy fire
            if (s.id == POWERUP_SHOT) continue;     // the capsule
            if (is(s.id)) continue;                 // already reflected
            // A bullet already spent earlier this tick (a spike ate it, the
            // zapper took it) is not in the table any more as far as the
            // reference is concerned. Cheap, and it stops a dead shot being
            // resurrected as live incoming fire.
            if (s.life <= 0) continue;
            const float d = s.z - z;
            if (d < window && d > -window) return &s;
        }
        return nullptr;
    }

    // ---- THE PER-TICK RAIL -------------------------------------------------
    // The reference's shared tail, plus each kind's own prologue. Returns TRUE
    // iff the shot must now be REMOVED by the caller -- the same removal
    // contract enemy_family.h states for enemies, so the caller must neither
    // touch `shot` nor advance its index afterwards.
    //
    // It performs the owner-slot return itself before returning true (that IS
    // unlink code 3), so the caller's only remaining job is the swap-down
    // removal that decrements shots_nums[SHOT_ID].
    static bool tick(GameEngine& engine, int time, int lane, Shot& shot);

    // ---- UNLINK CODE 3 -----------------------------------------------------
    // "Give the player back his shot." Called by tick() on every exit that
    // removes the shot, and exposed by name so that if a future shared path
    // ever removes a reflected shot some other way it has an obvious, single
    // thing to call. Idempotence is NOT claimed: call it exactly once.
    static void releaseOwnerSlot(GameEngine& engine, const Shot& shot);

    // Speed this bullet would come back at, in engine z per tick, always
    // POSITIVE (the direction is fixed: toward the player). Exposed so a
    // harness can assert the halving without driving a tick, and INLINE for
    // the same two reasons takeOver() is -- it is one table read and a
    // multiply, and takeOver() calls it.
    static float speedFor(int ownerShotId, Kind kind) {
        // The bullet's own outgoing speed, sign discarded. SHOT_DZ is positive
        // for player-owned shots and this is the magnitude either way, so a
        // future owner id with a negative row cannot flip the direction.
        float dz = SHOT_DZ[ownerShotId];
        if (dz < 0.0f) dz = -dz;
        return dz * ((kind == KIND_BEAST) ? BeastSpeedMul : MirrorSpeedMul);
    }

    // ======================================================================
    // THE TAG -- the ONLY place the Shot field mapping is spelled out
    //
    // models.h has no per-shot state union and this wave may not add one, so
    // the three values a reflected shot carries live in fields an ordinary
    // player bullet leaves at zero:
    //
    //     Shot::animation_phase -> the packed tag (kind | horn | owner id)
    //     Shot::px              -> spin, degrees
    //     Shot::py              -> |dz| per tick, captured at take-over
    //
    // px/py are UNREAD BY ANYTHING ELSE IN THE TREE (verified by search: no
    // site reads Shot::px or Shot::py at all), and animation_phase is read
    // only by POWERUP_SHOT's own decay branch, which a reflected shot can
    // never enter. Every access below goes through these four functions; if a
    // union lands later only these bodies change.
    // ======================================================================
    static void setTag(Shot& s, int ownerShotId, Kind kind, int hornVariant) {
        s.animation_phase = (ownerShotId << 2) | ((hornVariant & 1) << 1)
                          | (int)kind;
    }
    static Kind kind       (const Shot& s) { return (Kind)(s.animation_phase & 1); }
    static int  hornVariant(const Shot& s) { return (s.animation_phase >> 1) & 1; }
    static int  ownerShotId(const Shot& s) { return s.animation_phase >> 2; }
    static float& spin(Shot& s)            { return s.px; }
    static float  spin(const Shot& s)      { return s.px; }
    static float& speed(Shot& s)           { return s.py; }
    static float  speed(const Shot& s)     { return s.py; }
};

// ============================================================================
// ArcadeMirror -- the enemy. Object type 36 in the reference, generator id 8.
//
// A slow, spinning, faceted disc that comes down the tube, STOPS 40 world-z
// SHORT OF THE RIM, and sits there. It never fires. It never touches the
// player. Every shot you put into it is knocked back up the web AND HANDED
// BACK TO YOU as a reflected shot -- four times -- and the fifth kills it for a
// random 250 / 500 / 750.
//
// It is a PUZZLE, not a threat: something you must either shoot through five
// times or work around, while it turns your own fire into incoming fire down
// your own lane. Letting it reach the rim or giving it a grab would turn it
// into a flipper (recovery sec 10 item 5).
//
// ---- THE TWO SLOTS NAMED "MIRROR", because the reference's own annotation
//      table is WRONG about this and the recovery had to disprove it --------
//
// The reference has TWO objects called Mirror. Slot 36 is THIS one, the enemy.
// Slot 31 is a two-instruction spinner used as a PLAYER-CARRIED SHIELD in
// two-player versus mode, created only by the versus-mode claw setup, which
// deflects an opposing bullet by negating its velocity. They share only their
// drawn shape. The versus-mode one is out of scope for a single-player port
// and nothing here is derived from it -- but it is where the family's design
// comes from, and it plays the SAME SOUND on a deflection that this enemy
// plays when it is hit, which is a strong hint the two were one idea.
// (recovery sec 2, sec 4.)
//
// ---- SPAWN SCHEDULE, recorded here so this family's reference numbers sit
//      beside its behaviour --------------------------------------------------
//
// THE LEVEL SET IS WIRED: `_arcHasMirror` (enemy_spawns.h) ORs ARCB_MIRROR into
// GRID_LEVELS_SPAWN_ENEMY_ARCADE, and the debut is pinned by the static_assert
// beside it. Its list is exactly the one below.
// THE RELOAD PERIODS ARE NOT REPRODUCED -- this port has no per-type reload
// counter anywhere, so they stay prose: here, in enemy_spawns.h's own comment,
// and in docs/design/arcade_mirror_beast_recovery.md sec 3.7.
// The reference's wave scripts spawn a Mirror on a per-type reload counter --
// LOWER IS DENSER, in reference ticks (x 0.88953 for engine ticks):
//
//     levels 57-63   reload 400      (57 is its debut: a Mirror + super-2 wave)
//     levels 64-65   none
//     levels 66-95   reload 750
//     levels 96-98   reload 350
//     levels 99-100  none
//
// There is NO probability ramp for the Mirror -- density is the reload period
// alone -- and the later port's tables parse to an identical level list and
// identical periods, so there is no divergence to adjudicate. Both ports also
// gate every type index above 6 behind the arcade mode flag, so the Mirror
// cannot appear in the reference's own classic mode at all.
// ============================================================================
struct ArcadeMirror {
    // The ids this family claims (enemy_family.h step 1). The `switch` in
    // enemies.cpp move_enemies is the single dispatch and must agree.
    static constexpr int Ids[] = { ARCADE_MIRROR };

    // ---- MODES -------------------------------------------------------------
    // Only two, and the first is shared in spirit with the flipper: every
    // enemy that enters through the reference's insert-and-approach helper is
    // INERT while it closes the last 300 world-z, drawn as a single depth-
    // shaded pixel with its update routine not run at all.
    enum Mode {
        MODE_ARRIVAL = 0,   // inert, invulnerable, closing from behind the far plane
        MODE_LIVE    = 1,   // descending to the park plane, then holding
    };

    // ======================================================================
    // UNIT CONVERSION -- see the note on ArcadeReflectedShot's copy.
    // ======================================================================
    static constexpr float TICK_RATE = ArcadeReflectedShot::TICK_RATE;   // 1.1241872
    static constexpr float UNIT_Z    = ArcadeReflectedShot::UNIT_Z;      // 0.15625

    // ======================================================================
    // GEOMETRY OF THE TUBE
    // ======================================================================

    // The far plane, where a live enemy begins. Reference webz+80.
    static constexpr float FarPlaneZ = GRID_ELEMENT_LENGTH;              // 25.0
    // The rim, where the claw sits. Reference webz-80.
    static constexpr float RimPlaneZ = 0.0f;

    // ---- ARRIVAL -----------------------------------------------------------
    // Identical to ArcadeFlipper's, because it is the same reference helper:
    // the object is inserted 300 world-z BEHIND the far plane and drifts in at
    // 2 world-z per reference tick, i.e. 150 reference ticks / 133.4 engine
    // ticks / 2.135 s of "distant speck growing closer" before it becomes
    // live.
    //
    // NB the recovery (sec 3.1, sec 7.5) states that the port LACKS this
    // drift. That is stale: arcade_flipper.h's "S-1 ARRIVAL" block --
    // SpawnZ / ArrivalDz / ArrivalTicks -- already implements exactly it,
    // wall-clock converted, and this family reuses the same two numbers
    // rather than inventing a third arrival. See the report. (Cited by NAME:
    // the old ":158-166" pointer was correct when written and was rotted by
    // an insertion into that file in the very same commit.)
    static constexpr float SpawnZ     = FarPlaneZ + 300.0f * UNIT_Z;     // 71.875
    static constexpr float ArrivalDz  = 2.0f * UNIT_Z * TICK_RATE;       // 0.3513085
    // For the harness: (71.875 - 25) / 0.3513085 = 133.4 ticks.
    static constexpr float ArrivalTicks = 133.42f;

    // ---- THE PARK PLANE ----------------------------------------------------
    // Reference webz-40: FORTY world-z short of the rim, and therefore forty
    // short of where a flipper stops. That gap is the whole enemy. It hovers
    // about a quarter of the tube in from the rim, out of reach, as a
    // stationary thing you have to shoot THROUGH to reach anything behind it.
    static constexpr float ParkZ = 40.0f * UNIT_Z;                       // 6.25

    // ---- DESCENT SPEED -----------------------------------------------------
    // The Mirror descends on the FUSEBALL'S speed table, not the flipper's --
    // a 56-entry table indexed by `wave >> 1`, so consecutive wave PAIRS share
    // a value.
    //
    // RE-AUTHORED, and narrowly. The recovery deliberately does not reproduce
    // the table (IP boundary, its sec 9.2) and gives only its RANGE OVER THE
    // MIRROR'S OWN LEVELS: about 0.75 rising to 0.97 world-z per reference
    // tick across levels 57-98. So the ramp below is linear and EXACT AT BOTH
    // RECOVERED ENDS, and is HELD FLAT outside 57-98 rather than extrapolated
    // -- the Mirror does not spawn outside that band, so every extrapolated
    // value would be a number nobody can check being used by nothing.
    //
    // Same discipline, and the same flag to the verifier, as
    // ArcadeFlipper::RailDzWave0/RailDzWaveMax: this is the one place in this
    // file carrying numbers the recovery did not supply. Nothing else here
    // depends on the shape of the interior -- the recovery says so in as many
    // words: only the park plane and the knockback multiplier matter.
    static constexpr int   DescentTableSize = 56;    // indexed by wave >> 1
    static constexpr int   DescentFirstWave = 57;    // the Mirror's debut level
    static constexpr int   DescentLastWave  = 98;    // its last
    static constexpr float DescentWorldFirst = 0.75f;  // world-z per reference tick
    static constexpr float DescentWorldLast  = 0.97f;

    // ---- THE KNOCKBACK -----------------------------------------------------
    // A hit shoves the Mirror `velocity * 16` FURTHER FROM THE PLAYER.
    //
    // IT IS A DISTANCE, NOT A RATE, AND THAT IS A TRAP THIS PORT HAS ALREADY
    // PAID FOR ONCE. Sixteen ticks' worth of travel is a fixed LENGTH however
    // fast the host ticks, so it converts with UNIT_Z alone and must NOT carry
    // TICK_RATE -- exactly the distinction ArcadeSpiker::buildAllowance() is
    // written to preserve ("A DISTANCE, so it takes NO rate conversion").
    // Carrying the rate would inflate every shove by 12.4%.
    //
    // 0.75 * 16 * 0.15625 = 1.875 engine z at level 57, rising to 2.425 at 98
    // -- the recovery's "~12 to 15.5 world-unit shove per hit".
    static constexpr int KnockbackTicks = 16;

    // ---- THE HIT COUNTER ---------------------------------------------------
    // FIVE HITS KILL A MIRROR and the first four each hand you back a shot.
    // The reference stores 4 and tests the sign AFTER decrementing, so the
    // sequence is 4,3,2,1,0 (all reflecting) and the sixth decrement
    // underflows into death.
    //
    // The field it stores this in is the one a FLIPPER uses for its mode --
    // deliberate reuse in the reference, reproduced here (see the accessors)
    // so a reader comparing the two families sees the same overlay.
    static constexpr int InitialHits = 4;

    // ---- THE BULLET SEARCH -------------------------------------------------
    // Same lane, |dz| < 6 world-z, AND -- this is the entire difference
    // between the helper the Mirror calls and the ordinary one -- IT DOES NOT
    // KILL THE BULLET IT FINDS. That is what leaves the bullet available to be
    // taken over.
    static constexpr float HitDz = 6.0f * UNIT_Z;                        // 0.9375

    // ---- DEATH -------------------------------------------------------------
    // A RANDOM 250 / 500 / 750, uniform, rolled at the moment of death. Not a
    // fixed score: the reference draws one of three bonus values and converts
    // the corpse in place into a pixel-shatter graphic SHOWING THE AMOUNT --
    // which this engine's init_explosion already does for free, since it
    // floats the awarded number at the projected kill point.
    //
    // (The later port awards the same three values and then simply unlinks --
    // no sound, no graphic. The arcade reference wins: recovery sec 8 row 5.)
    static constexpr int DeathScoreLo   = 250;
    static constexpr int DeathScoreStep = 250;   // 250 / 500 / 750
    static constexpr int DeathScoreN    = 3;

    // ---- SPIN --------------------------------------------------------------
    // One of the reference's 256 angular units per tick -- a quarter of the
    // house rotation rate, so the facets glint slowly rather than tumbling.
    static constexpr float SpinDegPerTick = 1.40625f * TICK_RATE;        // 1.580888

    // ---- PRESENTATION ------------------------------------------------------
    // The reference plays its "pulsar pulse" sample on every non-fatal hit --
    // notably THE SAME SOUND its versus-mode shield plays when it deflects,
    // which is the strongest single piece of evidence that the two Mirrors are
    // one design idea. This port already owns a sample for exactly that
    // event: SfxId::REFLECT, "shot reflect". Presentation, therefore free
    // under DOCTRINE.md, and it needs no new asset.
    static constexpr SfxId HitSfx = SfxId::REFLECT;
    // Death raises the reference's "cleared level" fanfare. This engine's
    // init_explosion already raises BOOM pitched by energy on every enemy
    // death, and a second, louder cue on one enemy type would fight the
    // reward-juice budget ("excess is a response, never wallpaper"). So the
    // fanfare is deliberately NOT reproduced; flagged here so the omission is
    // visibly a decision. See the report.

    // ======================================================================
    // ENTRY POINTS
    // ======================================================================

    // Per-tick update. RETURN CONTRACT (enemy_family.h): true iff this call
    // REMOVED the enemy from `lane` slot `idx`. A Mirror only ever removes
    // ITSELF, and only by dying.
    static bool update(GameEngine& engine, const EnemyCtx& ctx,
                       int lane, int idx, Enemy& enemy);

    // The level generator's Mirror. Spawned at a UNIFORM-RANDOM lane -- the
    // caller picks it, exactly as it does for the flipper (contrast the
    // tanker, which excludes both end lanes, and the spiker, which picks its
    // own). Returns false if the lane was full.
    static bool spawn(GameEngine& engine, int lane);

    // THE VULNERABILITY PREDICATE (arcade_enemies.md sec 3.5), i.e. the
    // `enemy_shootable()` hook.
    //
    // IT IS ALWAYS FALSE, AND THAT IS THE ENEMY. Read the four sites it gates
    // (enemies_shared.h) and the whole Mirror falls out of one answer:
    //
    //   * the two shot sweeps must not damage it -- it has its OWN five-hit
    //     counter and its own knockback, and the shot that hits it is not
    //     consumed but RE-TASKED;
    //   * the SUPERZAPPER must not touch it. The reference's superzapper is
    //     strictly OPT-IN PER UPDATE ROUTINE (the recovery enumerates all ten
    //     sites that test the zapper flag) and the Mirror's routine never opts
    //     in. THE MIRROR IS IMMUNE (recovery sec 3.4, sec 10 item 6) -- while
    //     the Beast, whose routine tests the flag before its mode dispatch,
    //     dies to one zap. This is the sharpest behavioural difference between
    //     the two enemies in this recovery and it is easy to get backwards;
    //   * the TREMOR is this port's own weapon and has no recovered rule. It
    //     is treated as the superzapper is, deliberately: both are area
    //     weapons that would otherwise let the player delete the puzzle
    //     without solving it. Flagged as a judgement call, not a recovery.
    //
    // A Mirror is therefore killable by exactly one thing -- five shots -- and
    // it kills that fifth shot's four predecessors by giving them back to you.
    static bool shootable(const Enemy& enemy) { (void)enemy; return false; }

    // This level's descent step in engine z per ENGINE tick (a RATE).
    // `wave` is engine.current_level (0-based -- do not pass the HUD's
    // number, it would shift the whole ramp by half a table row).
    static float descentDz(int wave);
    // ...and one hit's shove, in engine z (a DISTANCE -- no rate conversion).
    static float knockbackZ(int wave);
    // The death bonus. CONSUMES ONE rand() DRAW, exactly as the reference
    // does at the moment of death.
    static int rollDeathScore();

    // ======================================================================
    // FIELD ACCESSORS -- the ONLY place this family's Enemy overlay is
    // spelled out (same discipline as arcade_flipper.h's block).
    //
    //     Enemy::current_lane   -> mode        (Mode)
    //     Enemy::sidestep_freq  -> hit counter (4 down through 0, then death)
    //     Enemy::anchor_rot     -> spin, degrees (models.h's own contract:
    //                              "degrees already rotated")
    //     everything else       -> UNUSED, left at its constructed value.
    //
    // `current_lane` for the mode matches ArcadeFlipper exactly, on purpose.
    // The reference puts the Mirror's HIT COUNTER in the field a flipper uses
    // for its mode; that overlay cannot be reproduced here because this family
    // needs a mode of its own for the arrival, so the counter moves to the
    // field the flipper uses for its sub-variant. Recorded because a reader
    // holding the recovery open WILL expect the counter to be in the mode
    // field.
    //
    // !! CONSEQUENCE FOR THE RENDERING WAVE, same as the flipper's: none of
    //    these are animation counters. entity_geometry.cpp's generic per-enemy
    //    animation block must never run for an arcade id.
    // ======================================================================
    static int&  mode(Enemy& e)        { return e.current_lane; }
    static int   mode(const Enemy& e)  { return e.current_lane; }
    static int&  hits(Enemy& e)        { return e.sidestep_freq; }
    static int   hits(const Enemy& e)  { return e.sidestep_freq; }
    static float& spin(Enemy& e)       { return e.anchor_rot; }
    static float  spin(const Enemy& e) { return e.anchor_rot; }
};

// ---- Compile-time binding of the adjudicated / load-bearing numbers --------
// Every value here is exactly representable in binary32 (all are n/64ths of
// GRID_ELEMENT_LENGTH), so these are exact equalities and not tolerances. They
// exist so that editing one of a matched pair without the other fails at
// COMPILE time -- the same discipline arcade_spiker.h and constants.h apply to
// their own adjudicated constants.
static_assert(ArcadeReflectedShot::UNIT_Z == 0.15625f,
              "arcade->engine depth scale (GRID_ELEMENT_LENGTH/160)");
static_assert(ArcadeReflectedShot::LethalDz == 0.3125f,
              "reflected-shot lethal window: THE ARCADE REFERENCE'S 2 world-z "
              "-- DOCTRINE.md adjudicated conflict 2, confirmed in this path");
static_assert(ArcadeMirror::HitDz == ArcadeReflectedShot::AbsorbDz
                  && ArcadeMirror::HitDz == 0.9375f,
              "the bullet-notice window and the Beast's absorb window are ONE "
              "recovered constant of 6 world-z (same reference helper), not "
              "two -- arcade_flipper.h's BeastHitDz is the third copy and "
              "carries the matching assert; edit all three or none");
static_assert(ArcadeMirror::ParkZ == GRID_ELEMENT_LENGTH * 0.25f,
              "the park plane is 40 of the tube's 160 world-z above the rim, "
              "i.e. EXACTLY a quarter of the tube -- if this stops holding, "
              "either UNIT_Z or the park plane has been edited alone");
static_assert(ArcadeReflectedShot::ExpiryZ > -GRID_ELEMENT_LENGTH * 0.25f,
              "the reflected shot must expire INSIDE the shared shot-removal "
              "bound, or the shared bound removes it first and the player "
              "never gets his shot slot back");
static_assert(ArcadeMirror::SpawnZ == 71.875f,
              "arrival spawn depth must match ArcadeFlipper::SpawnZ -- one "
              "reference helper, one number");

} // namespace enemyfam
} // namespace ts
