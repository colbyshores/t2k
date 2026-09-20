#pragma once
// ============================================================================
// arcade_tanker.h -- the ARCADE TANKER family: ARCADE_TANKER, ARCADE_FUSE_TANKER,
// ARCADE_PULSAR_TANKER. Spec: docs/design/arcade_enemies.md §2.2 (units §1, shared
// split §3.4/§3.5/§3.6). Shared-path map: docs/design/enemy_pipeline_audit.md.
//
// Per DOCTRINE.md this whole roster is an EXPLICIT EXCEPTION to "MECHANICS ARE
// GATED": the "original" being matched is the arcade reference, not this engine's own
// shipped exe. `fidelity-verifier` / `exe-comparator` must not correct any of
// it back toward the reference source / the shipped PC port.exe.
//
// ---- WHAT IT IS -----------------------------------------------------------
// A CARRIER, NOT A THREAT. It falls down ONE lane in a dead-straight line, can
// never change lane, and CAN NEVER KILL THE PLAYER at any moment of its life.
// What is dangerous is the EVENT of it opening: it splits into two children
// that appear on the two boundary lines of its lane and move APART.
//
// It opens for THREE causes and they are not two: SHOT, ZAPPED, LANDED.
// "Shot and landed are identical -- letting a tanker land scores and releases
// exactly what shooting it does; shooting only buys distance from the rim."
//
// ---- ADJUDICATED DIVERGENCE: NO SPIN --------------------------------------
// The later port plain tanker adds 2.8125 deg/tick to its body angle. THE THE ARCADE REFERENCE'S
// TANKER DOES NOT ROTATE, and arcade_enemies.md's header table adjudicates that
// conflict in the arcade reference's favour ("Flipper-tanker spin | the arcade reference (no spin) |
// the later port adds one; cosmetic"). So NONE of the three variants spin here. The later port
// rate is recorded below as ArcadeSpinDegPerTick and deliberately never read --
// do not "restore" it, and do not delete it either; it is the evidence that
// the omission is a decision.
//
// A consequence worth knowing: the reference's open handler re-snaps the body
// angle to the lane's outward angle precisely BECAUSE the spin had been
// drifting it. With no spin the angle never leaves the lane's outward angle,
// so the re-snap is structurally a no-op here -- which is why this family
// stores no angle at all (see "STATE", below).
//
// ---- STATE: WHERE IT LIVES, AND WHY IT IS NOT A UNION YET -----------------
// arcade_enemies.md §3.3 specifies a named `ArcadeState` union on `Enemy`. models.h
// does not have it yet (Wave A added only the four anchor fields), and this
// wave may not edit shared files. So the family's state is carried in existing
// `Enemy` fields THROUGH NAMED ACCESSORS declared here -- never by open-coding
// a scratch int at a use site. When `ArcadeState` lands, the accessors are the
// only things that move.
//
// The tanker needs exactly ONE bit of state -- which of its two modes it is in
// -- because with no spin there is no angle to carry, and position/lane are
// already `Enemy::z` and the lane vector it lives in.
//
//   Enemy::animation_phase  -> the MODE. 0 == APPROACH, non-zero == DESCEND.
//        Chosen so a ZERO-INITIALISED tanker is a valid APPROACH tanker, which
//        is what makes the family correct under BOTH release paths (see
//        "TWO RELEASE PATHS"). Tested for non-zero rather than == 1 so that
//        _transfer_enemy's `animation_phase = -animation_phase` (audit R3)
//        cannot corrupt it -- the tanker never changes lane, so that is
//        belt-and-braces, but the encoding costs nothing.
//
// The tanker writes NO field on its children. Building a hatchling means
// filling in the FLIPPER's state, and the flipper owns that encoding -- see
// "THE HATCHLING HANDOFF" below.
//
// ---- TWO RELEASE PATHS, AND WHY BOTH WORK ---------------------------------
// 1. `spawn()` below -- the later port arrival: the far-plane dot, the end-lane
//    restriction, the 3-slot reservation, and the enemies_todo[] decrement.
// 2. This engine's existing embryo path (init_embrio -> init_enemy), if the
//    arcade set is wired to reuse it. That builds the tanker at z = 25 (the
//    far plane) with mush_flag = 15 and animation_phase = 0 -- i.e. mode
//    APPROACH sitting exactly on the far plane, which this state machine hands
//    off to DESCEND on its very first tick. Correct, just without the dot.
//    NOTE it does NOT honour the end-lane restriction (it rolls
//    rand() % lane_count), so `openChildren` handles lane 0 / lane n-1
//    explicitly instead of assuming them away.
//
// ---- enemies_todo[] / is_level_clear(): THE HANG, STATED ------------------
// (audit §8 -- the showstopper, and the symptom points nowhere near the cause)
//
//   * `enemies_todo[id]` is the level's RELEASE budget, sized once by
//     init_level from the spawn bitmask. `is_level_clear()` is
//     "all enemies_todo[] == 0 AND no lane holds an enemy or an embryo".
//   * `spawn()` is the family's SINGLE release path and it owns BOTH halves of
//     the accounting: `enemies_todo[id] -= 1` and `enemies_nums[id] += 1`,
//     performed together and only after every refusal test has passed. A
//     release that skipped the decrement would leave enemies_todo[] permanently
//     positive and THE LEVEL WOULD NEVER END.
//   * CHILDREN NEVER TOUCH enemies_todo[]. They are not scheduled population;
//     they are a consequence of a tanker that was already counted. A child that
//     decremented enemies_todo[ARCADE_FLIPPER] would steal two level-scheduled
//     flippers per tanker, and once that counter reached 0 it would go
//     NEGATIVE -- which is_level_clear()'s `> 0` test reads as "clear",
//     MASKING the error rather than reporting it.
//   * Children DO `enemies_nums[childId] += 1`, which is what balances the
//     shared `_remove_enemy`'s `-= 1` (audit R1). Without it the counter goes
//     negative on the child's death, and init_embrio's per-type cap
//     (`embrios_nums + enemies_nums > max_per_type`) then lets the level
//     over-spawn that type for the rest of the wave.
//   * The level stays open while children live because a child is a real enemy
//     in `grid[lane].enemies`, so `num_enemies > 0`. The level ends only after
//     every tanker has been released AND opened AND both children have died.
//   * A released tanker ALWAYS opens: on being shot, on being zapped, or on
//     reaching the rim -- and the rim is reached unconditionally in at most
//     ~285 ticks. There is no path on which it lingers.
//   * THE ONE REMAINING HANG is a child that can never die because its family
//     is not implemented: it would fall to `default:` in move_enemies' switch
//     and sit motionless forever. That is what the *Ready gates below are for,
//     and arcade_tanker.cpp turns a spawn-mask that contradicts them into a BUILD
//     ERROR rather than a play-test discovery.
//
// ---- DEFERRED PAYLOADS -- APPLIED -----------------------------------------
// Both payloads landed in Wave 2. Kept as the record, not as a description of
// the code today: arcade_fuseball.{h,cpp} and arcade_pulsar.{h,cpp} exist,
// FuseballReady / PulsarReady below are both true, and both carriers spawn
// from their recovered debuts (enemy_spawns.h -- fuse tanker level 25, pulsar
// tanker wave 33). The reasoning below is why the gates were built as a
// refusal rather than a placeholder, and it is why flipping them was safe.
//
// AT THE TIME, fuseballs and pulsars were Wave-2 families that DID NOT EXIST.
// The decision was (a) THE FUSE- AND PULSAR-TANKERS ARE NOT SPAWNED AT ALL,
// not (b) they open into flippers as a placeholder. Reasons, in order:
//   1. (b) is a silent lie in play. A pulsar-tanker that hatches flippers is
//      indistinguishable from a plain tanker except by its body, so the player
//      learns the wrong rule and it changes under them when pulsars land.
//   2. (b) never exercises the id-mutation hook (audit R4) that the pulsar
//      hatchling exists to need -- born a flipper, retyped to a pulsar -- so
//      the one shared thing the variant requires stays unbuilt and untested.
//   3. (a) cannot ship wrong: `spawn()` refuses BEFORE any accounting, and the
//      static_asserts in arcade_tanker.cpp fail the build if a spawn-mask row
//      asks for a variant whose payload is missing.
// ============================================================================

#include "enemy_family.h"

namespace ts {
namespace enemyfam {

struct ArcadeTanker {
    // The ids this family claims. The `switch` in enemies.cpp move_enemies is
    // the single dispatch and must agree with this list.
    static constexpr int Ids[] = { ARCADE_TANKER, ARCADE_FUSE_TANKER, ARCADE_PULSAR_TANKER };

    // ======================================================================
    // UNITS -- arcade_enemies.md §1. Every recovered constant below is stored
    // VERBATIM in the reference's own unit and converted here, exactly once,
    // so the recovered number and the engine number can never drift.
    // ======================================================================

    // §1.1 Time. The reference is not vsync-locked: it reprograms the PIT with
    // divisor 0x4256 = 16982, giving 1193181.67 / 16982 = 70.2617 Hz. This
    // engine ticks at 62.5 Hz (TICK_MS = 16). WALL-CLOCK is preserved, not tick
    // counts -- a roster running 12% slow against the claw reads as sluggish.
    static constexpr float ArcadeHz   = 70.2617f;                 // reference logic Hz
    static constexpr float TsHz    = 62.5f;                    // 1000 / TICK_MS
    static constexpr float ArcadeRate = ArcadeHz / TsHz;             // 1.12419, per-tick delta scale

    // §1.2 Depth. The reference tube spans 5120 raw = 160 "world-z" (raw =
    // world x 32) from its far plane to its rim plane; this engine's spans
    // GRID_ELEMENT_LENGTH = 25 from z = 25 (far) to z = 0 (rim). PROPORTIONAL,
    // and deliberately NOT DOCTRINE.md's camera law of "1 port unit = 256 the later port
    // units" -- they map different things (the camera law maps the lane step
    // and the standoff). Keep the two conversions in separate named constants
    // so nobody folds them: a depth constant converts at 204.8 raw per engine
    // unit, a camera constant at 256.
    static constexpr float ArcadeZPerWorld = GRID_ELEMENT_LENGTH / 160.0f;   // 0.15625
    static constexpr float ArcadeZPerRaw   = GRID_ELEMENT_LENGTH / 5120.0f;  // 0.0048828125
    // reference world-z/tick -> engine z/tick, and reference raw/tick -> engine z/tick.
    static constexpr float ZRateWorld = ArcadeZPerWorld * ArcadeRate;   // 0.175655
    static constexpr float ZRateRaw   = ArcadeZPerRaw   * ArcadeRate;   // 0.00548920

    static constexpr float FarPlane = GRID_ELEMENT_LENGTH;   // z = 25, the spawn plane
    static constexpr float RimPlane = 0.0f;                  // z = 0, the claw's plane

    // ======================================================================
    // S0 APPROACH -- the dot (§2.2 "S0 APPROACH")
    // ======================================================================
    // "Placed at the far plane, then pushed a further 300 world-z back."
    static constexpr float SpawnBackWorldZ = 300.0f;                        // reference world-z
    static constexpr float SpawnZ = FarPlane + SpawnBackWorldZ * ArcadeZPerWorld;   // 71.875

    // "advances a fixed 2 world-z/tick, runner forced to no-op". Negative here
    // because this engine's enemies fly toward z = 0.
    static constexpr float ApproachWorldZPerTick = 2.0f;                    // reference world-z/tick
    static constexpr float ApproachDz = -(ApproachWorldZPerTick * ZRateWorld);   // -0.3513085

    // Informational, and the two must agree: the reference is a pel for 149
    // ticks and dispatches the real runner on the 150th. 46.875 / 0.3513085 =
    // 133.43 engine ticks = 2.135 s. (Not an integer, which is why the handoff
    // CLAMPS z to the far plane -- see update().)
    static constexpr int   ArcadeApproachTicks = 150;                          // reference ticks
    static constexpr float ApproachTicks    = 133.43f;                      // engine ticks

    // ======================================================================
    // S1 DESCEND (§2.2 "S1 DESCEND")
    // ======================================================================
    // "Descent speed comes from a table indexed by wave DIV 2 (consecutive
    // wave PAIRS share a value)". Raw units per reference tick, verbatim:
    static constexpr int   DescentBands = 8;
    static constexpr float DescentRawPerTick[DescentBands] = {
        16.0f, 17.0f, 18.0f, 19.0f, 20.0f, 23.0f, 24.0f, 25.0f   // reference raw/tick
    };
    // -> engine z/tick: 0.087827 (waves 0-1, a 284.7-tick / 4.55 s traversal)
    //    rising to     0.137230 (waves 14+, 182 ticks / 2.92 s).
    // UNRECOVERED beyond index 7: the flipper's own table is 56 entries long,
    // so this one almost certainly continues. CLAMPED at the hardest value --
    // a deliberate, recorded resolution, not an assumption that it ends here.
    // (The additive term found alongside it in the source is traditional-mode
    // only and is zero in the arcade reference mode, so there is nothing to add.)
    static float descentDz(int wave);

    // §2.2 S1 step 1. RECORDED, NEVER READ -- see "ADJUDICATED DIVERGENCE"
    // above. 2.8125 deg per reference tick x ArcadeRate = 3.1618 deg/engine tick,
    // one revolution per 113.9 ticks (1.822 s).
    static constexpr float ArcadeSpinDegPerTick = 2.8125f * ArcadeRate;   // 3.1618
    static constexpr bool  Spins = false;                            // the arcade reference: no rotation

    // ======================================================================
    // S2 OPEN / SPLIT and S3 DEATH
    // ======================================================================
    // "award 100 points (doubled in Beastly...)". Beastly is out of scope and
    // its doubling is dropped with it (§3.6). This is passed as `energy` to
    // init_explosion, which is how the arcade roster scores at all: the shared
    // path derives score from `energy`, and would otherwise pay the arcade life
    // of 1 (audit D4). Reached via enemies_shared.h `arcade_kill_score()`, so
    // all three death paths pay this same 100 -- shot (_handle_death), zapped
    // and tremor (move_zapper), and landed (this family's own land()). See the
    // REGISTRATION note at the bottom of this header.
    static constexpr int Score = 100;

    // The reference's own bullet test: exact lane equality, |dz| < 6 world-z,
    // strict less-than. RECORDED FOR REFERENCE ONLY -- this port does not
    // implement it. The shared swept-window exchange (collision.cpp move_shots)
    // is the sanctioned equivalent (§3.5) and it is strictly better: it cannot
    // miss a fast shot between ticks. Keep the number so a verifier can see the
    // two agree in scale (0.9375 vs a shot's per-tick travel).
    static constexpr float ArcadeHitWindowZ = 6.0f * ArcadeZPerWorld;   // 0.9375

    // "A tanker costs 3 budget units (itself plus one per child)" -- the
    // THREE-SLOT RESERVATION. It is not a stored reservation here: it is
    // emergent, because budgetInPlay() charges a LIVE tanker 3 and a live child
    // 1, so opening frees exactly the one unit the parent occupied.
    static constexpr int Cost = 3;
    // §3.4's port rule: release only while `in_play + cost <= ARCADE_MAX_IN_PLAY`.
    // The reference's own gate (`budget_in_play >= noclog`, noclog = 21 in the arcade reference
    // against a budget of 32) keeps at most ~11 units in play; 12 is §3.4's
    // stated port value.
    static constexpr int MaxInPlay = 12;

    // ======================================================================
    // PAYLOAD GATES -- see "DEFERRED PAYLOADS" above.
    //
    // Flipping one of these to true is an acknowledgement that the payload
    // family exists AND exposes the hatch entry point this family calls.
    // While a gate is false the variant refuses to spawn, emits no children,
    // and arcade_tanker.cpp fails the BUILD if a spawn-mask row asks for it.
    // ======================================================================
    static constexpr bool FlipperReady  = true;    // arcade_flipper.{h,cpp} exists
    // Wave 2 landed both payloads. The fuse-tanker opens into a pair of
    // FUSEBALLS (arcade_fuseball.h spawnHatchling, +1 then -1, which is what
    // parks them on the two boundary lines of the tanker's lane and sends them
    // apart). The pulsar-tanker opens into a pair of ordinary FLIPPERS marked
    // MARKER_PULSAR, which become pulsars when their first flip lands -- so its
    // payload really is "flipper, then pulsar", never a pulsar directly, and it
    // needs BOTH gates (payloadReady() already spells that dependency out).
    static constexpr bool FuseballReady = true;
    static constexpr bool PulsarReady   = true;

    // ======================================================================
    // THE HATCHLING HANDOFF (§2.1 "Tanker-hatched flipper")
    //
    // The tanker does NOT hand-build its children. Building one means filling
    // in the FLIPPER's state -- its mode, sub-variant, pause, forced spin,
    // captured flip rate and grab marker -- and that encoding belongs to
    // arcade_flipper.h, which maps them onto `Enemy`'s fields in its own way.
    // A hand-built copy here would be a second, drifting definition of the
    // flipper (this repo's HUD-lives-icon failure, exactly).
    //
    // So openChildren() calls
    //     ArcadeFlipper::spawnHatchling(engine, ctx, tankerLane, z, dir, marker)
    // twice, dir = +1 then dir = -1, and that ONE function performs the whole
    // reference sequence: build an ordinary flipper at the tanker's position,
    // depth and lane; run the immediate flip setup (which is what steps the
    // lane index and hinges the body on the border vertex the two lanes
    // share); THEN the super-promotion roll; THEN the -1 / -2 sentinel.
    //
    // That ORDER is observable and is the reason this is a call and not a
    // struct copy: "the first flip of a tanker-born super runs at the PLAIN
    // rate", because the flip setup captures the rate before the promotion
    // writes the sub-variant. A port that reorders the two ships a visibly
    // faster first flip.
    //
    // A cross-family DIRECT CALL is doctrine-clean: it is a static function in
    // a sibling TU, so there is no vtable, no `Behaviour*` and no function
    // table indexed in a loop. It is also not a per-frame path -- it runs
    // twice per tanker, once.
    //
    // What the tanker still owns is WHICH marker: MARKER_PULSAR for a
    // pulsar-tanker (the child becomes a real pulsar when its first flip
    // lands -- the audit's R4 id mutation), MARKER_HATCHLING for everything
    // else.
    // ======================================================================

    // ======================================================================
    // MODE
    // ======================================================================
    enum Mode { MODE_APPROACH = 0, MODE_DESCEND = 1 };
    static int  mode(const Enemy& e) {
        return (e.animation_phase != 0) ? MODE_DESCEND : MODE_APPROACH;
    }
    static void setMode(Enemy& e, int m) {
        e.animation_phase = (m == MODE_DESCEND) ? 1 : 0;
    }

    // ======================================================================
    // THE FAMILY'S ENTRY POINTS
    // ======================================================================

    // THE SHOOTABLE PREDICATE (audit §2/C9, §9/Z4). The approach dot has "no
    // collision test of any kind -- bullets and the superzapper pass straight
    // through". WIRED: the shared `enemy_shootable()` (enemies_shared.h) routes
    // all three tanker ids here, and it is called from all four damage sites --
    // collision.cpp's shot sweep, enemies.cpp's enemy-side sweep, and the
    // tremor and zapper searches in weapons.cpp via `enemy_zappable()`, which
    // delegates for every id but the fuseball. So the dot is untouchable by
    // every weapon; `die()` still refuses to split an APPROACH tanker as a
    // belt-and-braces guard rather than releasing two children 47 units behind
    // the far plane.
    static bool shootable(const Enemy& e) { return mode(e) != MODE_APPROACH; }

    // The §2.2 spawn lane rule: `lane = 1 + rand(lane_count - 2)`, where the
    // reference RNG helper returns 0 .. n-1. So lanes 1 .. n-2 -- NEVER the
    // first or last, on ANY web, open or closed (contrast the flipper, which
    // uses the full range). That restriction is LOAD-BEARING, not cosmetic: it
    // is exactly what makes both of `openChildren`'s end-lane fallthroughs
    // unreachable and guarantees the two children land in DISTINCT ADJACENT
    // lanes. Returns -1 when the web has no legal lane (lane_count < 3).
    static int pickSpawnLane(const GameEngine& engine);

    // THE ONE RELEASE PATH. Performs every gate, then constructs the arrival
    // dot and does BOTH halves of the population accounting exactly once.
    // Returns false (having changed nothing at all) when it refuses.
    static bool spawn(GameEngine& engine, int id, int lane);

    // Returns true iff the enemy was REMOVED from its lane (enemy_family.h).
    // The tanker removes itself exactly once, on the tick it reaches the rim
    // plane and opens -- a split that replaces the parent counts as removal.
    static bool update(GameEngine& engine, const EnemyCtx& ctx,
                       int lane, int idx, Enemy& enemy);

    // THE CUSTOM CORPSE. Called by the two shared paths that kill an enemy
    // without going through update(): _handle_death (shot) and the superzapper
    // (weapons.cpp). Releases the children and NOTHING ELSE -- it does not
    // remove the parent, does not score, does not touch enemies_todo[]; the
    // caller owns all three, exactly as the container hatch does.
    //
    // NO `zapped` FLAG, deliberately: the only two things the reference gates
    // on it are the capsule suppression -- which this port already gets for
    // free because the zapper path never calls note_powerup_kill (audit Z2) --
    // and a ring-instead-of-sphere corpse, whose geometry does not exist. An
    // unread bool would be a dead knob (DOCTRINE.md). Add it with the sphere.
    static void die(GameEngine& engine, const EnemyCtx& ctx,
                    int lane, int idx, const Enemy& enemy);

    // THE SPLIT itself, shared by both open paths. Returns how many children
    // were actually created; 2 in every reachable case. Exposed because it is
    // the single thing worth measuring about this family.
    static int openChildren(GameEngine& engine, const EnemyCtx& ctx,
                            int lane, const Enemy& parent);

    // The §3.4 population gate, in budget units. Emergent from enemies_nums[]
    // rather than a stored counter, so it can never disagree with the pool: a
    // live tanker is charged `Cost` (3), every other live arcade enemy 1.
    static int budgetInPlay(const GameEngine& engine);
    static int arcadeCost(int id);

    // Whether this variant's payload family exists yet.
    static bool payloadReady(int id);
};

} // namespace enemyfam
} // namespace ts

// ============================================================================
// REGISTRATION -- APPLIED (both deferred payloads have landed; see
// FuseballReady / PulsarReady above). Kept as the record, not as work
// outstanding.
//
// REGISTRATION -- what the PARENT must apply in the shared files. Listed here
// so the requirement travels with the family instead of only in a report.
// Every one of these is a shared edit this wave is forbidden to make.
//
//  1. game/enemies.cpp move_enemies -- the dispatch:
//         case ARCADE_TANKER: case ARCADE_FUSE_TANKER: case ARCADE_PULSAR_TANKER:
//             deleted = enemyfam::ArcadeTanker::update(engine, ctx, v, v2, enemy);
//             break;
//     WITHOUT THIS the tanker falls to `default:` and never moves.
//
//  2. game/enemies.cpp _handle_death -- the custom corpse, beside the
//     container hatch and BEFORE _remove_enemy (it pushes only into lane+-1,
//     so `enemy` and `idx` stay valid across it):
//         if (eid >= ARCADE_TANKER && eid <= ARCADE_PULSAR_TANKER) {
//             const enemyfam::EnemyCtx dctx{ time, engine.lane_count,
//                                            engine.grid_level_go_round };
//             enemyfam::ArcadeTanker::die(engine, dctx, lane, idx, enemy);
//         }
//     WITHOUT THIS a SHOT tanker dies without splitting.
//
//  3. game/weapons.cpp move_zapper, immediately before the inline removal
//     (the `enemy.life - ZAPPER_STRENGTH <= 0` arm):
//         if (enemy.id >= ARCADE_TANKER && enemy.id <= ARCADE_PULSAR_TANKER) {
//             const enemyfam::EnemyCtx zctx{ time, engine.lane_count,
//                                            engine.grid_level_go_round };
//             enemyfam::ArcadeTanker::die(engine, zctx, minv, minv2, enemy);
//         }
//     WITHOUT THIS a ZAPPED tanker dies without splitting. Must precede the
//     removal, or `enemy` is stale.
//
//  4. game/constants.h ENEMY_DESC -- the three tanker rows' `dz` should become
//     0.0f. This family owns its own level-scaled z step, and audit §5/M1 is
//     explicit: an arcade family that steps its own z and ALSO falls through
//     _apply_movement MOVES TWICE PER TICK. update() pre-compensates, so it is
//     correct either way -- MEASURED: the spawn-to-rim tick count is identical
//     on all 100 waves with the row at 0.0f and at the placeholder. What the
//     placeholder DOES cost is measured too: a SHOT tanker's children are born
//     0.087827 deeper than the z the bullet found, because _apply_movement
//     still steps the corpse between update() and _handle_death -- which is
//     exactly the reference's "on a hit, JUMP TO OPEN" rule, broken by one
//     step. Zero is the audit's own sanctioned answer and it makes the
//     pre-compensation a no-op instead of a correction.
//
//  5. game/enemies.cpp _handle_death -- score-from-life (audit §6/D4).
//     APPLIED: `ex_energy` routes the arcade ids through `arcade_kill_score()`
//     (enemies_shared.h), so a shot tanker is paid ArcadeTanker::Score's 100,
//     exactly as update()'s landing path already paid it. Without it a life-1
//     arcade kill would have scored ONE POINT and the two open paths would
//     score differently.
//
//  6. game/enemies_shared -- the shootable predicate (audit §2/C9, §9/Z4),
//     called from collision.cpp's shot sweep, enemies.cpp's enemy-side sweep,
//     the tremor's kill zone and the zapper's target search:
//         inline bool enemy_shootable(const Enemy& e) {
//             return !isArcadeEnemy(e.id) || arcade_shootable(e);
//         }
//     with ArcadeTanker::shootable for these three ids. WITHOUT IT the approach
//     dot is shootable, zappable and tremor-killable for 133 ticks.
//
//  7. game/enemies.cpp:159 -- the enemy-vs-player block is PER-SET (audit
//     §4/P2). A tanker CAN NEVER KILL THE PLAYER at any moment of its life.
//     Registration item 4 (dz = 0) degenerates the depth window to exact
//     float equality and makes this nearly unreachable, but "nearly" is not
//     the rule; branch the block.
//
//  8. rendering/entity_geometry.cpp getEnemyGeometry (audit §10/G1) -- an id
//     with no geometry row returns all-null and `buildEnemies` continues, so
//     the tanker is FULLY SIMULATED AND COMPLETELY INVISIBLE on both backends
//     and in both builds. This is exactly how SP_ZAPPER1 shipped invisible for
//     months. Three bodies are needed (§2.2 "Variant presentation"): one vector
//     shape for the plain tanker, five fuseball copies 72 deg apart for the
//     fuse-tanker, two pulsar shapes 180 deg apart for the pulsar-tanker. Only
//     the plain tanker's stored shape id is ever used in the reference; the
//     other two overwrite theirs every frame.
//
//  9. game/enemy_spawns.h -- the arcade spawn table. Bit `i` == EnemyId `i`,
//     so the plain tanker is bit ARCADE_TANKER (14 + 3 = 17), i.e. `1 << 17`.
//     THE FUSE- AND PULSAR-TANKER BITS MUST STAY CLEAR until their payloads
//     exist; arcade_tanker.cpp static_asserts exactly that, so a mistake here is
//     a compile error rather than a level that never ends.
//
// 10. CMakeLists.txt SOURCES -- add `src/game/enemies/arcade_tanker.cpp` beside
//     reflector.cpp. Makefile.3ds picks it up by wildcard already (its globs
//     are three directory levels deep), so the 3DS side needs no edit.
// ============================================================================
