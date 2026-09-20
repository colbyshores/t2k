// ============================================================================
// arcade_tanker.cpp -- ARCADE_TANKER / ARCADE_FUSE_TANKER / ARCADE_PULSAR_TANKER.
// See arcade_tanker.h for the contract, the state encoding, the enemies_todo[]
// argument and the full registration list. Spec: docs/design/arcade_enemies.md
// §2.2; shared-path hazards: docs/design/enemy_pipeline_audit.md.
// ============================================================================

#include "arcade_tanker.h"

#include "arcade_flipper.h"          // the hatch entry point -- see the handoff note
#include "arcade_fuseball.h"         // ...and the fuse-tanker's own payload
#include "../enemy_spawns.h"
#include "../web_geometry.h"

#include <cstdlib>

namespace ts {
namespace enemyfam {

// ============================================================================
// THE LEVEL-HANG GUARD, at COMPILE time.
//
// init_level sizes enemies_todo[] straight from the spawn bitmask. If a level
// asks for a variant whose payload family does not exist, `spawn()` refuses
// forever, enemies_todo[id] stays positive forever, is_level_clear() never
// fires and THE LEVEL HANGS WITH NOTHING ON SCREEN -- the audit's §8, and its
// symptom points nowhere near its cause. Make it a build error instead.
//
// THESE NOW DO REAL WORK, AS THE STANDING GUARD FOR THE REVERSE EDIT. All
// three gates are true (arcade_tanker.h FlipperReady / FuseballReady /
// PulsarReady) and all three bits are set on real rows -- the plain tanker on
// most rows, ARCB_FUSE_TANKER on 34 and ARCB_PULSAR_TANKER on 41 (the
// _arcHas* predicates in enemy_spawns.h). So each assert passes on its FIRST
// disjunct, and what it catches is a gate cleared while its bit is still set.
// NB the two DEFERRED messages below are stale in that light -- they describe
// the state the gates were written for, not today's.
// ============================================================================
static constexpr bool arcadeMaskAsksFor(int id) {
    for (int i = 0; i < SPAWN_TABLE_LEVELS; ++i) {
        if (GRID_LEVELS_SPAWN_ENEMY_ARCADE[i] & (1 << id)) return true;
    }
    return false;
}
static_assert(ArcadeTanker::FlipperReady || !arcadeMaskAsksFor(ARCADE_TANKER),
              "A level spawns ARCADE_TANKER but arcade_flipper does not exist: its "
              "children would never die and the level would never end. Flip "
              "ArcadeTanker::FlipperReady when the flipper family lands.");
static_assert(ArcadeTanker::FuseballReady || !arcadeMaskAsksFor(ARCADE_FUSE_TANKER),
              "A level spawns ARCADE_FUSE_TANKER but the fuseball family is "
              "DEFERRED (arcade_enemies.md §2.6). Clear the bit or ship fuseballs.");
static_assert(ArcadeTanker::PulsarReady || !arcadeMaskAsksFor(ARCADE_PULSAR_TANKER),
              "A level spawns ARCADE_PULSAR_TANKER but the pulsar family is "
              "DEFERRED (arcade_enemies.md §2.6). Clear the bit or ship pulsars.");

// The unit chain, pinned. These are the numbers docs/design/arcade_enemies.md
// §1.4 publishes; if an edit above moves one, the build says so rather than
// the roster feeling wrong at a uniform 12%.
static constexpr bool nearly(float a, float b, float eps) {
    return (a - b) < eps && (b - a) < eps;
}
static_assert(nearly(ArcadeTanker::ArcadeRate, 1.12419f, 1e-5f), "tick conversion");
static_assert(nearly(ArcadeTanker::SpawnZ, 71.875f, 1e-4f), "spawn depth");
static_assert(nearly(ArcadeTanker::ApproachDz, -0.35131f, 1e-5f), "approach rate");
static_assert(nearly(ArcadeTanker::ArcadeHitWindowZ, 0.9375f, 1e-6f), "hit window");
static_assert(nearly(ArcadeTanker::ArcadeSpinDegPerTick, 3.1618f, 1e-4f), "spin rate");

// The two approach numbers must agree, so neither can be "corrected" alone:
// 150 reference ticks x ARCADE_TICKS (62.5/70.2617 = 0.88953) is the engine tick
// count, and so is the distance divided by the rate.
static_assert(nearly(ArcadeTanker::ArcadeApproachTicks * (ArcadeTanker::TsHz / ArcadeTanker::ArcadeHz),
                     ArcadeTanker::ApproachTicks, 0.05f), "approach duration");
static_assert(nearly((ArcadeTanker::SpawnZ - ArcadeTanker::FarPlane) / -ArcadeTanker::ApproachDz,
                     ArcadeTanker::ApproachTicks, 0.05f), "approach duration");

// THE ADJUDICATION, as a compile-time statement rather than a comment. The
// the later port plain tanker spins its body angle; the THE ARCADE REFERENCE'S TANKER DOES NOT, and
// arcade_enemies.md's header table settles that in the arcade reference's favour. Because
// nothing spins, this family stores no body angle at all -- so flipping
// `Spins` to true would silently do NOTHING rather than start a rotation.
// Stop the build here instead and point at the header's note.
static_assert(!ArcadeTanker::Spins,
              "The arcade reference's tanker does not rotate (arcade_enemies.md header table, "
              "'Flipper-tanker spin | the arcade reference (no spin)'). This family stores no "
              "body angle, so setting Spins would change nothing -- add the angle "
              "field and the open-time re-snap first, and re-read the adjudication.");

// ============================================================================
// Speeds
// ============================================================================

float ArcadeTanker::descentDz(int wave) {
    // "indexed by wave DIV 2 (consecutive wave PAIRS share a value)".
    int i = wave / 2;
    if (i < 0) i = 0;
    if (i >= DescentBands) i = DescentBands - 1;   // UNRECOVERED past 7; clamp
    return -(DescentRawPerTick[i] * ZRateRaw);
}

// ============================================================================
// Population budget (§3.4). No stored counter: a stored one could disagree with
// the pool after a death, a level change or a lane-full refusal, and the whole
// point of the reservation is that it cannot.
// ============================================================================

int ArcadeTanker::arcadeCost(int id) {
    if (id == ARCADE_TANKER || id == ARCADE_FUSE_TANKER || id == ARCADE_PULSAR_TANKER)
        return Cost;                     // 3: itself plus one per child
    if (isArcadeEnemy(id)) return 1;
    return 0;                            // this engine's own roster is not budgeted
}

int ArcadeTanker::budgetInPlay(const GameEngine& engine) {
    int n = 0;
    for (int id = ARCADE_ID_BASE; id < ENEMIES_NUM_IDS; ++id)
        n += engine.enemies_nums[id] * arcadeCost(id);
    return n;
}

// ============================================================================
// Payloads
// ============================================================================

bool ArcadeTanker::payloadReady(int id) {
    switch (id) {
    case ARCADE_TANKER:        return FlipperReady;
    case ARCADE_FUSE_TANKER:   return FuseballReady;
    // Needs BOTH: the child is constructed as a flipper and retyped later.
    case ARCADE_PULSAR_TANKER: return PulsarReady && FlipperReady;
    default:                return false;
    }
}

// ============================================================================
// Spawn -- S0's arrival dot (§2.2 "Spawn", "S0 APPROACH")
// ============================================================================

int ArcadeTanker::pickSpawnLane(const GameEngine& engine) {
    const int n = engine.lane_count;
    // Fewer than three lanes has no interior lane at all, and lane +-1 would
    // then resolve back onto the parent's own lane -- which would push a child
    // into the vector openChildren's caller is holding a reference into.
    if (n < 3) return -1;
    // lane = 1 + rand(n - 2), reference RNG helper contract 0 .. n-1.
    return 1 + (rand() % (n - 2));
}

bool ArcadeTanker::spawn(GameEngine& engine, int id, int lane) {
    if (id != ARCADE_TANKER && id != ARCADE_FUSE_TANKER && id != ARCADE_PULSAR_TANKER)
        return false;
    // DEFERRED PAYLOADS. Refuse BEFORE any accounting: a refusal that had
    // already decremented enemies_todo[] would silently shorten the level, and
    // one that had already incremented enemies_nums[] would strand a phantom.
    if (!payloadReady(id)) return false;

    // The level's release budget. Zero means this level has had all the tankers
    // it was sized for.
    if (engine.enemies_todo[id] <= 0) return false;

    if (engine.lane_count < 3) return false;
    if (lane < 1 || lane > engine.lane_count - 2) return false;   // §2.2 end-lane rule

    // The 3-slot reservation (§3.4). `Cost` covers the tanker AND the two
    // children it will certainly release, so the gate is checked once, here,
    // and opening never has to ask again.
    if (budgetInPlay(engine) + Cost > MaxInPlay) return false;

    GridElement& el = engine.grid[lane];
    if (el.num_enemies >= MAX_ENEMIES) return false;

    Enemy t;
    t.id   = id;
    t.z    = SpawnZ;                    // 71.875: the far plane plus 300 world-z
    t.life = ENEMY_LIFE[id];            // 1 -- one hit kills, the laser pierces (§3.5)
    // THE mush_flag TRAP (audit §3), and it is the highest-value finding in
    // that document. Bit 1 is NOT a mushroom bit at four shared sites: clear,
    // and an upgraded player shot damages this enemy only 1 roll in 11 (it
    // reads as a bullet sponge) and the renderer applies this engine's
    // breathing wobble + 2.25 z-stretch, which §5.3 forbids for arcade
    // enemies. 15 is exactly what GameEngine::init_enemy writes, so no shared
    // path can tell a hand-built tanker from a constructor-built one.
    t.mush_flag = 15;
    t.anchor = ANCHOR_LANE_MID;         // "snapped ONCE to the lane midpoint"
    t.pivot_side = -1;
    t.anchor_rot = 0.0f;
    t.cross_t = 0.0f;
    t.current_lane = lane;
    t.animation_vec = 1;
    t.max_animation = 100;              // unused by this family; kept sane for
    t.oo_max_animation = 0.01f;         // the shared render path's `aa`
    t.sidestep_freq = 0;
    t.shoot_freq = 0;                   // unused by this family (ArcadeFlipper maps
                                        // its pause counter here; a tanker has none)
    setMode(t, MODE_APPROACH);          // == animation_phase 0

    el.enemies.push_back(t);
    el.num_enemies += 1;

    // THE RELEASE ACCOUNTING, both halves, together, exactly once per tanker.
    // enemies_nums balances the shared _remove_enemy; enemies_todo is what
    // is_level_clear() waits on. See arcade_tanker.h's enemies_todo argument.
    engine.enemies_nums[id] += 1;
    engine.enemies_todo[id] -= 1;
    return true;
}

// ============================================================================
// Removal -- a byte-for-byte re-statement of enemies.cpp's `_remove_enemy`,
// which is `static` there and so cannot be called from another TU. If that one
// ever changes, this must too.
// ============================================================================

static void removeParent(GameEngine& engine, int lane, int idx) {
    GridElement& el = engine.grid[lane];
    engine.enemies_nums[el.enemies[idx].id] -= 1;
    el.num_enemies -= 1;
    if (idx < el.num_enemies) el.enemies[idx] = el.enemies[el.num_enemies];
    if (!el.enemies.empty()) el.enemies.pop_back();
}

// ============================================================================
// THE SPLIT (§2.2 "S2 OPEN / SPLIT", "S2a variant 0")
//
//   "Built as ordinary flippers copying the tanker's position, depth, lane and
//    angle. The first is moved to the lane's SECOND border point, lane
//    incremented, target set to the new lane's outward angle, mode flipping,
//    rotation +speed; the second mirrors to the FIRST border point, lane
//    decremented, rotation -speed. So the pair appears on the two boundary
//    lines of the tanker's lane and flips outward in opposite directions,
//    never overlapping."
//
// So the geometry is fully determined by the parent's lane and nothing else:
//   dir +1 -> destination lane+1, hinge = parent lane's RIGHT border vertex,
//             which is the DESTINATION lane's LEFT vertex  -> pivot_side -1
//   dir -1 -> destination lane-1, hinge = parent lane's LEFT border vertex,
//             which is the DESTINATION lane's RIGHT vertex -> pivot_side +1
// and the two children therefore start one full lane apart, on the two lines
// that bound the tanker, sweeping in opposite senses. THAT is "moving apart":
// it is not a velocity, it is the two hinges and the two signs.
//
// All of that is ArcadeFlipper::spawnHatchling's job, not this file's -- see
// arcade_tanker.h "THE HATCHLING HANDOFF" for why the tanker calls instead of
// hand-building. What this function owns is: whether the variant may open at
// all, which marker the pair carries, and the +1-then-(-1) order.
// ============================================================================

int ArcadeTanker::openChildren(GameEngine& engine, const EnemyCtx& ctx,
                            int lane, const Enemy& parent) {
    const int id = parent.id;
    if (!payloadReady(id)) return 0;         // DEFERRED payload: never opens
    if (ctx.lane_count < 3) return 0;

    // "re-snap position to the lane midpoint and the body angle back to the
    // lane's outward angle (the SAME field the spin increments, which is why
    // opening cancels the spin exactly rather than approximately)". With the
    // the arcade reference's no-spin tanker the angle never left the lane's outward angle
    // and the position is ANCHOR_LANE_MID throughout, so this is structurally
    // a no-op here. Stated rather than silently omitted, because it is the
    // whole reason the reference's angle field is safe to reuse.

    // A pulsar-tanker marks its children -2 so the shared flipper stopped-
    // handler converts them into real pulsars when the first flip lands; every
    // other variant marks -1 ("cannot grab; drop into RAIL when this first flip
    // lands"). No super-promotion roll happens here -- that is inside
    // spawnHatchling, deliberately AFTER the flip setup.
    const int mark = (id == ARCADE_PULSAR_TANKER) ? ArcadeFlipper::MARKER_PULSAR
                                               : ArcadeFlipper::MARKER_HATCHLING;

    const float z = parent.z;
    int made = 0;

    // ---- THE FUSE-TANKER OPENS INTO FUSEBALLS, NOT FLIPPERS ---------------
    // The fuse-tanker differs from the plain tanker by EXACTLY ONE FIELD --
    // which contents routine it opens into -- and this is that field. Its
    // children are not flippers with a marker: they are fuseballs, parked on
    // the two BOUNDARY LINES of the tanker's lane and crossing APART, which is
    // the same silhouette as the flipper hatch by a completely different
    // mechanism. Everything else (the far-plane spawn, the end-lane
    // restriction, the 3-slot reservation, the straight descent, "shot and
    // landed are identical") is the shared carrier this file already provides.
    //
    // +1 FIRST, matching the reference's own order. It matters only because it
    // fixes the order of any rand() draws the two children make.
    if (id == ARCADE_FUSE_TANKER) {
        if (ArcadeFuseball::spawnHatchling(engine, ctx, lane, z, +1)) ++made;
        if (ArcadeFuseball::spawnHatchling(engine, ctx, lane, z, -1)) ++made;
        return made;
    }

    // "one given an immediate LEFT flip, one RIGHT". The +1 child first,
    // matching the reference's own order -- which matters because each child's
    // promotion roll consumes the shared rand() stream.
    if (ArcadeFlipper::spawnHatchling(engine, ctx, lane, z, +1, mark)) ++made;
    if (ArcadeFlipper::spawnHatchling(engine, ctx, lane, z, -1, mark)) ++made;
    return made;
}

// ============================================================================
// The corpse, for the two shared kill paths (shot, zapper). See arcade_tanker.h.
// ============================================================================

void ArcadeTanker::die(GameEngine& engine, const EnemyCtx& ctx,
                    int lane, int idx, const Enemy& enemy) {
    (void)idx;
    // The approach dot has "no collision test of any kind -- bullets and the
    // superzapper pass straight through", and that is now enforced upstream:
    // enemies_shared.h's `enemy_shootable()` dispatches these three ids to
    // ArcadeTanker::shootable (registration item 6, applied), so neither path
    // that reaches die() -- _handle_death, gated by the two shot sweeps, and
    // the zapper's lethal arm, gated by `enemy_zappable()` -- can deliver an
    // APPROACH dot here any more. The guard STAYS as the backstop: releasing
    // two children at z = 71.875 would put them 47 units behind the far plane,
    // where the flipper's own rail state machine has no meaning. Dying without
    // splitting is the safe, visible fallback -- not a rule.
    if (mode(enemy) == MODE_APPROACH) return;
    openChildren(engine, ctx, lane, enemy);
}

// ============================================================================
// The state machine
// ============================================================================

// The shared _apply_movement (enemies.cpp) adds ENEMY_DZ[id] to z AFTER
// update() returns, so a family that owns its own step must pre-subtract it or
// it MOVES TWICE PER TICK (audit §5/M1). Reflector::update uses exactly this
// technique. With the recommended descriptor row (dz = 0, registration item 4)
// this is `e.z = want` and the shared clamp at `z = -ENEMY_DZ[id]` can never
// fire. With the placeholder row it is a real correction and the descent is
// still tick-for-tick identical (measured on all 100 waves) -- but the SHOT
// path is not: _apply_movement steps the corpse once between update() and
// _handle_death, so the children are born 0.0878 deeper than the bullet found
// the tanker. Nothing here can prevent that from inside the family.
static inline void setZ(Enemy& e, float want) {
    e.z = want - ENEMY_DZ[e.id];
}

// Reaching the rim: clamp exactly, pay the economy, split, remove. Landing and
// being shot MUST be identical (§2.2), so this performs precisely what
// _handle_death performs, in the same order -- explosion (which is where score,
// the multiplier bump, the bonus shed and the floating text all live), then
// note_powerup_kill, then the children, then removal.
static bool land(GameEngine& engine, const EnemyCtx& ctx,
                 int lane, int idx, Enemy& enemy) {
    // Snapshot before anything can touch the pool, exactly as _handle_death
    // does. (openChildren only ever pushes into lane+-1, never into `lane`, and
    // init_level reserves every lane vector to MAX_ENEMIES so no push can
    // reallocate -- but the snapshot costs nothing and removes the class.)
    Enemy self = enemy;
    self.z  = ArcadeTanker::RimPlane;
    enemy.z = ArcadeTanker::RimPlane;   // "clamp exactly to the rim plane"

    engine.init_explosion(ctx.time, ArcadeTanker::Score, lane, ArcadeTanker::RimPlane,
                          EXPLOSION_ENEMY, self.id);
    if (engine.note_powerup_kill()) {
        engine.init_shot(lane, ArcadeTanker::RimPlane, POWERUP_SHOT);
        engine.show_powerup_text(ctx.time, -1);   // "collect powerup"
    }
    ArcadeTanker::openChildren(engine, ctx, lane, self);

    removeParent(engine, lane, idx);
    return true;   // REMOVED -- the caller must not touch `enemy` or advance idx
}

bool ArcadeTanker::update(GameEngine& engine, const EnemyCtx& ctx,
                       int lane, int idx, Enemy& enemy) {
    // Already killed THIS tick by move_shots (slot 322, which runs before
    // move_enemies at 324): take no further step. The reference's DESCEND step
    // 2 is "run the shared bullet/zapper test; ON A HIT, JUMP TO OPEN" -- it
    // does not descend again first. Returning here lets _handle_death open the
    // tanker at exactly the z the bullet found it at, which is what makes
    // "shooting only buys distance from the rim" literally true.
    if (enemy.life <= 0) return false;

    // ---- S0 APPROACH: the dot -------------------------------------------
    // Inert, invulnerable, no fire, no rotation, no collision test of any
    // kind. It still occupies a lane, so it holds off is_level_clear() exactly
    // as the reference's depth scan holds off the level-complete zoom.
    if (mode(enemy) == MODE_APPROACH) {
        const float nz = enemy.z + ApproachDz;
        if (nz > FarPlane) {
            setZ(enemy, nz);
            return false;
        }
        // "on the 150th the depth lands exactly on the far plane, the draw
        // index un-negates, and the real runner dispatches THAT SAME TICK."
        // The engine's tick count is 133.43, not an integer, so the exact
        // landing has to be a clamp -- a port decision forced by the 70.26 ->
        // 62.5 Hz conversion, worth at most 0.04 z.
        enemy.z = FarPlane;
        setMode(enemy, MODE_DESCEND);
        // fall through -- the same tick runs DESCEND
    }

    // ---- S1 DESCEND: the only live state --------------------------------
    // Per tick, in this order (§2.2):
    //  1. spin -- ADJUDICATED AWAY (the arcade reference's tanker does not rotate).
    //  2. the shared bullet/zapper test -- it lives OUTSIDE this function
    //     (collision.cpp move_shots runs at slot 322, before move_enemies at
    //     324; weapons.cpp move_zapper at 319) and reaches OPEN through die().
    //  3. subtract the per-level descent speed.
    //  4. still past the rim -> return; otherwise clamp exactly to the rim
    //     plane and fall through into OPEN.
    //
    // x/y are never recomputed: a dead-straight line down one lane, and it
    // CANNOT CHANGE LANE.
    const float nz = enemy.z + descentDz(engine.current_level);
    if (nz > RimPlane) {
        setZ(enemy, nz);
        return false;
    }
    return land(engine, ctx, lane, idx, enemy);
}

} // namespace enemyfam
} // namespace ts
