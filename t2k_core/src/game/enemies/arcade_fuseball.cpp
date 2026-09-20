// ============================================================================
// arcade_fuseball.cpp -- THE FUSEBALL. See arcade_fuseball.h for the contract,
// the five things that must not be got wrong, the rail model, the field map,
// the conflict with docs/design/arcade_enemies.md, and the REGISTRATION list.
//
// SPEC: docs/design/arcade_fuseball_recovery.md. Shared-path hazards:
// docs/design/enemy_pipeline_audit.md.
//
// STATE MACHINE, one line each:
//   ARRIVAL  inert far dot, invulnerable, does not think and does not spin;
//            closes 0.35131 z/tick from z = 71.875 to the far plane, then
//            CLIMB on that same tick.
//   CLIMB    descends IN PLACE on one rail. The ONLY state where depth moves,
//            and the state with NO COLLISION CALL OF ANY KIND -- it can be
//            neither shot nor touched, at any depth, on either side. After
//            49 reference ticks it picks a side and crosses.
//   CROSS    traverses ONE lane at FROZEN depth in 16 exact steps of the
//            lane's own border delta / 16. Shootable only in the middle 9/16;
//            LETHAL to a player in that lane for the whole crossing.
//
// STRUCTURE. Two mode handlers, four helpers, all file-local; the only things
// the rest of the game sees are the entry points declared in the header. No
// virtual, no vtable, no function-pointer table -- the mode dispatch is a
// two-way branch on a three-value enum, exactly like enemies.cpp's own id
// dispatch (DOCTRINE.md perf doctrine, enemy_family.h). No STL growth in the
// per-tick path: the only container touch is the lane transfer's push_back,
// which happens at most twice per 72-tick cycle onto a vector init_level
// already reserve()s to MAX_ENEMIES.
//
// AND ONE PROPERTY WORTH STATING OUTRIGHT, because it is unusual in this
// directory: THE UPDATE TOUCHES NO GEOMETRY AT ALL. Direction is lane-index
// arithmetic, the crossing is a step counter, and the position is a fraction
// the renderer resolves through web_geometry.h's single lane accessor. So
// there is no second definition of where a lane's borders are, no sqrt, no
// atan2, and nothing for a non-unit lane vector to be wrong about.
//
// ============================================================================
// MEASURED, against the real GameEngine, so nobody has to re-derive it
// ============================================================================
//   climb phase        44 ticks, EVERY TIME   (ideal 43.587 -- see the note
//                                              on the two reloads below)
//   cross phase        29..30 ticks, mean 29.026  (ideal 28.465)
//   full cycle         73.03 ticks / 1.168 s      (reference 81 of its ticks
//                                                  = 1.153 s, so +1.3%)
//   descent duty       0.6025                     (ideal 0.6049)
//   ticks on which depth AND lateral position both moved:            0
//   crossing steps: bit-exactly k/16 on all 25721 sampled positions
//   step length vs |lane|/16, all 100 webs / 1513 lanes: worst 4.6e-07
//   step 16 vs the lane's right border vertex:           worst 4.2e-08
//   spawn -> rim at level 1: 592 ticks (9.47 s), of which 134 are the dot
//   shootable fraction at the rim: 55% of a crossing, 22% of its LIFE
//
// THE TWO RELOADS ARE DELIBERATELY DIFFERENT, and this is the only place a
// judgement call about timing was made:
//
//   * THE CROSS DELAY CARRIES ITS RESIDUE (`t += CrossStepTicks`). Rounding it
//     to a flat 2 engine ticks per step would make a crossing 32 ticks instead
//     of 28.5 -- 12% long -- and the crossing is the enemy's signature motion,
//     the thing a player times a shot against. Carrying the residue gives a
//     Bresenham 2/2/2/1 cadence whose 16 steps total 29 ticks.
//   * THE RAIL DWELL DOES NOT (`t = RiseTicks`). Carrying it there would make
//     the dwell alternate 43/44/43/44 to save 0.4 of a tick in 43.6 (0.95%),
//     and a jittering park reads worse than a constant one. The reference's
//     own dwell is a fixed integer count, so a constant 44 is also the more
//     faithful shape.
// ============================================================================

#include "arcade_fuseball.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>   // rand

namespace ts {
namespace enemyfam {

// ============================================================================
// Per-level scalars
// ============================================================================

int ArcadeFuseball::speedRaw(int level) {
    // Consecutive level PAIRS share a value. `level` is engine.current_level,
    // 0-BASED. The table covers 0..111; the reference relies on its own wave
    // wrap to stay inside it, this port clamps -- a real difference only above
    // level 111, where the reference would read past the table.
    int idx = level / 2;
    if (idx < 0) idx = 0;
    if (idx >= SpeedTableLen) idx = SpeedTableLen - 1;
    return SpeedRaw[idx];
}

float ArcadeFuseball::descentDz(int level) {
    // A per-tick DELTA, so it takes the RATE conversion.
    return (float)speedRaw(level) * RAW_Z * TICK_RATE;
}

float ArcadeFuseball::effectiveDz(int level) {
    // What actually governs how long a fuseball takes to reach the rim: only
    // 60.5% of its ticks descend, because a crossing one is frozen in depth.
    // At the slowest table entry that is 0.0531 z/tick -- the 25-unit tube in
    // ~471 engine ticks / 7.5 s, plus the 2.1 s approach dot; at the fastest,
    // ~3.4 s. Informational (nothing in the family reads it) but exposed so a
    // balance question can be answered without re-deriving the duty cycle.
    return descentDz(level) * DescentDuty;
}

// ============================================================================
// Presentation helpers -- pure, so both backends get the same answer
// ============================================================================

bool ArcadeFuseball::legMirrored(int time_ms, int leg) {
    // FLOOR-divide, so the pattern also holds still either side of zero rather
    // than reflecting about it (engine.time is monotonic in practice; this
    // costs one compare and removes the class).
    int frame = time_ms / FlickerPeriodMs;
    if (time_ms < 0 && (time_ms % FlickerPeriodMs) != 0) frame -= 1;

    // The reference reseeds its generator FROM THE FRAME COUNTER at the top of
    // the draw routine and restores it afterwards, so the five-leg pattern is
    // a pure function of the frame counter and is IDENTICAL for every frame
    // inside a reseed window. Any full-avalanche integer mix reproduces that
    // property; what matters is (a) purity -- the render path must never draw
    // from the shared rand() stream, which would make simulation depend on
    // whether a frame was drawn -- and (b) that the five legs are independent,
    // or the rosette flickers as one block instead of crackling.
    uint32_t h = (uint32_t)frame * 2654435761u + (uint32_t)leg * 2246822519u + 0x9E3779B9u;
    h ^= h >> 16; h *= 2246822519u;
    h ^= h >> 13; h *= 3266489917u;
    h ^= h >> 16;
    return (h & 1u) != 0u;
}

int ArcadeFuseball::rollScore() {
    // 250 / 500 / 750, UNIFORMLY AT RANDOM -- not by depth, not by level, not
    // by how it died. Confirmed three independent ways (recovery sec 1.6).
    return ScoreMin + ScoreStep * (rand() % ScoreChoices);
}

// ============================================================================
// The rail model (arcade_fuseball.h, "THE RAIL MODEL")
// ============================================================================

int ArcadeFuseball::parkedRail(const GameEngine& engine, int lane, const Enemy& e) {
    const int n = engine.lane_count;
    if (n <= 0) return 0;
    // cross_t is exactly 0 or exactly 1 while parked, and 1 only for the
    // clamped last rail of an OPEN web (or a landing whose destination lane
    // was at this port's own MAX_ENEMIES cap). The 0.5 threshold is a
    // formality: both values are exact, and using a midpoint rather than an
    // equality keeps this honest if a caller ever asks mid-crossing.
    int r = lane + ((e.cross_t > 0.5f) ? 1 : 0);
    if (engine.grid_level_go_round && r >= n) r -= n;
    return r;
}

float ArcadeFuseball::crossTForStep(int stepCounter, int direction) {
    // The counter runs 15 -> -1 and the position is advanced BEFORE the
    // decrement, so a counter of `s` means (15 - s) steps have been taken:
    // 0 at entry, 16 when it underflows. EXACT IN BINARY32 -- 1/16 is a power
    // of two, the integers 0..16 are exact, and so is 1 - k/16. That is what
    // makes "16 steps of exactly the border delta / 16" a structural property
    // here rather than an arithmetic hope: there is no accumulation to drift.
    const float taken = (float)(StepStart - stepCounter) * (1.0f / (float)CrossSteps);
    return (direction > 0) ? taken : (1.0f - taken);
}

// ============================================================================
// Direction choice -- PURE LANE-INDEX ARITHMETIC, NO GEOMETRY, NO RANDOMNESS
// (recovery sec 1.2 "Direction choice", sec 7.7)
// ============================================================================

int ArcadeFuseball::chooseDir(const GameEngine& engine, int rail) {
    const int d0 = rail - engine.player.grid_element_pos;

    if (!engine.grid_level_go_round) {
        // OPEN WEB: positive difference -> left, otherwise right. A TIE GOES
        // RIGHT, and that is not an accident of the port -- the reference's
        // test is a two-way branch whose "less or equal" arm is the right one,
        // so a fuseball standing in the player's own lane always commits to
        // the same side. It is also what makes a rimmed fuseball PING-PONG
        // across the player's lane rather than settling.
        return (d0 > 0) ? -1 : +1;
    }

    // CLOSED WEB: take the SHORTER way round. The half-width is an ARITHMETIC
    // SHIFT, so it TRUNCATES -- on a 15-lane web the half is 7, not 7.5, and
    // the exactly-half case therefore falls to the far side.
    const int half = engine.lane_count >> 1;
    if (d0 > 0) return (d0 < half) ? -1 : +1;
    const int d = -d0;
    return (d < half) ? +1 : -1;
}

// ============================================================================
// Predicates
// ============================================================================

bool ArcadeFuseball::shootable(const Enemy& e) {
    // HEADLINE FACT 3, and the whole of it is these two lines.
    //
    // The climb routine contains NO collision call of ANY kind -- read end to
    // end it is a depth step, a timer, a direction choice and a tail call --
    // and the arrival dot's update is not even dispatched. So a fuseball is
    // shootable ONLY while crossing.
    if (mode(e) != MODE_CROSS) return false;

    // ...and then only in the middle NINE of the sixteen crossing positions.
    // The counter runs 15 -> 0, so 4..12 is 3/16 through 11/16 of the lane;
    // the source's own comment on the pair of compares is "only kill if in
    // lane centre".
    //
    // ORDERING NOTE, and it costs at most one tick: the reference evaluates
    // this at the TOP of the crossing routine, on the counter value before
    // that tick's step. In this port the two shot sweeps straddle the family
    // update -- collision.cpp move_shots is tick slot 322 and runs BEFORE
    // move_enemies (324), while enemies.cpp's own enemy-side sweep runs
    // immediately after update() returns -- so one of them reads the counter
    // one step early. Both are inside a 9-step window on a one-hit-kill enemy,
    // and the alternative (moving the test into update()) would mean this
    // family owning a shot sweep, which arcade_enemies.md sec 3.5 explicitly
    // keeps shared.
    const int s = step(e);
    return s >= ShotStepLo && s <= ShotStepHi;
}

bool ArcadeFuseball::zappable(const Enemy& e) {
    // What the SUPERZAPPER should ask instead of shootable(). The reference's
    // fuseball update opens with an UNCONDITIONAL collision call on any
    // superzap frame, BEFORE it dispatches to the climb or cross routine, so
    // the zapper takes a fuseball in any mode -- parked included. Only the
    // inert far dot is exempt, and that is because its update is not run at
    // all.
    //
    // WIRED via `enemy_zappable()` (enemies_shared.h section 1b), the sibling
    // predicate that exists for exactly this id: the tremor kill zone and the
    // zapper target search (both weapons.cpp) ask it instead of
    // `enemy_shootable()`, and every other id falls through to that. Not a
    // nicety -- a fuseball is unshootable ~75% of the time and never lands and
    // never expires, so without the split one parked ball holds
    // `is_level_clear()` open forever.
    return mode(e) != MODE_ARRIVAL;
}

int ArcadeFuseball::threatLane(const Enemy& e, int lane) {
    // DOCUMENTATION-BEARING AND UNCALLED -- the file-local `tryGrab` below is
    // the live player test for this family (see the note at the declaration).
    //
    // Parked: NOTHING, on either side, at any depth.
    //
    // THE ABANDONED TWO-LANE HOOK, and it is why an earlier claim in this
    // project said otherwise. The climb routine DOES write the fuseball's own
    // lane index into a field whose source comment calls it a collision-detect
    // alternative -- but the recovery enumerated all seventeen references to
    // that field and proved the write is DEAD: it is overwritten by the
    // left/right cross setup within the same call chain, before any reader can
    // see it, and the two readers that exist read it only as the
    // left/right direction flag. Neither collision routine touches it at all.
    // The single path that escapes the overwrite (an open web, lane 0, "left"
    // refused) leaves the fuseball in the climb state, where the field is
    // never read. It is a design intent with no consumer. DO NOT IMPLEMENT IT
    // -- and note this port does not even have a field to leave it in, because
    // `dir` carries the flag directly.
    if (mode(e) != MODE_CROSS) return -1;

    // Crossing: the lane it is physically INSIDE, which is exactly the lane
    // index it is stored under -- a right cross keeps index i and traverses
    // lane i, a left cross pre-decremented to i-1 and traverses lane i-1. Live
    // for the WHOLE crossing; the 4..12 window applies to BULLETS ONLY.
    return lane;
}

// ============================================================================
// Internal helpers
// ============================================================================

namespace {

// Move this fuseball from `fromLane` slot `idx` to `toLane`. Mirrors the
// shared `_transfer_enemy` (enemies.cpp) -- same swap-down removal, same
// reserve()d vectors -- MINUS its `animation_phase = -animation_phase` and its
// `animation_vec` flip, both of which are classic-family lane-crossing
// conventions (audit sec 7/R3) and would corrupt this family's direction flag.
// `_transfer_enemy` is `static` in enemies.cpp and unreachable from here; the
// audit and arcade_enemies.md sec 3.3 both name a shared enemies_shared.h
// version as the end state.
//
// `enemies_nums` is deliberately untouched: A TRANSFER IS NOT A REMOVAL.
// The caller must have checked the destination's capacity first, and `moved`
// must already be a COPY -- `enemy` is a reference into the source vector and
// the swap-down below overwrites it.
void transferTo(GameEngine& engine, int fromLane, int idx, int toLane,
                const Enemy& moved) {
    GridElement& from = engine.grid[fromLane];
    GridElement& to   = engine.grid[toLane];

    to.enemies.push_back(moved);
    to.num_enemies += 1;

    from.num_enemies -= 1;
    if (idx < from.num_enemies) from.enemies[idx] = from.enemies[from.num_enemies];
    if (!from.enemies.empty()) from.enemies.pop_back();
}

// THE PLAYER KILL (recovery sec 1.5). Reached ONLY from the crossing handler,
// and NOT windowed by the step counter -- it sits after the shot window's skip
// label, so it is live for the whole crossing.
//
// It is not a distance test. Three conditions and a suppression:
//   * depth at or past the rim plane. The climb's one-sided clamp is what
//     makes this reachable at all: z undershoots the rim by up to one step and
//     then stops, so an arrived fuseball sits at a small NEGATIVE z rather
//     than on a knife edge of float equality at exactly 0;
//   * the level-end zoom not already running (the reference gates on its
//     wave timer holding the zoom sentinel; this port's equivalent is the
//     player's out_animation);
//   * the claw flagged vulnerable;
//   * the SAME LANE INDEX, exact integer compare -- no lateral geometry
//     anywhere;
//   * and |enemy.z - claw.z| <= 2 world-z, INCLUSIVE (the reference's compare
//     is a "greater than" skip, so the bound itself passes).
//
// On success the reference posts its own message and falls into the standard
// player-death path; here that is init_gameover. PHASE A, exactly as the
// flipper's grab is: the kill goes through the existing path, which is correct
// and boring. If the roster ever gains the two-phase drag, it is its own
// engine animation and nothing here changes.
bool tryGrab(GameEngine& engine, const enemyfam::EnemyCtx& ctx, int lane,
             const Enemy& enemy) {
    if (enemy.z > ArcadeFuseball::RimPlaneZ) return false;

    PlayerInfo& p = engine.player;
    if (p.out_animation != 0) return false;        // the level-end zoom
    if (p.gameover_animation > 0) return false;    // the claw is not vulnerable
    if (lane != p.grid_element_pos) return false;
    if (std::fabs(enemy.z - p.z) > ArcadeFuseball::GrabDz) return false;

    // "caught you" (id 16) -- this engine's own message for an enemy CATCHING
    // the player, which is the event here (enemies.cpp's shared
    // enemy-vs-player block raises the same id). JUDGEMENT CALL, and it is
    // presentation, therefore free under DOCTRINE.md: the flipper's grab chose
    // "shot you" (id 13) instead, so the two arcade families currently
    // announce a catch differently. Unify them in a presentation pass if that
    // reads badly; do not unify them by changing which event each one is.
    engine.init_gameover(ctx.time, 16);
    return true;
}

// ---------------------------------------------------------------------------
// CROSS SETUP. Runs on the tick the rail dwell expires (or, for a tanker
// hatchling, at construction) and does all of this atomically:
//   - resolves which LANE the crossing traverses, from the RAIL the fuseball
//     is parked on and the chosen direction;
//   - refuses if there is no rail that way, leaving the fuseball parked to
//     re-decide 49 reference ticks later. THAT REFUSAL IS THE REFERENCE'S OWN
//     IDIOM: its left-cross helper returns without changing state when there
//     is no lane 0-ward on an open web. This port applies the identical idiom
//     to the RIGHT edge, which the reference forgot -- see arcade_fuseball.h
//     "THE RAIL MODEL" for the two defects that omission causes there and why
//     naming rail `n` removes both;
//   - seeds the 16-step counter, the direction flag and the per-step delay;
//   - places the body on the correct END of that lane (cross_t 0 for a right
//     cross, 1 for a left one), which for a left cross is EXACTLY where the
//     fuseball is already standing -- so the motion vector is exact with no
//     repositioning, which is precisely why the reference pre-decrements the
//     lane index rather than adjusting the position;
//   - and TRANSFERS the enemy if the traversed lane is not the one it is
//     stored under, which is the case for every left cross out of an ordinary
//     park.
//
// Returns true iff the enemy was REMOVED from `lane` slot `idx`
// (enemy_family.h): true on a transfer, false on a refusal and false on a
// right cross out of an ordinary park, which needs no transfer.
// ---------------------------------------------------------------------------
bool beginCross(GameEngine& engine, const enemyfam::EnemyCtx& ctx, int lane,
                int idx, Enemy& enemy, int rail, int d) {
    using F = ArcadeFuseball;
    const int n = ctx.lane_count;
    if (n < 2) return false;

    int crossLane;
    if (d > 0) {
        // Right: traverse lane `rail`, from its LEFT border to its RIGHT.
        if (rail > n - 1) {
            // Only reachable on an OPEN web, from the clamped last rail: there
            // is no lane to the right of the final vertex. Refuse and
            // re-decide, exactly as the reference refuses at its own edge.
            return false;
        }
        crossLane = rail;
    } else {
        // Left: traverse lane `rail - 1`, from its RIGHT border to its LEFT.
        if (rail == 0) {
            if (!ctx.go_round) return false;   // the reference's own refusal
            crossLane = n - 1;                 // ...or wrap on a closed web
        } else {
            crossLane = rail - 1;
        }
    }
    if (crossLane < 0 || crossLane >= n) return false;   // defensive; unreachable

    // Capacity is checked BEFORE anything is committed, so a refusal leaves
    // the fuseball parked with nothing spent and it simply retries after the
    // next dwell. (This port's own MAX_ENEMIES cap; the reference has no
    // per-lane limit.)
    if (crossLane != lane && engine.grid[crossLane].num_enemies >= MAX_ENEMIES)
        return false;

    // ---- the atomic setup, on a COPY (it may be about to change vectors) ---
    Enemy moved = enemy;
    F::mode(moved)   = F::MODE_CROSS;
    F::dir(moved)    = (d > 0) ? +1 : -1;
    F::step(moved)   = F::StepStart;                       // 15
    F::timer(moved)  = F::CrossStepTicks;                  // no step this tick
    moved.cross_t    = F::crossTForStep(F::StepStart, d);  // 0 right, 1 left
    moved.anchor     = ANCHOR_RAIL;
    F::stamp(moved)  = ctx.time;                           // acts once per tick

    if (crossLane == lane) {
        enemy = moved;
        return false;
    }
    transferTo(engine, lane, idx, crossLane, moved);
    return true;
}

// ---------------------------------------------------------------------------
// CROSS LANDING. The 16th step has been taken; the crossing is over and the
// fuseball is standing on the next rail.
//
// The reference sets the mode back to climb FIRST, then either advances the
// lane index (right cross) or leaves it where the setup already put it (left
// cross), then re-snaps the position to that lane's LEFT border vertex. Both
// of those re-snaps are no-ops here by construction -- crossTForStep already
// returned exactly 1.0 or exactly 0.0 -- so what is left is only the lane
// bookkeeping.
//
// Returns true iff the enemy was REMOVED from `lane` slot `idx`.
// ---------------------------------------------------------------------------
bool landCross(GameEngine& engine, const enemyfam::EnemyCtx& ctx, int lane,
               int idx, Enemy& enemy) {
    using F = ArcadeFuseball;
    F::mode(enemy)  = F::MODE_CLIMB;
    F::timer(enemy) = F::RiseTicks;

    if (F::dir(enemy) < 0) {
        // LEFT: the index already stepped at setup, so the fuseball is parked
        // on this lane's own left border and there is nothing to move.
        enemy.cross_t = 0.0f;
        return false;
    }

    // RIGHT: the rail it reached is this lane's RIGHT border, which is the
    // NEXT lane's left border -- so advancing the index and re-snapping to
    // cross_t 0 is the same point expressed on the next lane.
    const int n = ctx.lane_count;
    int dst = -1;
    if (lane < n - 1)      dst = lane + 1;
    else if (ctx.go_round) dst = 0;

    if (dst < 0 || engine.grid[dst].num_enemies >= MAX_ENEMIES) {
        // THE CLAMP (arcade_fuseball.h "THE RAIL MODEL"), reached in exactly
        // two cases: the last rail of an OPEN web, which no lane index can
        // name; and a destination lane at this port's own MAX_ENEMIES cap.
        // Park on the rail actually reached, expressed on the lane the
        // fuseball is already stored under. Geometrically exact, on the web,
        // and self-healing -- the next crossing re-normalises it, and until
        // then parkedRail() reports the right rail so the direction test does
        // too. The reference instead leaves its index one lane out of sync
        // here, which costs it a visual pop and, on a further right cross,
        // walks the fuseball off the web entirely; the later port increments
        // out of range and reads the endpoint table past its end. Neither is
        // ported.
        enemy.cross_t = 1.0f;
        return false;
    }

    Enemy moved   = enemy;
    moved.cross_t = 0.0f;
    transferTo(engine, lane, idx, dst, moved);
    return true;
}

// ---------------------------------------------------------------------------
// STATE 0 -- CLIMB. Descend in place; when the dwell expires, pick a side.
// ---------------------------------------------------------------------------
bool climbTick(GameEngine& engine, const enemyfam::EnemyCtx& ctx, int lane,
               int idx, Enemy& enemy) {
    using F = ArcadeFuseball;

    // NO COLLISION CALL OF ANY KIND. Not a bullet test, not a player test, not
    // an alien-fire tick (the roster's one global fire timer has exactly three
    // call sites and none of them is here -- see enemies_shared.h sec 6). The
    // ABSENCE is the behaviour: headline fact 3.

    // DEPTH ONLY EVER MOVES HERE, and the gate is one-sided ON PURPOSE: it is
    // tested BEFORE the step, so z may undershoot the rim plane by at most one
    // step and then stops. That undershoot is what makes the player-kill
    // test's `z <= rim` reachable once the fuseball has arrived. Clamping to
    // exactly 0 instead would put a lethal test on float equality.
    if (enemy.z >= F::RimPlaneZ) enemy.z -= F::descentDz(engine.current_level);

    float& t = F::timer(enemy);
    t -= 1.0f;
    if (t > 0.0f) return false;

    // The dwell is RELOADED BEFORE the direction is chosen, so a cross that is
    // refused at a web edge still costs the fuseball a full dwell -- it
    // re-decides 49 reference ticks later, it does not retry next tick.
    //
    // `=` and not `+=`, unlike the cross delay: see the two-reloads note at the
    // top of this file. Measured, this makes every dwell exactly 44 engine
    // ticks rather than alternating 43/44.
    t = F::RiseTicks;

    const int rail = F::parkedRail(engine, lane, enemy);
    return beginCross(engine, ctx, lane, idx, enemy, rail,
                      F::chooseDir(engine, rail));
}

// ---------------------------------------------------------------------------
// STATE 1 -- CROSS. Traverse one lane at frozen depth, in 16 exact steps.
// ---------------------------------------------------------------------------
bool crossTick(GameEngine& engine, const enemyfam::EnemyCtx& ctx, int lane,
               int idx, Enemy& enemy) {
    using F = ArcadeFuseball;

    // 1. THE SHOT TEST is deliberately NOT here. It is the shared
    //    `enemy_shootable()` predicate (enemies_shared.h), which reads this
    //    family's step counter through shootable(); arcade_enemies.md sec 3.5
    //    keeps the sweeps themselves shared, and the recovered window is a
    //    property of the enemy, not of the sweep.
    //
    // 2. THE PLAYER TEST -- live for the WHOLE crossing, windowed by nothing.
    //    Runs BEFORE the step, matching the reference's order, so the lane and
    //    depth it tests are the ones the player can see.
    if (tryGrab(engine, ctx, lane, enemy)) return false;

    // 3. THE PER-STEP DELAY. fuse_crossdelay is 1, so the reference's
    //    countdown is 1, 0, -1: one step every TWO reference ticks. Converted
    //    to wall clock that is 1.779 engine ticks per step, which is not an
    //    integer -- so the residue is CARRIED (`+=`, not `=`) and the 16 steps
    //    land on a Bresenham-even 2/2/2/1 cadence totalling 28.5 ticks rather
    //    than drifting 12% long. See arcade_fuseball.h for why the doc's "4"
    //    is wrong.
    float& t = F::timer(enemy);
    t -= 1.0f;
    if (t > 0.0f) return false;
    t += F::CrossStepTicks;

    // 4. ONE EXACT STEP. The reference adds a 16.16 motion vector equal to the
    //    lane's border-to-border delta divided by 16 and then decrements the
    //    counter; here the position is DERIVED from the counter instead, which
    //    is the same sequence of points with no accumulation to drift and no
    //    second definition of where the lane's borders are.
    int& s = F::step(enemy);
    s -= 1;
    enemy.cross_t = F::crossTForStep(s, F::dir(enemy));
    if (s >= 0) return false;

    // 5. THE 16TH STEP LANDED. The crossing is over.
    return landCross(engine, ctx, lane, idx, enemy);
}

} // namespace

// ============================================================================
// update
// ============================================================================

bool ArcadeFuseball::update(GameEngine& engine, const EnemyCtx& ctx,
                            int lane, int idx, Enemy& enemy) {
    // ONE ACT PER TICK. move_enemies walks lanes 0..n-1, so a fuseball this
    // family moves to a HIGHER lane index -- which a right-cross landing does
    // -- is visited again in the same frame and would take a second action.
    // See arcade_fuseball.h; the flipper carries the identical guard.
    if (stamp(enemy) == ctx.time) return false;
    stamp(enemy) = ctx.time;

    // Already killed THIS tick by move_shots (slot 322, before move_enemies at
    // 324): take no further action. Nothing in this family depends on it --
    // there is no corpse to place and no children to release -- but a dead
    // fuseball must not still take the player, and the shared enemy-vs-player
    // block this family replaces guards on `enemy.life > 0` for exactly that
    // reason.
    if (enemy.life <= 0) return false;

    // ---- ARRIVAL: the far dot ------------------------------------------
    // Inert, invulnerable, no spin, no thinking. The reference does not
    // dispatch the fuseball's own update during this phase at all -- the
    // shared object runner closes the distance and returns -- so everything
    // below this block is deliberately skipped while it lasts.
    if (mode(enemy) == MODE_ARRIVAL) {
        enemy.z -= ArrivalDz;
        if (enemy.z > FarPlaneZ) return false;
        // The reference lands exactly on the far plane on its 150th tick and
        // dispatches the real update THAT SAME TICK. 133.43 engine ticks is
        // not an integer, so the exact landing has to be a clamp -- a port
        // decision forced by the 70.26 -> 62.5 Hz conversion, worth at most
        // 0.04 z.
        enemy.z      = FarPlaneZ;
        mode(enemy)  = MODE_CLIMB;
        timer(enemy) = RiseTicks;
        // ...and it begins climbing ON THAT SAME TICK: fall through.
    }

    // ---- the body spin, every live tick, in EVERY mode -------------------
    // A fuseball looks IDENTICAL parked and crossing. That is not an
    // oversight to be improved on: the player cannot see when it is
    // shootable, and that is the enemy. One revolution per 113.9 ticks.
    float& a = spin(enemy);
    a += SpinDegPerTick;
    if (a >= 360.0f) a -= 360.0f;   // one subtract: the step is far under 360

    // ---- the two live states --------------------------------------------
    // A two-way branch, and a fuseball holding any foreign mode value climbs
    // rather than freezing -- freezing would pin the level open forever, since
    // this enemy never lands and never expires.
    if (mode(enemy) == MODE_CROSS)
        return crossTick(engine, ctx, lane, idx, enemy);
    return climbTick(engine, ctx, lane, idx, enemy);
}

// ============================================================================
// Construction
// ============================================================================

namespace {

// The fields every fuseball starts with, whatever made it.
void initCommon(Enemy& e, float z) {
    using F = ArcadeFuseball;
    e      = Enemy{};
    e.id   = ARCADE_FUSEBALL;
    e.z    = z;
    e.life = ENEMY_LIFE[ARCADE_FUSEBALL];   // 1 -- one hit anywhere kills

    // THE mush_flag TRAP (enemy_pipeline_audit.md sec 3, the highest-value
    // finding in that document). `mush_flag` is NAMED as the mushroom part
    // bitmask and is not used that way: four shared sites read bit 1 as a
    // general-purpose "takes direct damage / does not breathe" gate, and an
    // inline-constructed Enemy inherits 0 -- which would make a fuseball take
    // damage from an upgraded shot only 1 roll in 11 (reading as a bullet
    // sponge, the exact opposite of the one-hit rule) and would give it this
    // engine's breathing wobble and 2.25 z-stretch, which arcade_enemies.md
    // sec 5.3 forbids for arcade enemies. 15 is exactly what
    // GameEngine::init_enemy writes, so no shared path can tell a hand-built
    // fuseball from a constructor-built one. REPLACE THIS with the audit's
    // named predicates when that lands as its own change; do not spread
    // `= 15` to more spawn sites.
    e.mush_flag = 15;

    // IT RIDES RAILS. ANCHOR_RAIL from birth to death, parked and crossing
    // alike -- models.h names this anchor for exactly this family, and
    // web_geometry.h's enemyAnchorPos already resolves it as
    // left + (right - left) * cross_t, which is the whole position model.
    e.anchor     = ANCHOR_RAIL;
    e.cross_t    = 0.0f;
    e.pivot_side = -1;                      // unused; kept at models.h's default

    F::mode(e)   = F::MODE_ARRIVAL;
    F::step(e)   = F::StepStart;
    F::dir(e)    = +1;
    F::timer(e)  = F::RiseTicks;
    F::stamp(e)  = INT32_MIN;               // "never updated"
    F::spin(e)   = 0.0f;
}

} // namespace

bool ArcadeFuseball::spawn(GameEngine& engine, int lane) {
    if (lane < 0 || lane >= engine.lane_count) return false;
    GridElement& elem = engine.grid[lane];
    // The reference refuses when no object slot is free and costs exactly ONE
    // slot (contrast the tanker's three). This port's equivalent is the
    // per-lane cap; the roster-wide budget is ArcadeTanker::budgetInPlay(),
    // which already charges every arcade id 1 through arcadeCost(), so a
    // fuseball counts against it with no change there.
    if (elem.num_enemies >= MAX_ENEMIES) return false;

    Enemy e;
    // UNIFORM OVER THE WHOLE WEB, both end lanes included: unlike the tanker,
    // whose end-lane exclusion exists to keep its two children in distinct
    // adjacent lanes, a fuseball has no children. The caller's random lane is
    // that rule, so it is used as given.
    initCommon(e, SpawnZ);

    elem.enemies.push_back(e);
    elem.num_enemies += 1;
    engine.enemies_nums[ARCADE_FUSEBALL] += 1;

    // AUDIT sec 8, AND IT IS THE ONE THAT HANGS THE LEVEL IF IT IS MISSED:
    // enemies_todo[] is decremented by the SHARED machinery in exactly one
    // place -- init_embrio -- and the arcade arrival replaces the embryo
    // entirely. A release that never decrements it leaves is_level_clear()
    // false forever with nothing on screen, and the symptom points nowhere
    // near the cause. The release point IS the spawn for this set, so every
    // arcade family decrements its own here (see the accounting rule above
    // arcade_release in enemies_shared.h) -- which means the caller must NOT
    // also decrement it.
    if (engine.enemies_todo[ARCADE_FUSEBALL] > 0)
        engine.enemies_todo[ARCADE_FUSEBALL] -= 1;
    return true;
}

bool ArcadeFuseball::spawnHatchling(GameEngine& engine, const EnemyCtx& ctx,
                                    int parentLane, float z, int d) {
    const int n = ctx.lane_count;
    if (n < 3) return false;
    if (parentLane < 0 || parentLane >= n) return false;

    // THE HATCH GEOMETRY (recovery sec 2.2). The reference reaches it by
    // TEMPORARILY incrementing the parent's lane index around the first
    // child's construction, so that child computes its position from the left
    // border of lane+1 -- which IS the parent lane's right border -- and then
    // crosses further right; the second is built on the parent's own lane and
    // crosses further left. Net: the pair appears on the two boundary lines of
    // the tanker's lane and moves APART.
    //
    // Expressed in this file's rail model that is simply: child +1 parks on
    // rail parentLane+1 and crosses right; child -1 parks on rail parentLane
    // and crosses left. Both traverse a lane adjacent to the parent's, which
    // is what keeps the tanker's own `enemy` reference valid across the call
    // (its openChildren never pushes into its own lane).
    //
    // The tanker's end-lane restriction guarantees both are in range on every
    // web; the bounds tests below exist because this port's OTHER release path
    // (the classic embryo, if an arcade tanker ever reaches the field through
    // it) does not honour that restriction.
    int crossLane;
    if (d > 0) {
        int r = parentLane + 1;
        if (r > n - 1) {
            if (!ctx.go_round) return false;
            r -= n;
        }
        crossLane = r;
    } else {
        if (parentLane == 0) {
            if (!ctx.go_round) return false;
            crossLane = n - 1;
        } else {
            crossLane = parentLane - 1;
        }
    }
    if (crossLane == parentLane) return false;   // would push into the caller's vector
    if (crossLane < 0 || crossLane >= n) return false;

    GridElement& elem = engine.grid[crossLane];
    if (elem.num_enemies >= MAX_ENEMIES) return false;

    Enemy e;
    // DEPTH IS COPIED FROM THE PARENT VERBATIM -- no far dot, no approach
    // phase. That is the single biggest difference between the two creation
    // paths, and it is why a hatched pair is dangerous immediately while a
    // wave-spawned fuseball spends 2.1 s as a dot.
    initCommon(e, z);
    // ...and it starts MID-CROSSING rather than parked: the reference calls
    // the cross setup immediately after construction, so the child's very
    // first tick is a crossing tick.
    mode(e)   = MODE_CROSS;
    dir(e)    = (d > 0) ? +1 : -1;
    step(e)   = StepStart;
    timer(e)  = CrossStepTicks;
    e.cross_t = crossTForStep(StepStart, d);   // 0 for right, 1 for left
    // The tanker calls this from inside its own update, so the child must NOT
    // also act on this tick.
    stamp(e)  = ctx.time;

    elem.enemies.push_back(e);
    elem.num_enemies += 1;
    engine.enemies_nums[ARCADE_FUSEBALL] += 1;
    // NOT charged against enemies_todo[]: a hatchling is the tanker's payload,
    // not one of the level's planned releases. Charging it would steal two
    // level-scheduled fuseballs per tanker and, once that counter reached 0,
    // drive it NEGATIVE -- which is_level_clear()'s `> 0` test reads as
    // "clear", MASKING the error rather than reporting it.
    //
    // What a hatchling also does NOT get, all four confirmed absences: no
    // promotion roll (the flipper hatch has one; this one does not), no "stop
    // after one move" mark (likewise), no colour inheritance (the child's own
    // colour is written explicitly and is in any case invisible under the
    // five-slot rosette), and no slot accounting of its own -- the tanker
    // reserved three units when it spawned.
    return true;
}

} // namespace enemyfam
} // namespace ts
