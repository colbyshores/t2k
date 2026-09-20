// ============================================================================
// arcade_pulsar.cpp -- ARCADE_PULSAR and the ARCADE_PULSAR_SPARK it becomes at
// the rim. See arcade_pulsar.h for the provenance, the IP boundary, the unit
// conversion, every constant with its source, the three adjudicated
// divergences, the field map, and the registration list.
//
// Spec: docs/design/arcade_pulsar_recovery.md. Implementation contract:
// docs/design/arcade_enemies.md. Shared-path map:
// docs/design/enemy_pipeline_audit.md.
//
// STRUCTURE. One file-local POD for the fleet-wide pulse, a handful of
// file-local helpers, and the entry points declared in the header. No virtual,
// no vtable, no function-pointer table -- the whole family is straight-line
// code behind two `if`s, and the only dispatch anywhere near it is the `switch`
// on Enemy::id in enemies.cpp (DOCTRINE.md perf doctrine, enemy_family.h).
//
// NO STL GROWTH IN THE PER-TICK PATH. There are exactly three container-growth
// sites in the whole file and none of them runs every tick. TWO ARE ONE-OFFS:
// the spawn's push_back and the rim split's -- both onto vectors init_level
// already reserve()s to MAX_ENEMIES. THE THIRD is the spark's lane step, a
// push_back plus a pop_back on those same already-reserved vectors (the same
// swap-down the shared _transfer_enemy uses), and it happens at most once per
// spark per ~4.45 ticks.
// ============================================================================

#include "arcade_pulsar.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>   // NO rand() in this TU, despite the sibling families:
                     // `spawn` uses the caller's uniform-random lane AS
                     // GIVEN (arcade_pulsar.h; contrast the tanker, whose
                     // lane is discarded), so this family rolls nothing.
                     // The only standard names used here are std::fabs
                     // (<cmath>) and INT32_MIN (<cstdint>). Do not add a
                     // lane roll to make the old annotation true.

namespace ts {
namespace enemyfam {

namespace {

// ============================================================================
// THE FLEET-WIDE PULSE
//
// ONE counter for the whole game, not one per object (recovery sec 1.3, and
// sec 8.2: "A per-object phase would look wrong and would multiply the sound").
// Every pulsar, every spark and every pulsar-tanker extends and contracts in
// unison off this, and the renderer must read it too -- which is exactly why it
// is reachable through ArcadePulsar's statics rather than hidden here.
//
// WHERE IT LIVES, AND WHY IT IS NOT ON GameEngine. The recovery (sec 6 item 8)
// and registration item 6 both asked for these four fields on GameEngine beside
// `arcade_fire_timer`, cleared in init_level. That was deliberately NOT done --
// engine.cpp's init_level records the judgement: `pulseShape()` / `pulsePhase()`
// are NO-ARGUMENT statics the renderer calls with no GameEngine in hand, so
// engine-side storage would leave a second copy shadowing this one. `init_level`
// calls `pulseReset()` instead, which supplies the one wave start the reset
// heuristic below cannot observe (a death re-entering the SAME level).
//
// It is four scalars in a POD: no allocation, no dispatch, nothing per-frame
// beyond one float add and one compare for the entire fleet.
// ============================================================================
struct PulseState {
    int   phase    = 0;             // 0..15, the reference's masked counter
    float acc      = 0.0f;          // REFERENCE ticks accumulated toward the next phase
    int   lastTick = INT32_MIN;     // the SIM TICK ms this last advanced on ("never").
                                    // NOT engine.time -- see pulseTick().
    int   level    = -1;            // the wave this counter belongs to
    bool  crackled = false;         // fleet-wide once-per-cycle sound latch
};

PulseState g_pulse;

// The crackle latch, split out so the one place that sets it and the one place
// that clears it are both obvious. The reference's own mechanism exactly: the
// FIRST pulsar to notice the lethal phase plays the sound and raises a global
// flag so the rest of the fleet stays silent, and the flag is cleared when the
// counter wraps to 0 -- i.e. ONCE PER CYCLE FOR THE WHOLE FLEET, not once per
// pulsar.
inline bool crackleLatched()  { return g_pulse.crackled; }
inline void latchCrackle()    { g_pulse.crackled = true; }

// ============================================================================
// Shared predicates
// ============================================================================

// "The player's claw is flagged vulnerable" -- condition 1 of BOTH catch tests.
// This port's spelling of that flag is the one ArcadeFlipper::tryGrab already
// established, and it is deliberately the same expression in both families so
// the roster cannot end up with two ideas of when the claw can be taken.
//
// NB it also answers the recovery's UNCERTAIN sec 7.3 (whether a spark can kill
// during the end-of-wave zoom) by DEFERRING TO THE PORT'S EXISTING PREDICATE
// rather than inventing an answer: out_animation is this engine's level-exit
// slide, and the claw is not catchable during it. The asymmetry the recovery IS
// certain about -- that the pulsar's whole routine stops during the zoom and
// the spark's does not -- is reproduced in the two update()s.
inline bool clawVulnerable(const GameEngine& engine) {
    const PlayerInfo& p = engine.player;
    return p.out_animation == 0 && p.gameover_animation <= 0;
}

// ============================================================================
// Construction helpers
// ============================================================================

// Everything an Enemy needs to BE a spark. Used by the rim split for both
// halves, so the two can never be built differently -- which is also how
// adjudicated divergence (b) (the reference never writes the clone's colour)
// comes out right for free: the clone is a struct copy of the original.
void initSpark(Enemy& e, int lane, int dirSign, int time) {
    e.id           = ARCADE_PULSAR_SPARK;
    // life 1 is load-bearing rather than a placeholder: the shared shot/enemy
    // exchange is a mutual hit-point subtraction, so one hit kills and the
    // laser pierces, for free (arcade_enemies.md sec 3.5).
    e.life         = ENEMY_LIFE[ARCADE_PULSAR_SPARK];
    e.current_lane = lane;
    // "re-pin to the lane midpoint" -- the reference's own re-anchor call. A
    // spark sits square in its lane, exactly like the pulsar that made it.
    e.anchor       = ANCHOR_LANE_MID;
    e.pivot_side   = -1;
    e.anchor_rot   = 0.0f;
    e.cross_t      = 0.0f;
    e.px           = 0.0f;
    e.py           = 0.0f;
    e.animation_phase = 0;          // UNUSED on a spark; see the field map
    e.sidestep_freq   = 0;
    e.shoot_freq      = 0;
    // ---- THE mush_flag TRAP (enemy_pipeline_audit.md sec 3) ---------------
    // `mush_flag` is named as the mushroom part bitmask and is not used that
    // way: four shared sites treat bit 1 as a general gate. A hand-built enemy
    // inherits 0 and is therefore (a) damaged by an upgraded player shot only
    // 1 roll in 11 -- reading as "the spark is a bullet sponge", the exact
    // opposite of the one-hit rule -- and (b) given this engine's breathing
    // wobble and z-stretch, which arcade_enemies.md sec 5.3 forbids for arcade
    // enemies. 15 is what GameEngine::init_enemy sets and answers both. The
    // audit's own recommendation is to replace the four gates with named
    // predicates as its OWN change; until that lands this is the one-line form
    // of the same answer. Do not spread `= 15` to more spawn sites.
    e.mush_flag    = 15;
    ArcadePulsarSpark::setDir(e, dirSign);
    ArcadePulsarSpark::prop(e)  = 0.0f;
    // Stamped with the CURRENT tick, so a freshly split spark does not also
    // act on its creation frame. The recovery's UNCERTAIN sec 7.6 asks exactly
    // this and could not resolve it (it depends on the reference's list
    // insertion policy against a traversal that has already stashed its next
    // pointer); the deterministic reading is the defensible one. It is only a
    // question of ONE tick, and the spark is shootable immediately either way.
    ArcadePulsarSpark::stamp(e) = time;
    // e.z is DELIBERATELY NOT TOUCHED: both sparks inherit the pulsar's depth.
}

// ============================================================================
// The spark's lane step
// ============================================================================

// Step exactly one lane in the spark's own direction and re-pin to the new
// lane's midpoint. Returns true iff the spark LEFT `lane`'s slot `idx` (the
// enemy_family.h removal contract).
//
// THE TRANSFER IS DONE HERE rather than through enemies.cpp's `_transfer_enemy`
// for two reasons, and the second one is fatal rather than stylistic: that
// helper is `static` in another TU and unreachable, AND it FLIPS
// `animation_vec` on every transfer (audit sec 7/R3) -- which on a spark is THE
// STEP DIRECTION, so routing through it would reverse the spark on every single
// step and it would oscillate between two lanes forever instead of circling.
// Same swap-down semantics, same already-reserve()d vectors, and
// `enemies_nums` is deliberately untouched: a transfer is not a removal.
bool stepOneLane(GameEngine& engine, int lane, int idx, Enemy& enemy) {
    const int n = engine.lane_count;
    if (n < 2) return false;            // degenerate web: nowhere to walk

    int d   = ArcadePulsarSpark::dir(enemy);
    int dst = lane + d;

    if (engine.grid_level_go_round) {
        // CLOSED WEB: running off either end WRAPS. The two sparks circle
        // forever in opposite directions, passing each other -- that is the
        // whole picture of the enemy.
        dst = ((dst % n) + n) % n;
    } else if (dst < 0 || dst >= n) {
        // OPEN WEB: the spark REVERSES and immediately re-runs the step in the
        // new direction, so it bounces between the two ends WITHOUT LOSING A
        // TICK. (Reversing and waiting for the next propagation window would
        // make it visibly hesitate at every end; the reference does not.)
        d = -d;
        ArcadePulsarSpark::setDir(enemy, d);
        dst = lane + d;
        if (dst < 0 || dst >= n) return false;   // unreachable: n >= 2
    }
    if (dst == lane) return false;

    // PORT CAP, NOT THE REFERENCE'S. MAX_ENEMIES is this engine's per-lane
    // ceiling and the reference has no equivalent. Staying put -- keeping the
    // direction and retrying at the next propagation window -- is the least
    // surprising answer: reversing here would make a crowded lane look like an
    // open-web end, and forcing the move would corrupt the pool.
    if (engine.grid[dst].num_enemies >= MAX_ENEMIES) return false;

    GridElement& from = engine.grid[lane];
    GridElement& to   = engine.grid[dst];

    Enemy moved = from.enemies[idx];        // copy AFTER setDir, so it carries
    moved.current_lane = dst;               // any open-web reversal
    moved.anchor       = ANCHOR_LANE_MID;   // "re-pin to the new lane's midpoint"
    to.enemies.push_back(moved);
    to.num_enemies += 1;

    from.num_enemies -= 1;
    if (idx < from.num_enemies)
        from.enemies[idx] = from.enemies[from.num_enemies];
    if (!from.enemies.empty())
        from.enemies.pop_back();
    return true;
}

} // namespace

// ============================================================================
// ArcadePulsar -- the pulse
// ============================================================================

int ArcadePulsar::pulseFramesPerPhase(int level) {
    // Selected ONCE per wave by floor(wave / 4) -- the source's own comment is
    // "word every 4 waves". `level` is engine.current_level, 0-BASED.
    int idx = level / 4;
    if (idx < 0) idx = 0;
    // The table covers waves 0..111. The reference relies on its own wave wrap
    // to stay inside it and would read one word past the end on waves 112-115
    // (recovery UNCERTAIN sec 7.4 -- benign there, and unreachable in this
    // port's 100 levels). Clamping is the only difference, and it is a real
    // difference only somewhere this port cannot go.
    if (idx >= PulseTableLen) idx = PulseTableLen - 1;
    // The stored value is a RELOAD for a decrement-then-test-sign countdown,
    // so a reload of 8 permits 9 passes: frames per phase = reload + 1.
    return PULSE_RELOAD[idx] + 1;
}

float ArcadePulsar::pulsePhaseTicks(int level) {
    // A reference TICK COUNT converted to ENGINE ticks. Fractional on purpose
    // -- see arcade_pulsar.h's tick-rate block: rounding collapses the two
    // hardest of the six difficulty bands into one, and the pulse rate IS the
    // pulsar's entire difficulty scaling.
    return (float)pulseFramesPerPhase(level) / TICK_RATE;
}

int   ArcadePulsar::pulsePhase()  { return g_pulse.phase; }
bool  ArcadePulsar::pulseLethal() { return g_pulse.phase == LethalPhase; }
int   ArcadePulsar::pulseShape()  { return PULSE_SHAPE[g_pulse.phase & PulseMask]; }
float ArcadePulsar::pulseAmplitude() {
    return (float)pulseShape() / (float)ShapeMax;
}

void ArcadePulsar::pulseReset() {
    g_pulse = PulseState{};
}

void ArcadePulsar::pulseTick(GameEngine& engine, int tick_ms) {
    PulseState& s = g_pulse;

    // `tick_ms` is the SIMULATION tick clock (EnemyCtx::time), never
    // engine.time -- see the header. engine.time is a per-RENDERED-FRAME wall
    // stamp and game_advance batches up to 15 sim ticks behind one frame, so
    // using it would make the pulse rate frame-rate dependent and halve it on
    // the GPU-bound 3DS target.

    // ---- THE WAVE RESET --------------------------------------------------
    // The reference zeroes the counter at every wave start, and that matters:
    // it is what makes the first pulse of a level land at a predictable phase
    // rather than wherever the previous level left off.
    //
    // THE TRIGGERS. This branch infers a wave start from two observable facts
    // -- the level number changed, or the tick clock ran BACKWARDS (a new run,
    // or a fresh GameEngine in a harness). The third trigger comes from
    // OUTSIDE: `GameEngine::init_level` calls `pulseReset()` directly
    // (engine.cpp), which zeroes this state and leaves `level` at -1 so the
    // check below re-stamps the wave start on the next tick. That is what
    // covers a DEATH re-entering the SAME level, where `current_level` does not
    // change. See engine.cpp for why the counter stays owned here rather than
    // moving onto GameEngine.
    if (engine.current_level != s.level || tick_ms < s.lastTick) {
        s.phase    = 0;
        s.acc      = 0.0f;
        s.crackled = false;
        s.level    = engine.current_level;
        s.lastTick = tick_ms;
        return;                       // the wave-start tick itself does not advance
    }

    // IDEMPOTENT PER SIM TICK. Every family update() calls this, so on a tick
    // with six pulsars alive it runs six times and advances once. That is what
    // makes it correct whether it is driven from here or from a game_step slot
    // (the end state), and it is why it can be called unconditionally at the
    // top of both update()s without any caller having to know who else did.
    if (tick_ms == s.lastTick) return;
    s.lastTick = tick_ms;

    // ---- THE ADVANCE ------------------------------------------------------
    // Accumulate REFERENCE ticks and fire a phase when the band's integer
    // frames-per-phase is reached. `while` rather than `if` costs nothing and
    // is honest: TICK_RATE (1.124) is far below the smallest band (4), so it
    // can never actually loop twice, but the loop does not depend on knowing
    // that.
    const float per = (float)pulseFramesPerPhase(engine.current_level);
    s.acc += TICK_RATE;
    while (s.acc >= per) {
        s.acc -= per;
        s.phase = (s.phase + 1) & PulseMask;
        // The crackle latch is released when the counter WRAPS, which is what
        // makes the sound once-per-cycle rather than once-per-lethal-phase-
        // entry-per-pulsar.
        if (s.phase == 0) s.crackled = false;
    }
}

// ============================================================================
// ArcadePulsar -- collision
// ============================================================================

bool ArcadePulsar::killsPlayerInLane(const GameEngine& engine, int lane) {
    // THE WHOLE TEST, and its shortness is the point (recovery sec 1.4 /
    // sec 8.1). The pulsar uses the LANE-ONLY variant of the reference's catch
    // routine, and there is NO DEPTH WINDOW ANYWHERE IN IT: a pulsar in the
    // lethal phase kills the player from anywhere down the tube, at any
    // distance, as long as the claw is standing in its lane. That is why the
    // lane strobes -- the flash is the warning and the whole lane is the
    // hazard. Do not add a |dz| test here "because every other enemy has one".
    if (!pulseLethal()) return false;

    const PlayerInfo& p = engine.player;

    // 1. the claw is flagged vulnerable
    if (!clawVulnerable(engine)) return false;

    // 2. the claw is IN THIS LANE. This is the entire spatial test.
    if (p.grid_element_pos != lane) return false;

    // 3. the claw's own depth is still at or beyond the rim plane -- the
    //    reference's comment is "check if on top (may be jumping!)". THIS IS
    //    THE ONLY ESCAPE THERE IS: not distance, not firing, not the
    //    superzapper. In this engine the claw rests at z = 0 and a jump drives
    //    z NEGATIVE (player.cpp's parabola bottoms at -6.25), so `>= 0` is the
    //    port spelling of "has not moved past the rim plane" -- the same
    //    expression player.cpp's own electrocution test already uses.
    if (p.z < RimPlaneZ) return false;

    return true;
}

// ============================================================================
// ArcadePulsar -- update
// ============================================================================

bool ArcadePulsar::update(GameEngine& engine, const EnemyCtx& ctx,
                          int lane, int idx, Enemy& enemy) {
    // The fleet-wide pulse, before anything reads it. Idempotent per tick, so
    // whichever pulsar-family enemy the lane walk reaches first advances it and
    // every later one sees the same phase. ctx.time is the SIM TICK clock --
    // see pulseTick's header note on why engine.time would be wrong.
    pulseTick(engine, ctx.time);

    // ---- THE END-OF-WAVE GUARD -------------------------------------------
    // The reference's pulsar routine early-outs ENTIRELY during the
    // level-complete zoom: it does not descend, does not electrify, does not
    // kill and does not split. The SPARK deliberately has no such guard, and
    // that asymmetry is recovered and certain (only its consequence is
    // UNCERTAIN, sec 7.3) -- so it is reproduced here rather than smoothed
    // over. This port's nearest equivalent of that zoom is the player's
    // level-exit slide.
    //
    // NB the pulsar is still SHOOTABLE during the slide, where the reference's
    // bullet test sits behind this same early-out. Closing that would mean
    // teaching the shared `enemy_shootable()` about out_animation, which is a
    // shared-file edit this wave may not make; it is in the report.
    if (engine.player.out_animation != 0) return false;

    // ======================================================================
    // ARRIVAL -- the inert collapsed dot
    // ======================================================================
    if (mode(enemy) == MODE_ARRIVAL) {
        // The shared collapsed-object head closes a fixed 2 world-z per tick.
        // The live routine is NOT dispatched while an object is collapsed, so
        // an inbound pulsar does NOT electrify its lane, is NOT lethal, and
        // (via shootable()) cannot be hit by anything.
        enemy.z -= ApproachDz;
        if (enemy.z > FarPlaneZ) return false;

        // ---- THE TOUCHDOWN HOLD -- the pulsar-only rule -------------------
        // A pulsar that has reached the far plane while the global phase is at
        // or above HoldUntilPhase has THE STEP IT JUST TOOK ADDED STRAIGHT
        // BACK, and tries again next tick. The reference's own comment is
        // "makes pulsars wait until innocent before touchdown". Nothing else
        // in either roster does this, and without it a pulsar can materialise
        // ALREADY LETHAL in the player's lane -- which neither reference can
        // do (recovery sec 8.4). It can hover here for up to MaxHoldPhases of
        // the 16.
        if (pulsePhase() >= HoldUntilPhase) {
            enemy.z += ApproachDz;
            return false;
        }

        // Land exactly on the far plane, so the descent always starts from
        // z = 25 with no sub-tick residue (the same clamp the flipper's rail
        // and arrival both take; the reference clears the depth fraction here).
        enemy.z = FarPlaneZ;
        setMode(enemy, MODE_DESCEND);
        // ...and it begins descending ON THAT SAME TICK: fall through.
    }

    // ======================================================================
    // DESCEND -- the only live state
    //
    // THE ORDER BELOW IS RECOVERED AND LOAD-BEARING: the depth step happens
    // BEFORE the lethal test, and the rim split is LAST.
    // ======================================================================

    // ---- 1. DEPTH --------------------------------------------------------
    // The lane NEVER changes: a pulsar spawns in a lane and dies in it. No
    // lane-seeking, no sidestep, no flip, no "toward player" steering.
    // The speed NEVER changes either -- there is no per-wave pulsar z-speed
    // table in either tree (see the header). 284.65 ticks top to bottom, on
    // every level in the game.
    enemy.z -= DescentDz;

    // ---- 2. THE CRACKLE --------------------------------------------------
    // The first live pulsar to notice the lethal phase plays it and raises the
    // fleet-wide latch, so it fires ONCE PER PULSE CYCLE FOR THE WHOLE FLEET.
    // Placed before the kill so the sound lands on the frame the lane goes
    // live, whether or not anybody is standing in it.
    if (pulseLethal() && !crackleLatched()) {
        latchCrackle();
        engine.sfx.push(CrackleSfx, SfxAction::ONE_SHOT,
                        CrackleSfxPitch, CrackleSfxVol);
    }

    // ---- 3. THE KILL -----------------------------------------------------
    // No depth window; see killsPlayerInLane(). THE PULSAR SURVIVES ITS OWN
    // KILL -- this is not a death path for it, and it neither scores nor
    // explodes.
    if (killsPlayerInLane(engine, lane)) {
        engine.init_gameover(ctx.time, DeathTextId);
        return false;
    }

    // ---- 4. THE RIM ------------------------------------------------------
    // It does NOT die here: it becomes two sparks (recovery sec 8.5 -- "the rim
    // split is not a death. It scores nothing, plays no explosion, and produces
    // two live enemies that pay 150 each"). So there is deliberately no
    // init_explosion, no arcade_kill_score, and no note_powerup_kill on this
    // path.
    if (enemy.z <= RimPlaneZ) {
        // Clamp first. JUDGEMENT CALL, flagged: the recovery does not say the
        // conversion clamps, only that both sparks inherit the pulsar's depth.
        // A spark NEVER MOVES IN Z AGAIN, so an unclamped overshoot (up to
        // 0.0878 past the rim) would be a permanent sub-rim offset for the rest
        // of its life -- and both the flipper's rail and this family's own
        // arrival clamp at their planes for exactly that reason. It changes
        // nothing about the catch test either way: 0.0878 is well inside the
        // 0.3125 window.
        enemy.z = RimPlaneZ;
        ArcadePulsarSpark::splitFromPulsar(engine, ctx, lane, idx, enemy);
        // TRUE, and the contract's own wording is why (enemy_family.h: "a
        // transfer to another lane counts as removal -- the slot now holds a
        // different enemy"). Slot `idx` now holds SPARK A, and the clone's
        // push_back may have reallocated this lane's vector, so `enemy` is
        // stale. move_enemies re-reads the slot without advancing, which gives
        // it spark A -- already stamped for this tick, so it does not act, but
        // it is shootable and it is in the pool from this instant.
        return true;
    }

    return false;
}

// ============================================================================
// ArcadePulsar -- construction
// ============================================================================

bool ArcadePulsar::spawn(GameEngine& engine, int lane) {
    if (lane < 0 || lane >= engine.lane_count) return false;
    GridElement& elem = engine.grid[lane];
    if (elem.num_enemies >= MAX_ENEMIES) return false;

    Enemy e{};
    e.id   = ARCADE_PULSAR;
    // life 1 is load-bearing (see initSpark's note).
    e.life = ENEMY_LIFE[ARCADE_PULSAR];
    // THE ARRIVAL DOT: spawned a further 300 world-z behind the far plane and
    // flagged collapsed, so it reads as a distant pixel closing in. Same depth
    // and same closing rate as the flipper's and the tanker's -- it is the
    // same shared insertion path (static_asserted in the header).
    e.z            = SpawnZ;
    e.current_lane = lane;
    e.anchor       = ANCHOR_LANE_MID;
    e.pivot_side   = -1;
    e.anchor_rot   = 0.0f;
    e.cross_t      = 0.0f;
    e.px           = 0.0f;
    e.py           = 0.0f;
    // The four generic scratch fields carry NSDMI defaults (animation_vec 1,
    // max_animation 100, oo_max_animation 0.01) that mean nothing to this
    // family. Zeroed explicitly so a reader of a dumped Enemy is not misled and
    // so the harness's per-field comparisons are meaningful.
    e.animation_vec    = 0;
    e.max_animation    = 0;
    e.oo_max_animation = 0.0f;
    e.sidestep_freq    = 0;
    e.shoot_freq       = 0;
    // The mush_flag trap -- see initSpark.
    e.mush_flag    = 15;
    setMode(e, MODE_ARRIVAL);

    elem.enemies.push_back(e);
    elem.num_enemies += 1;
    engine.enemies_nums[ARCADE_PULSAR] += 1;

    // AUDIT sec 8, THE LEVEL-HANG: enemies_todo[] is decremented by the SHARED
    // machinery in exactly one place -- init_embrio -- and the arcade arrival
    // replaces the embryo entirely. A release that never decrements it leaves
    // is_level_clear() false forever with nothing on screen and no diagnostic.
    // The release point IS the spawn for this set, so every arcade family
    // decrements its own here (see the accounting rule above arcade_release in
    // enemies_shared.h) -- which means the caller's spawner must NOT also
    // decrement it.
    if (engine.enemies_todo[ARCADE_PULSAR] > 0)
        engine.enemies_todo[ARCADE_PULSAR] -= 1;
    return true;
}

void ArcadePulsar::becomeFromFlipper(GameEngine& engine, int lane, Enemy& enemy) {
    // THE ID MUTATION, and the counter fix is the whole of audit sec 7/R4 for
    // this one transition: rewriting `enemy.id` in place without it leaves
    // `enemies_nums[old]` never decremented and drives `enemies_nums[new]`
    // NEGATIVE on death, after which init_embrio's per-type cap lets the level
    // over-spawn that type for the rest of the wave.
    const int oldId = enemy.id;
    if (oldId != ARCADE_PULSAR) {
        if (engine.enemies_nums[oldId] > 0) engine.enemies_nums[oldId] -= 1;
        engine.enemies_nums[ARCADE_PULSAR] += 1;
    }
    enemy.id = ARCADE_PULSAR;

    // MODE_DESCEND, NOT MODE_ARRIVAL: the reference's conversion does not touch
    // depth and does not restart an arrival, so the new pulsar is live -- and
    // CLAW-LETHAL FROM THAT INSTANT -- at whatever depth its parent tanker
    // opened at. (arcade_enemies.md sec 2.2 S2c states that outright, as
    // "clearing the sentinel to 0 so it is claw-lethal from that instant";
    // in this family's encoding, being live IS mode DESCEND.)
    setMode(enemy, MODE_DESCEND);

    // "...and re-snap it to its new lane's midpoint and outward angle." The
    // flipper reached this lane by flipping into it, so it is still hinged on a
    // border vertex; a pulsar sits square in the lane.
    enemy.current_lane = lane;
    enemy.anchor       = ANCHOR_LANE_MID;
    enemy.pivot_side   = -1;
    enemy.anchor_rot   = 0.0f;
    enemy.cross_t      = 0.0f;

    // Clear the flipper's scratch fields, all of which mean something else on a
    // pulsar (sub-variant, pause countdown, forced spin, tick stamp, flip total
    // and flip rate). Left alone they are not merely untidy: `px`/`py` are the
    // ANCHOR_FREE world position and a stale non-zero pair would place the body
    // wrongly the moment anything reads that anchor mode.
    enemy.animation_vec    = 0;
    enemy.max_animation    = 0;
    enemy.oo_max_animation = 0.0f;
    enemy.sidestep_freq    = 0;
    enemy.shoot_freq       = 0;
    enemy.px               = 0.0f;
    enemy.py               = 0.0f;

    // NOT TOUCHED, ALL INHERITED, exactly as the reference's conversion leaves
    // them: z (it keeps the depth it flipped at), life, and mush_flag (the
    // flipper was built with 15).
}

// ============================================================================
// ArcadePulsarSpark
// ============================================================================

int ArcadePulsarSpark::splitFromPulsar(GameEngine& engine, const EnemyCtx& ctx,
                                       int lane, int idx, Enemy& pulsar) {
    // `idx` is part of the family contract's slot identity (it is the slot
    // `pulsar` occupies) and is deliberately unused: the conversion happens IN
    // PLACE through the reference, so nothing has to index the vector -- which
    // is also what keeps the function correct across the clone's push_back.
    (void)idx;
    GridElement& elem = engine.grid[lane];

    // ---- SPARK A: the pulsar itself, converted IN PLACE -------------------
    // Direction +1, written explicitly. THE ADJUDICATED CASE (divergence (a)
    // in the header): the authoritative source writes the SECOND direction to
    // this object instead of to the clone, so its survivor ends the frame
    // running -1 and the clone's direction is whatever the recycled pool slot
    // held. DOCTRINE.md records the user's decision that the later port wins here
    // -- reproducing pool-recycling order is noise, not fidelity -- so both
    // directions are written, to the right objects.
    const int oldId = pulsar.id;
    if (oldId != ARCADE_PULSAR_SPARK) {
        if (engine.enemies_nums[oldId] > 0) engine.enemies_nums[oldId] -= 1;
        engine.enemies_nums[ARCADE_PULSAR_SPARK] += 1;
    }
    initSpark(pulsar, lane, +1, ctx.time);

    int made = 1;

    // ---- SPARK B: the clone, running the OTHER way -----------------------
    // PORT CAP, not the reference's: with the lane already full there is
    // nowhere to put the clone, so the split yields ONE spark. Refusing is the
    // only safe answer (the alternative is corrupting the pool), it cannot
    // strand anything -- spark A is a normal live enemy that the level's clear
    // test already accounts for -- and it is vanishingly rare, since the pulsar
    // itself is one of the enemies filling that lane. Flagged in the report.
    if (elem.num_enemies < MAX_ENEMIES) {
        // COPIED BEFORE THE push_back, which may reallocate this lane's vector
        // and leave `pulsar` dangling. Nothing below touches `pulsar` again,
        // and update() returns "removed" so the caller re-reads the slot.
        Enemy b = pulsar;
        setDir(b, -1);
        elem.enemies.push_back(b);
        elem.num_enemies += 1;
        engine.enemies_nums[ARCADE_PULSAR_SPARK] += 1;
        made = 2;
    }

    // enemies_todo[] IS DELIBERATELY UNTOUCHED. Sparks are not scheduled
    // population; they are a consequence of a pulsar that was already counted,
    // exactly like the tanker's hatchlings. A spark that decremented
    // enemies_todo[] would steal level-scheduled releases and, once that
    // counter hit 0, drive it NEGATIVE -- which is_level_clear()'s `> 0` test
    // reads as "clear", MASKING the error rather than reporting it.
    return made;
}

bool ArcadePulsarSpark::update(GameEngine& engine, const EnemyCtx& ctx,
                               int lane, int idx, Enemy& enemy) {
    // The fleet-wide pulse. A spark breathes in lockstep with every pulsar on
    // screen -- it reuses the pulsar's own shape and animation, still driven by
    // the same global counter. Ticked BEFORE the once-per-tick guard so a frame
    // on which every spark happens to be stamped out still advances it.
    ArcadePulsar::pulseTick(engine, ctx.time);

    // ONE ACT PER TICK. move_enemies walks lanes 0..n-1, so a spark that steps
    // to a HIGHER lane index is visited AGAIN in the same frame and would take
    // a second step -- which compounds at the seam of a closed web into a spark
    // that laps the ring. Same guard, same field and same reason as
    // ArcadeFlipper's; delete it the day move_enemies grows a transfer guard.
    if (stamp(enemy) == ctx.time) return false;
    stamp(enemy) = ctx.time;

    // ---- 1. THE PLAYER CHECK IS FIRST ------------------------------------
    // Recovered ordering: the spark tests the claw before anything else,
    // including before its own bullet test. And it has NO end-of-wave guard,
    // unlike the pulsar -- the asymmetry is deliberate (see clawVulnerable()).
    {
        const PlayerInfo& p = engine.player;
        // Same lane, claw vulnerable, and -- unlike the pulsar -- A REAL DEPTH
        // WINDOW: this is the ordinary catch routine, at the ADJUDICATED 2
        // world-z (CatchDz). In practice both bodies sit at the rim, so it
        // reads as "same lane" plus a small tolerance that a jumping claw
        // immediately leaves.
        if (clawVulnerable(engine) && p.grid_element_pos == lane
            && std::fabs(enemy.z - p.z) <= CatchDz) {
            engine.init_gameover(ctx.time, ArcadePulsar::DeathTextId);
            // THE SPARK SURVIVES ITS OWN KILL, exactly like the pulsar: this is
            // not a death path for it.
            return false;
        }
    }

    // ---- 2. THE SHOT / ZAP CHECK -----------------------------------------
    // SHARED, and already correctly ordered around this call: collision.cpp
    // move_shots is tick slot 322 and runs BEFORE move_enemies (324), and
    // enemies.cpp's catch-up sweep runs immediately after this function
    // returns. A spark is one hit from death in every state, and the
    // superzapper reaches it through the same shared path.

    // ---- 3./4. THE PROPAGATION COUNTDOWN ---------------------------------
    // One lane every 5 REFERENCE ticks, constant for the whole game -- there is
    // no per-wave propagation table in either tree. Carried as a fractional
    // accumulator in reference ticks for the same reason the pulse is: 5
    // reference ticks is 4.4477 engine ticks, and rounding either way costs
    // 10-12% of the ring speed on the one enemy whose entire threat is how fast
    // it comes round at you.
    //
    // THE STEP IS GATED, NOT CONTINUOUS (recovery sec 8.6): the spark does not
    // crawl, it HOPS a whole lane and snaps to the midpoint. It never moves in
    // z at all.
    prop(enemy) += ArcadePulsar::TICK_RATE;
    if (prop(enemy) < PropFrames) return false;
    prop(enemy) -= PropFrames;

    // ---- 5. STEP ONE LANE, AND RE-PIN ------------------------------------
    return stepOneLane(engine, lane, idx, enemy);
}

} // namespace enemyfam
} // namespace ts
