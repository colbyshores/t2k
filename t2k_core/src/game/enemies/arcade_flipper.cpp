// ============================================================================
// arcade_flipper.cpp -- the arcade FLIPPER family. See arcade_flipper.h for the
// contract, the field map, and the two verifier corrections.
//
// Spec: docs/design/arcade_enemies.md sec 2.1 (read against the header's
// adjudication table). Shared-path map: docs/design/enemy_pipeline_audit.md.
//
// STATE MACHINE, one line each:
//   ARRIVAL  inert 2x2 dot, invulnerable, closes 0.35131 z/tick from z=71.875
//            to the far plane, then RAIL (or STOPPED for a super-3).
//   RAIL     walks one lane down at the level-scaled descent speed; lane and
//            model angle never change; at the rim it clamps to z=0 and runs
//            flip-setup ON THAT SAME TICK.
//   FLIP     hinged on the border vertex shared with the destination lane; the
//            lane index ALREADY stepped to the destination, only an angle
//            moves. Non-monotonic vulnerability; grabs on the SOURCE lane.
//   STOPPED  parked at the lane midpoint; marker branch, forced spin, the
//            same-lane grab, then the pause countdown fires the next flip.
//
// ...and ONE branch off the front of RAIL, which is the entire Beast: see
// beastRail() below and arcade_flipper.h's SUB_BEAST block.
// ============================================================================

#include "arcade_flipper.h"

#include "enemies_shared.h"   // arcade_alien_fire_tick -- the RAIL fire site
#include "arcade_mirror.h"    // ArcadeReflectedShot -- the SHARED rail the
                              // Beast's shed horn and the Mirror's ring are
                              // both instances of. Included in the .cpp and
                              // not the header on purpose: arcade_flipper.h is
                              // pulled in by enemies_shared.h, and nothing
                              // there needs the reflected shot.
#include "../web_geometry.h"
#include "../math_lut.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace ts {
namespace enemyfam {

namespace {

constexpr float RAD2DEG = 180.0f / 3.14159265358979323846f;

// ---------------------------------------------------------------------------
// THE RAIL DESCENT TABLE -- the ONLY level-scaled quantity on this family.
//
// The original indexes a 56-entry table by `wave >> 1` (consecutive wave PAIRS
// share a value) and shifts it left 5 into a 16.16 z step. The recovery
// (arcade_enemies.md sec 2.1 S0 / sec 1.4) captured only its ENDPOINTS:
//     wave 0-1   0.625 world-z/tick -> 0.10979 engine z/tick (227.7 ticks)
//     wave 110+  0.867 world-z/tick -> 0.15231 engine z/tick (164   ticks)
// so the interior is RE-AUTHORED as a monotone linear ramp that is exact at
// both ends. This is the one place in this family carrying numbers the
// recovery did not supply, and it is named as such for the verifier: the real
// table is more likely piecewise-flat (the spiker's, which WAS recovered, runs
// 26,26,28,30,32,34,36,38 / 40,40,42,42,44,44,44,44 / ...). Nothing else in
// the family depends on the shape of the interior.
// ---------------------------------------------------------------------------
constexpr std::array<float, ArcadeFlipper::RailTableSize> makeRailTable() {
    std::array<float, ArcadeFlipper::RailTableSize> t{};
    constexpr int last = ArcadeFlipper::RailTableSize - 1;
    for (int i = 0; i <= last; ++i) {
        t[i] = ArcadeFlipper::RailDzWave0
             + (ArcadeFlipper::RailDzWaveMax - ArcadeFlipper::RailDzWave0)
               * ((float)i / (float)last);
    }
    return t;
}
constexpr std::array<float, ArcadeFlipper::RailTableSize> RAIL_DZ = makeRailTable();

static_assert(RAIL_DZ[0] == ArcadeFlipper::RailDzWave0, "rail table endpoint");
static_assert(RAIL_DZ[ArcadeFlipper::RailTableSize - 1] == ArcadeFlipper::RailDzWaveMax,
              "rail table endpoint");

inline float normalize360(float deg) {
    deg = std::fmod(deg, 360.0f);
    if (deg < 0.0f) deg += 360.0f;
    return deg;
}

// The lane's own direction, in degrees. Read through webLane so this family
// cannot invent a fourth definition of "where a lane is" (web_geometry.h);
// f.dx/f.dy are grid[i].dx/dy, the STORED step, which is the true lane vector
// on every web (Tsunami rows unit-length, Typhoon rows deliberately not).
inline float laneAngleDeg(const GameEngine& e, int lane) {
    const LaneFrame f = webLane(e, lane);
    // fastAtan2: one divide + one interpolated read (<=1.5e-6 rad) replaces
    // the dearer libm atan2; feeds the flip animation wedge, not mechanics.
    return fastAtan2(f.dy, f.dx) * RAD2DEG;
}

// Wrap a lane step. Closed webs wrap; open webs are the caller's problem (the
// direction chooser has already bounced off both ends).
inline int stepLane(const GameEngine& e, int lane, int dir) {
    int d = lane + dir;
    if (e.grid_level_go_round) {
        const int n = e.lane_count;
        d = ((d % n) + n) % n;
    }
    return d;
}

} // namespace

// ===========================================================================
// Level scaling
// ===========================================================================

float ArcadeFlipper::railDescent(int wave) {
    if (wave < 0) wave = 0;
    int i = wave >> 1;
    if (i > RailTableSize - 1) i = RailTableSize - 1;
    return RAIL_DZ[i];
}

int ArcadeFlipper::promoteLevelSuper2Num(int wave) {
    // 2 * max(wave - 16, 0) / 256 -- ZERO below wave 16.
    int w = wave - 16;
    return (w > 0) ? 2 * w : 0;
}

int ArcadeFlipper::promoteHatchSuper2Num(int wave) {
    // 4 * min(wave, 63) / 256
    int w = (wave < 0) ? 0 : wave;
    if (w > 63) w = 63;
    return 4 * w;
}

int ArcadeFlipper::promoteSuper3Num(int wave) {
    // 2 * min(wave, 127) / 256
    int w = (wave < 0) ? 0 : wave;
    if (w > 127) w = 127;
    return 2 * w;
}

// ===========================================================================
// Web geometry
// ===========================================================================

// Character-for-character entity_geometry.cpp `clawNormalSign`. Duplicated
// ONLY because that copy is `static` inside a rendering TU; the flipper and
// the claw must never disagree about which side of a lane is "inside the
// tube", so if one of these changes the other has to. Hoist both into
// web_geometry.h in a follow-up.
float ArcadeFlipper::webWindingSign(const GameEngine& engine) {
    if (!engine.grid_level_go_round) return 1.0f;
    float x = 0.0f, y = 0.0f, area2 = 0.0f, perim = 0.0f;
    for (int i = 0; i < engine.lane_count; i++) {
        const float dx = engine.grid[i].dx, dy = engine.grid[i].dy;
        const float nx = x + dx, ny = y + dy;
        area2 += x * ny - nx * y;
        perim += std::sqrt(dx * dx + dy * dy);
        x = nx; y = ny;
    }
    if (perim <= 1e-6f) return 1.0f;
    if (std::fabs(area2) / (perim * perim) < 0.01f) return 1.0f;  // self-crossing
    return (area2 < 0.0f) ? -1.0f : 1.0f;
}

// ---------------------------------------------------------------------------
// THE FLIP MAGNITUDE. The body is pinned to the border vertex the two lanes
// share and rotates until it lies along the destination lane -- so the
// rotation is the one that carries the far end of the source lane onto the far
// end of the destination lane, and the branch it takes is the web's INTERIOR
// wedge at that vertex.
//
// That is not a guess: it is the only reading under which all three magnitudes
// the recovery quotes fall out of one rule (arcade_enemies.md sec 2.1 S1
// "Duration is NOT constant" + "S1 VULNERABILITY"):
//
//     collinear join     interior 180.0  -> 512/1024 turn, 32 the later port ticks
//     16-gon corner      interior 157.5  -> 448/1024 turn, 28 the later port ticks
//     reflex 135 corner  interior 315.0  -> 896/1024 turn, 56 the later port ticks
//
// and under which the recovered vulnerability threshold is EXACT: a flip
// exceeding 286.875 deg is a vertex whose interior angle exceeds 286.875, i.e.
// whose lane direction turns by more than 106.875 deg -- the spec's "only at
// reflex/spike corners, where the lane direction turns 107-180 deg".
//
// The +-SIGN is a property of the web's WINDING, which is exactly what the
// spec says this port must derive from its own lane vectors rather than
// inherit from the later port web data's convention. On a counter-clockwise ring the
// left normal points into the tube, so a flip toward a HIGHER lane index
// sweeps clockwise and a flip toward a LOWER index sweeps counter-clockwise;
// on a clockwise ring (this port has one: Tsunami web 11, signed area -14.0)
// both mirror.
// ---------------------------------------------------------------------------
float ArcadeFlipper::flipRotation(const GameEngine& engine, int srcLane,
                               int dstLane, int dir) {
    const float thS = laneAngleDeg(engine, srcLane);
    const float thD = laneAngleDeg(engine, dstLane);

    // The ray from the HINGE to the body's far end, before and after.
    //   dir>0: hinge is the source lane's RIGHT vertex; the body's far end is
    //          the source lane's LEFT vertex (at thS+180 from the hinge) and
    //          must land on the destination lane's far end (at thD).
    //   dir<0: mirror -- hinge is the source lane's LEFT vertex.
    const float phiStart = (dir > 0) ? (thS + 180.0f) : thS;
    const float phiEnd   = (dir > 0) ? thD            : (thD + 180.0f);

    const float ccw = normalize360(phiEnd - phiStart);
    const bool  ccwRing = (webWindingSign(engine) > 0.0f);
    // Which of the two arcs is the interior wedge.
    const bool  takeCcw = ccwRing ? (dir < 0) : (dir > 0);

    float mag = takeCcw ? ccw : (360.0f - ccw);
    // A lane pair that doubles straight back on itself pinches the wedge to
    // zero (Tsunami web 31 lanes 0/1 are (0,+1) then (0,-1)). Zero rotation
    // would resolve the flip with no visible motion at all; a full tumble is
    // the readable answer and is what the other branch already gives.
    if (mag < MinFlipDeg) mag = 360.0f;
    return takeCcw ? mag : -mag;
}

// ===========================================================================
// S1b DIRECTION CHOICE (once per flip; CONFIRMED in every branch, sec 2.1)
// ===========================================================================

int ArcadeFlipper::chooseDir(const GameEngine& engine, int lane, int subVariant) {
    const int n = engine.lane_count;
    if (n < 2) return 0;

    const int delta = lane - engine.player.grid_element_pos;
    if (delta == 0) return 0;                    // park in STOPPED, no flip
    int dir = (delta > 0) ? -1 : +1;             // toward the player

    // Closed web: take the SHORT way round. Arithmetic shift, and the
    // exactly-half case falls to the REVERSED direction.
    if (engine.grid_level_go_round && std::abs(delta) >= (n >> 1))
        dir = -dir;

    // Super-flipper-3 INVERTS the result while still descending: it
    // deliberately FLEES the player until it reaches the rim.
    if (subVariant == SUB_SUPER3_DIVE)
        dir = -dir;

    // Open web: asking to step off either end falls through to the opposite
    // direction. It BOUNCES; it does not wrap.
    if (!engine.grid_level_go_round) {
        if (lane + dir < 0 || lane + dir > n - 1) dir = -dir;
        if (lane + dir < 0 || lane + dir > n - 1) return 0;
    }
    return dir;
}

// ===========================================================================
// Vulnerability and threat -- THE TWO CORRECTIONS
// ===========================================================================

float ArcadeFlipper::remaining(const Enemy& e) {
    const float r = std::fabs(total(e)) - std::fabs(turned(e));
    return (r > 0.0f) ? r : 0.0f;
}

bool ArcadeFlipper::shootable(const Enemy& enemy) {
    switch (mode(enemy)) {
    case MODE_ARRIVAL:
        // The dot has NO collision test of any kind in the original: bullets,
        // the superzapper and (audit Z4) the tremor all pass straight through.
        return false;
    case MODE_FLIP: {
        // CORRECTION (a). The original divides the remaining rotation by 4 and
        // truncates to a SIGNED BYTE -- i.e. reduces it to the SHORTEST SIGNED
        // ANGLE, not the remaining travel -- and is invulnerable only while
        // that magnitude exceeds 52/256 of a turn. Because of the wrap, a flip
        // longer than 286.875 deg OPENS with a brief shootable window.
        const float a = normalize360(remaining(enemy));
        const float shortest = (a > 180.0f) ? (a - 360.0f) : a;
        return std::fabs(shortest) <= VulnDeg;
    }
    default:
        return true;   // RAIL and STOPPED: one hit anywhere kills.
    }
}

bool ArcadeFlipper::zappable(const Enemy& enemy) {
    // Killable by the super zapper / tremor in EVERY mode except the inert
    // arrival dot. The original short-circuits run_flipper on _sz and kills the
    // flipper regardless of the mid-flip vulnerability window (see the header),
    // so that window must not gate the area weapons here -- only the far 2x2
    // arrival dot stays out.
    return mode(enemy) != MODE_ARRIVAL;
}

int ArcadeFlipper::threatLane(const Enemy& enemy, int lane) {
    const int m = marker(enemy);
    // Both grab entry points require a NON-NEGATIVE marker (sec 2.1 S3). A
    // tanker hatchling carries -1 (or -2) until its first flip lands and
    // therefore cannot grab at all.
    if (m < 0) return -1;

    switch (mode(enemy)) {
    case MODE_ARRIVAL:
        return -1;                       // inert
    case MODE_RAIL:
        // Flippers never fire and never grab while railing: sec 2.1 S0 lists
        // bullet / alien-fire / descend / rim and nothing else, and sec 2.5
        // confirms the rail is a FIRE site, not a grab site.
        return -1;
    case MODE_FLIP:
        // CORRECTION (b). It grabs ONLY during the invulnerable phase, and it
        // grabs in the lane it is flipping OUT OF -- the lane its body still
        // visually occupies while pivoting around the shared border -- NOT the
        // destination lane it has already been indexed into. It threatens the
        // destination only once the flip resolves and the parked test runs on
        // the following tick.
        return shootable(enemy) ? -1 : m;
    default:
        return lane;                     // parked: the CURRENT lane
    }
}

// ===========================================================================
// Internal helpers
// ===========================================================================

namespace {

// The same-lane / depth-window grab, shared by the mid-flip and parked entry
// points. Returns true iff the player was taken.
//
// PHASE A (arcade_enemies.md sec 2.1 S3 "Implementation staging"): the kill goes
// through the existing init_gameover path -- correct, boring. PHASE B adds the
// two-phase drag (both bodies pushed 0.35131 z/tick deeper for 71.2 ticks,
// then a camera retreat for 35.6) as its own engine animation; do not block on
// it.
bool tryGrab(GameEngine& engine, const EnemyCtx& ctx, int lane, Enemy& enemy) {
    const int threat = ArcadeFlipper::threatLane(enemy, lane);
    if (threat < 0) return false;

    PlayerInfo& p = engine.player;
    // "...and are suppressed once the level-complete zoom has begun."
    if (p.out_animation != 0 || p.gameover_animation > 0) return false;
    if (threat != p.grid_element_pos) return false;
    // Parked window |dz| <= 2 world-z = 0.3125 engine z (ParkedGrabDz -- see
    // the header's adjudication note; the later port's 8 world-z / 1.25 was
    // shipped once and rejected). The mid-flip test uses
    // the same shared lane-comparison routine in the original, so it inherits
    // the same window here; it only ever binds for a super-flipper-3, which is
    // the one variant that flips while still deep in the tube.
    if (std::fabs(enemy.z - p.z) > ArcadeFlipper::ParkedGrabDz) return false;

    engine.init_gameover(ctx.time, 13);   // "shot you"
    return true;
}

// ---------------------------------------------------------------------------
// `beastrail` -- SUB-VARIANT 5, AND IT IS THE WHOLE OF THE BEAST'S OWN CODE.
//
// The reference dispatches the rail through a table of sub-variant handlers;
// slot 0 is the standard rail (which this file inlines) and slot 5 is this.
// Its entire body is: look for a bullet, and if there is not one, JUMP INTO
// THE STANDARD RAIL at its fire/descend label. So --
//
//     ON ANY TICK IT IS NOT HIT, A BEAST IS A PLAIN FLIPPER.
//
// -- which is why this returns a bool and the caller falls through rather than
// this function duplicating a line of the rail (recovery sec 5.3, sec 10 item
// 7). Returns true iff it WAS hit, in which case the Beast does nothing else
// at all this tick: no alien fire, no descent, no rim test. The reference's
// shed path returns rather than falling through, and that one skipped tick per
// hit is observable as a hitch in the descent.
//
// THE SHED HORN IS THE PROJECTILE. A non-fatal hit does not damage the Beast
// in any sense this engine would call damage -- it takes the bullet away and
// hands it back as the horn that just came off, spinning, at half speed. That
// is the readable tell that tells the player what they just did.
//
// THE KILLING SHOT IS NOT CONSUMED (recovery sec 1.5, sec 10 item 2): the
// death branch below jumps away BEFORE the take-over, so the bullet the
// non-destructive search found is left live and flies on up the web. Both the
// Beast's third hit and the Mirror's fifth behave this way and it is real,
// exploitable, and easy to get backwards.
// ---------------------------------------------------------------------------
bool beastRail(GameEngine& engine, int lane, Enemy& enemy) {
    using F = ArcadeFlipper;

    // The NON-DESTRUCTIVE bullet search -- the same reference helper the
    // Mirror calls, which finds a live player bullet in this lane within
    // |dz| < 6 world-z and deliberately does NOT kill it.
    Shot* bullet = ArcadeReflectedShot::findBullet(engine, lane, enemy.z,
                                                  F::BeastHitDz);
    if (bullet == nullptr) return false;      // ...so it is a plain flipper.

    // TEST THE COUNTER BEFORE DECREMENTING IT. 2 -> shed, 1 -> shed, 0 -> die.
    // Three hits, two sheds; see arcade_flipper.h BeastHorns.
    const int before = F::horns(enemy);
    if (before == 0) {
        // The flipper death, unchanged: 150 points, the shared explosion, the
        // shared capsule cadence, the shared removal. Expressed as life = 0
        // rather than a self-removal precisely so that it IS the flipper's
        // death -- move_enemies runs `_handle_death` on any enemy left at zero
        // life after its update, and that is the one path the whole roster's
        // scoring, bonus shed and powerup cadence go through.
        //
        // (Contrast the Mirror, which owns its own death: its score is rolled
        // per corpse and the shared path resolves an arcade score from the id
        // alone, with nowhere to put a per-corpse number.)
        enemy.life = 0;
        return true;
    }

    F::setHorns(enemy, before - 1);
    // WHICH horn: the up-left one first, the up-right one second. See
    // arcade_flipper.h BeastHornFirst/Second for why that is the arcade
    // reference's order even though it sheds the mirror image of the horn that
    // visibly vanishes.
    const int hornVariant = (before == F::BeastHorns) ? F::BeastHornFirst
                                                     : F::BeastHornSecond;
    // NO SOUND. The Mirror plays one on every non-fatal hit; the Beast plays
    // none, and the recovery states that as an explicit contrast.
    ArcadeReflectedShot::takeOver(engine, *bullet,
                                  ArcadeReflectedShot::KIND_BEAST, hornVariant);
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// FLIP-SETUP. Runs BEFORE the first tick of the flip and does all of this
// atomically (sec 2.1 S1):
//   - records the SOURCE lane in the grab field  <- correction (b)
//   - chooses a direction (S1b), or a FORCED one for a spinning hatchling
//   - moves the lane index to the DESTINATION
//   - moves world x/y onto the border vertex the two lanes SHARE -- the hinge
//   - moves the rotation origin from the body centre to that same edge
//   - sets the target angle to the destination lane's orientation
//   - sets the signed rate
//
// Returns the destination lane, or -1 if it parked instead (delta == 0, an
// open-web dead end, or a full destination lane).
//
// THE LANE MOVE IS A REAL TRANSFER between grid[] vectors, done here rather
// than through enemies.cpp `_transfer_enemy` because that helper is static and
// NEGATES animation_phase (audit R3), which on a flipper is the grab marker.
// `enemies_nums` is deliberately untouched: a transfer is not a removal.
// ---------------------------------------------------------------------------
static int flipSetup(GameEngine& engine, const EnemyCtx& ctx, int lane, int idx,
                     Enemy& enemy, int forcedDir) {
    using F = ArcadeFlipper;

    const int subVariant = F::sub(enemy);
    int dir = (forcedDir != 0) ? forcedDir
                               : F::chooseDir(engine, lane, subVariant);

    if (forcedDir != 0 && !engine.grid_level_go_round) {
        // A forced spin still "falls back to the opposite direction at an
        // open-web end" (sec 2.1 "Super stop").
        const int n = engine.lane_count;
        if (lane + dir < 0 || lane + dir > n - 1) dir = -dir;
        if (lane + dir < 0 || lane + dir > n - 1) dir = 0;
    }

    const int dst = (dir != 0) ? stepLane(engine, lane, dir) : lane;

    // Park: no direction, or the destination lane is full. (The full-lane case
    // is not in the recovery -- MAX_ENEMIES is this port's own cap -- and
    // parking is the safe reading: it retries next pause.)
    if (dir == 0 || dst == lane ||
        engine.grid[dst].num_enemies >= MAX_ENEMIES) {
        F::mode(enemy)   = F::MODE_STOPPED;
        F::pause(enemy)  = F::pauseReloadFor(subVariant);
        F::total(enemy)  = 0.0f;
        F::turned(enemy) = 0.0f;
        F::rate(enemy)   = 0.0f;
        enemy.anchor     = ANCHOR_LANE_MID;
        return -1;
    }

    // ---- the atomic setup, on a COPY (it is about to change vectors) -------
    Enemy moved = enemy;
    F::marker(moved) = lane;                    // the SOURCE lane
    F::mode(moved)   = F::MODE_FLIP;
    const float rot  = F::flipRotation(engine, lane, dst, dir);
    F::total(moved)  = rot;
    F::turned(moved) = 0.0f;
    // "sets the signed rate" -- CAPTURED HERE, not re-read per tick. That is
    // load-bearing for the tanker hatch: the initial flip setup runs BEFORE
    // the promotion writes the sub-variant, so THE FIRST FLIP OF A
    // TANKER-BORN SUPER RUNS AT THE PLAIN RATE (sec 2.1, observable).
    F::rate(moved)   = (rot >= 0.0f ? 1.0f : -1.0f) * F::rateFor(subVariant);
    // The hinge: the border vertex the two lanes share, expressed on the
    // DESTINATION lane. dir>0 -> the destination's LEFT vertex; dir<0 -> its
    // RIGHT. web_geometry.h enemyAnchorPos resolves it.
    moved.anchor     = ANCHOR_PIVOT;
    moved.pivot_side = (dir > 0) ? -1 : +1;
    F::stamp(moved)  = ctx.time;                // acts once per tick

    // ---- transfer, with enemies.cpp's swap-down semantics ------------------
    GridElement& from = engine.grid[lane];
    GridElement& to   = engine.grid[dst];
    to.enemies.push_back(moved);
    to.num_enemies += 1;

    from.num_enemies -= 1;
    if (idx < from.num_enemies)
        from.enemies[idx] = from.enemies[from.num_enemies];
    if (!from.enemies.empty())
        from.enemies.pop_back();

    return dst;
}

// The flip has resolved: mode 2, rotation origin cleared, position snapped to
// the destination lane's MIDPOINT, angle reset to that lane's orientation (an
// invisible 180 deg jump, because the body is symmetric).
static void flipLand(GameEngine& engine, Enemy& enemy) {
    using F = ArcadeFlipper;
    F::mode(enemy)   = F::MODE_STOPPED;
    F::pause(enemy)  = F::pauseReloadFor(F::sub(enemy));
    F::total(enemy)  = 0.0f;
    F::turned(enemy) = 0.0f;
    F::rate(enemy)   = 0.0f;
    enemy.anchor     = ANCHOR_LANE_MID;
    enemy.pivot_side = -1;                       // rotation origin cleared
    // "flip sound plays unless the level is ending".
    if (engine.player.out_animation == 0)
        engine.sfx.push(ArcadeFlipper::FlipSfx, SfxAction::ONE_SHOT,
                        ArcadeFlipper::FlipSfxPitch);
}

// ===========================================================================
// update
// ===========================================================================

bool ArcadeFlipper::update(GameEngine& engine, const EnemyCtx& ctx,
                        int lane, int idx, Enemy& enemy) {
    // ONE ACT PER TICK. move_enemies walks lanes 0..n-1, so an enemy this
    // family transfers to a HIGHER lane index is visited again in the same
    // frame and would take a second flip step. See arcade_flipper.h.
    if (stamp(enemy) == ctx.time) return false;
    stamp(enemy) = ctx.time;

    // ---- S-1 ARRIVAL ----------------------------------------------------
    if (mode(enemy) == MODE_ARRIVAL) {
        // Inert, invulnerable, no fire, no rotation. It DOES still participate
        // in the depth scan, which in this port falls out for free because
        // is_level_clear() counts grid[].num_enemies.
        enemy.z -= ArrivalDz;
        if (enemy.z > FarPlaneZ) return false;
        // "the depth lands exactly on the far plane": clamp, so the rail
        // always starts from z = 25 with no sub-tick residue.
        enemy.z = FarPlaneZ;
        // A level-spawned super-flipper-3 NEVER USES THE RAIL STATE: it spawns
        // (past the dot) straight into STOPPED with a -1 pause, so it flips on
        // its first live tick and every tick thereafter, and does its
        // descending inside the flip handler.
        if (sub(enemy) == SUB_SUPER3_DIVE) {
            mode(enemy)  = MODE_STOPPED;
            pause(enemy) = pauseReloadFor(SUB_SUPER3_DIVE);
        } else {
            mode(enemy) = MODE_RAIL;
        }
        // ...and it begins that mode ON THAT SAME TICK: fall through.
    }

    // ---- S0 RAIL --------------------------------------------------------
    if (mode(enemy) == MODE_RAIL) {
        //   0. THE RAIL SUB-VARIANT DISPATCH. The reference indexes a table of
        //      handlers here; this port branches, because the table has
        //      exactly two entries this port can reach (0 = the standard rail,
        //      inlined below; 5 = the Beast) and a function-pointer table
        //      indexed per enemy per tick is precisely what the perf doctrine
        //      forbids (enemy_family.h).
        //
        //      ANY OTHER SUB-VARIANT FALLS THROUGH TO THE STANDARD RAIL, which
        //      is the safe answer and matters for one slot in particular: the
        //      reference's slot 1 is a versus-mode rail this port has no
        //      equivalent of, and the recovery flags (its sec 7.2) that an
        //      unguarded dispatch would make it silently behave as a plain
        //      flipper. Here that is the DEFINED behaviour rather than an
        //      accident, and nothing can construct one anyway.
        if (sub(enemy) == SUB_BEAST && beastRail(engine, lane, enemy))
            return false;   // hit: shed or died, and it acts no further this
                            // tick. Not removed -- a death is `life = 0` and
                            // move_enemies' own `_handle_death` takes it.
        // Per tick, in this exact order (the ordering is load-bearing):
        //   1. BULLET COLLISION -- SHARED, and already correctly ordered
        //      around this call: collision.cpp move_shots is tick slot 322 and
        //      runs BEFORE move_enemies (324), and enemies.cpp's catch-up
        //      sweep runs immediately after this function returns.
        //   2. THE SHARED ALIEN-FIRE TICK (sec 2.5) -- one global timer that
        //      every railing enemy decrements, so the fleet-wide fire rate
        //      scales with the number of descenders. It is PER-SET state, not
        //      this family's: it lives on GameEngine as `arcade_fire_timer`
        //      (cleared in init_level) and the spiker's climb and descend are
        //      the other two of its exactly three call sites. The POLICY is
        //      alienFireTick() below, because this is where it was recovered;
        //      the free function is enemies_shared.h's, so all three sites
        //      spell the call the same way. It fires BEFORE the descent step,
        //      so the bullet is born at the z the shooter is leaving.
        arcade_alien_fire_tick(engine, lane, enemy.z);
        //   3. subtract the level's descent speed
        enemy.z -= railDescent(engine.current_level);
        //   4. on reaching the rim plane: clamp z to 0, zero the z fraction,
        //      set mode 1, and run flip-setup ON THE SAME TICK.
        if (enemy.z > RimPlaneZ) return false;
        enemy.z = RimPlaneZ;
        mode(enemy) = MODE_FLIP;
        return flipSetup(engine, ctx, lane, idx, enemy, 0) >= 0;
    }

    // ---- S1 FLIP --------------------------------------------------------
    if (mode(enemy) == MODE_FLIP) {
        // Super-flipper-3's descent happens INSIDE the flip handler: the
        // sub-variant-3 branch subtracts the same rail speed per flip tick.
        // It therefore costs one non-descending tick per flip (the STOPPED
        // tick on which flip-setup runs), which is why it falls very slightly
        // slower than a plain flipper.
        if (sub(enemy) == SUB_SUPER3_DIVE) {
            enemy.z -= railDescent(engine.current_level);
            if (enemy.z <= RimPlaneZ) {
                // CORRECTED: the 3 -> 4 promotion does NOT clamp z to the rim
                // plane, unlike the rail path, so a super-flipper-3 overshoots
                // the rim by up to one frame's worth of z. Carry the
                // overshoot; it is a real, visible tell.
                //
                // NB the SHARED _apply_movement floors every enemy at
                // -ENEMY_DZ[id], which is exactly 0 once the descriptor row is
                // 0 (see the report) -- so the overshoot survives only if the later port
                // ids are also exempted from that path (audit M1).
                sub(enemy) = SUB_SUPER3_SEEK;
            }
        }

        // The grab test runs on the PRE-STEP remaining, so the vulnerability
        // window and the grab window are read from the same number on the same
        // tick. (The original's ordering inside the flip handler was not
        // recovered; self-consistency is the defensible choice.)
        if (tryGrab(engine, ctx, lane, enemy)) return false;

        const float step = std::fabs(rate(enemy));
        if (step <= 0.0f) {                 // defensive: a zero-rate flip
            flipLand(engine, enemy);        // cannot progress, so resolve it
            return false;
        }
        // TERMINATION -- RE-AUTHORED, MANDATORY. The original tests err == 0
        // with no tolerance, which only terminates because every the later port web
        // orientation is a multiple of 1/64 turn. This port's webs have
        // arbitrary turning angles, so an exact-equality test WOULD HANG:
        // step, and when the remaining rotation is at or below one step, SNAP
        // to the target and complete.
        if (remaining(enemy) <= step) {
            turned(enemy) = total(enemy);
            flipLand(engine, enemy);
            return false;
        }
        turned(enemy) += (total(enemy) >= 0.0f ? step : -step);
        return false;
    }

    // ---- S2 STOPPED -----------------------------------------------------
    // Per tick, in order:
    //   1. BULLET COLLISION -- SHARED (see the RAIL note).
    //   2. TANKER-MARKER BRANCH.
    if (marker(enemy) < 0) {
        // ---- MARKER -2 -> METAMORPHOSE INTO A PULSAR, NOW ------------------
        // A pulsar-tanker's two children are born ORDINARY FLIPPERS, flip
        // exactly one lane, and become pulsars THERE -- so its payload is
        // "flipper, then pulsar", never a pulsar directly. This branch was
        // written as a seam waiting for a destination; the destination now
        // exists.
        //
        // becomeFromFlipper() owns BOTH per-id counters (audit R4): rewriting
        // `enemy.id` in place without that leaves enemies_nums[old] never
        // decremented and drives enemies_nums[new] NEGATIVE on death, which
        // then lets the level over-spawn that type for the rest of the wave.
        // It returns with the slot still occupied -- by a pulsar -- so this is
        // NOT a removal and must return false.
        if (marker(enemy) == MARKER_PULSAR) {
            ArcadePulsar::becomeFromFlipper(engine, lane, enemy);
            return false;
        }

        // Any other negative -> drop back into RAIL and dive (and
        // super-flipper-3 additionally re-flips at once).
        const int sv = sub(enemy);
        if (sv == SUB_SUPER3_DIVE || sv == SUB_SUPER3_SEEK) {
            mode(enemy) = MODE_FLIP;
            if (flipSetup(engine, ctx, lane, idx, enemy, 0) >= 0) return true;
            // ...unless there was nowhere to flip, in which case it takes the
            // ordinary path below (flipSetup parked it, so restore RAIL).
        }
        mode(enemy) = MODE_RAIL;
        return false;
    }

    //   2b. SUPER STOP -- the forced spin. If the counter is zero, behave as
    //       standard; otherwise decrement its MAGNITUDE and flip again
    //       immediately in a FIXED direction (negative = lower index, positive
    //       = higher), with no pause and WITHOUT LOOKING AT THE PLAYER.
    //
    //       The counter is a random 1..4 written ONLY by the super-promotion
    //       routine, which is called ONLY from the flip-tanker hatch, and its
    //       sign is inherited from which of the hatched pair it is -- so each
    //       hatchling keeps spinning the way it was launched and the pair fans
    //       2-5 lanes apart. Level-spawned supers explicitly zero it.
    if (spin(enemy) != 0) {
        const int dir = (spin(enemy) > 0) ? +1 : -1;
        spin(enemy) -= dir;                  // decrement toward zero
        mode(enemy) = MODE_FLIP;
        return flipSetup(engine, ctx, lane, idx, enemy, dir) >= 0;
    }

    //   3. SAME-LANE PLAYER TEST: same lane AND |dz| <= ParkedGrabDz
    //      (0.3125 engine z = 2 world-z) AND the claw is active -> grab.
    if (tryGrab(engine, ctx, lane, enemy)) return false;

    //   4. PAUSE COUNTDOWN. Decremented on entry; the flip fires on the tick
    //      the decrement underflows, so PauseTicks = 8 is 8 full waiting ticks
    //      plus the flipping tick = 9 wall ticks. A reload of -1 (super-2 and
    //      super-3) underflows every tick, so they never pause.
    pause(enemy) -= 1;
    if (pause(enemy) < 0) {
        mode(enemy) = MODE_FLIP;
        return flipSetup(engine, ctx, lane, idx, enemy, 0) >= 0;
    }
    return false;
}

// ===========================================================================
// Construction
// ===========================================================================

namespace {

// The fields every flipper starts with, whatever made it.
void initCommon(Enemy& e, int id, int subVariant, int markerVal, float z) {
    using F = ArcadeFlipper;
    e = Enemy{};
    e.id   = id;
    e.z    = z;
    e.life = ENEMY_LIFE[id];              // 1 -- one hit anywhere kills
    // INTERIM (audit sec 3, the mush_flag trap). Four shared gates read bit 1
    // of `mush_flag` as a general-purpose "takes direct damage / does not
    // breathe" flag, and an inline-constructed Enemy inherits 0 -- which would
    // make the flipper take damage from an upgraded shot only 1 roll in 11
    // (reading as a bullet sponge, the exact opposite of the one-hit rule) and
    // would give it this engine's breathing wobble, which sec 5.3 forbids.
    // 15 is what GameEngine::init_enemy writes. REPLACE THIS with the audit's
    // named predicate (enemyTakesDirectDamage / enemyHasBreathe) when that
    // lands as its own change; do not spread `= 15` to more spawn sites.
    e.mush_flag = 15;

    e.anchor     = ANCHOR_LANE_MID;
    e.pivot_side = -1;
    e.anchor_rot = 0.0f;
    e.cross_t    = 0.0f;

    F::mode(e)   = F::MODE_ARRIVAL;
    F::sub(e)    = subVariant;
    F::pause(e)  = F::pauseReloadFor(subVariant);
    F::spin(e)   = 0;
    F::marker(e) = markerVal;
    F::stamp(e)  = INT32_MIN;             // "never updated"
    F::total(e)  = 0.0f;
    F::rate(e)   = 0.0f;
    // THE BEAST'S HORN COUNTER, derived from the sub-variant rather than
    // passed, so no caller can build a Beast without its horns or a flipper
    // with them. Zero on every other sub-variant -- which is also the value
    // that would send a non-Beast straight down beastRail's death branch, so
    // the RAIL dispatch's `sub == SUB_BEAST` gate is the thing keeping it
    // unreachable, not this line.
    F::setHorns(e, (subVariant == F::SUB_BEAST) ? F::BeastHorns : 0);
}

} // namespace

bool ArcadeFlipper::spawn(GameEngine& engine, int lane, int id) {
    if (lane < 0 || lane >= engine.lane_count) return false;
    GridElement& elem = engine.grid[lane];
    if (elem.num_enemies >= MAX_ENEMIES) return false;

    const int wave = engine.current_level;
    const int requested = id;

    int subVariant = SUB_PLAIN;
    if (id == ARCADE_SFLIPPER2) {
        subVariant = SUB_SUPER2;
    } else if (id == ARCADE_SFLIPPER3) {
        subVariant = SUB_SUPER3_DIVE;
    } else if (id == ARCADE_BEAST) {
        // THE BEAST TAKES NO PROMOTION ROLL, and its position in this chain --
        // BEFORE the rand() below rather than after -- is what guarantees it.
        // The reference's Beast maker calls the shared flipper maker and then
        // overwrites its five fields; it never enters the super-promotion
        // path, so a Beast must not consume a draw from the random stream
        // either. Reordering these two arms would be invisible in behaviour
        // and would silently shift every subsequent random in the level.
        subVariant = SUB_BEAST;
    } else if (rand() % 256 < promoteLevelSuper2Num(wave)) {
        // The PLAIN spawner can only ever produce super-2, and only from wave
        // 16 up. Super-3 is reachable only from the generator's own id-11
        // entry (above) or from the tanker's second roll (spawnHatchling).
        subVariant = SUB_SUPER2;
        id = ARCADE_SFLIPPER2;
    }

    Enemy e;
    // Spawned at z = 71.875 with its draw index NEGATED -- here, MODE_ARRIVAL,
    // which is what makes it the perspective-projected 2x2 dot.
    // The grab marker starts as the flipper's OWN lane so that a level-spawned
    // flipper reads as non-negative and can grab from its first parked tick;
    // only tanker hatchlings carry a negative marker.
    initCommon(e, id, subVariant, lane, SpawnZ);
    // Level-spawned super-2 and super-3 explicitly ZERO the forced-spin
    // counter and never do the forced spin.
    spin(e) = 0;

    elem.enemies.push_back(e);
    elem.num_enemies += 1;
    engine.enemies_nums[e.id] += 1;

    // AUDIT sec 8, and this is the one that HANGS THE LEVEL if it is missed:
    // enemies_todo[] is decremented by the SHARED machinery in exactly one
    // place -- init_embrio -- and the arcade arrival replaces the embryo
    // entirely. An arcade release that never decrements it leaves
    // is_level_clear() false forever with nothing on screen. Every arcade
    // family decrements its own here (see the accounting rule above
    // arcade_release in enemies_shared.h). Charged to the id the caller ASKED
    // for, so the super-2 promotion above cannot strand a budget entry.
    if (engine.enemies_todo[requested] > 0)
        engine.enemies_todo[requested] -= 1;
    return true;
}

bool ArcadeFlipper::spawnHatchling(GameEngine& engine, const EnemyCtx& ctx,
                                int lane, float z, int dir, int markerVal) {
    if (lane < 0 || lane >= engine.lane_count) return false;
    GridElement& elem = engine.grid[lane];
    if (elem.num_enemies >= MAX_ENEMIES) return false;

    // Built as an ORDINARY flipper copying the tanker's position, depth and
    // lane, already parked, with its OWN lane as the marker so flip-setup
    // records a real source lane.
    Enemy e;
    initCommon(e, ARCADE_FLIPPER, SUB_PLAIN, lane, z);
    mode(e) = MODE_STOPPED;

    elem.enemies.push_back(e);
    elem.num_enemies += 1;
    engine.enemies_nums[ARCADE_FLIPPER] += 1;
    // NOT charged against enemies_todo[]: a hatchling is the tanker's payload,
    // not one of the level's planned releases.

    const int idx = elem.num_enemies - 1;
    // The immediate flip, at the PLAIN rate -- see the ordering note in the
    // header. ctx.time is only used for the once-per-tick stamp: the tanker
    // calls this from inside its own update, so the hatchling must NOT also
    // act on this tick.
    mode(elem.enemies[idx]) = MODE_FLIP;
    const int dst = flipSetup(engine, ctx, lane, idx, elem.enemies[idx], dir);

    // The child now lives at the END of the destination lane (or stayed put if
    // it parked).
    GridElement& home = engine.grid[(dst >= 0) ? dst : lane];
    Enemy& child = home.enemies[home.num_enemies - 1];

    // ...and ONLY NOW the super-promotion roll, which is what makes the first
    // flip run plain.
    const int wave = engine.current_level;
    if (rand() % 256 < promoteHatchSuper2Num(wave)) {
        sub(child) = SUB_SUPER2;
        child.id   = ARCADE_SFLIPPER2;
        engine.enemies_nums[ARCADE_FLIPPER]   -= 1;
        engine.enemies_nums[ARCADE_SFLIPPER2] += 1;
        if (rand() % 256 < promoteSuper3Num(wave)) {
            sub(child) = SUB_SUPER3_SEEK;   // hatched at the tanker's depth,
                                            // already hunting: the DIVE
                                            // variant is the level-spawned one
            child.id   = ARCADE_SFLIPPER3;
            engine.enemies_nums[ARCADE_SFLIPPER2] -= 1;
            engine.enemies_nums[ARCADE_SFLIPPER3] += 1;
        }
        pause(child) = pauseReloadFor(sub(child));
        // The forced spin: a random 1..4 whose SIGN is which of the hatched
        // pair this is, so the two fan apart and keep fanning.
        const int mag = ForcedSpinMin + rand() % (ForcedSpinMax - ForcedSpinMin + 1);
        spin(child) = (dir >= 0) ? mag : -mag;
    }

    // ...and LAST the -1 / -2 sentinel: cannot grab, and drop into RAIL when
    // this first flip lands. It is overwritten with a real lane number by the
    // next flip-setup, after which it is an ordinary hunting flipper.
    marker(child) = markerVal;
    return true;
}

// ===========================================================================
// sec 2.5 -- the arcade roster's ONE global fire timer
// ===========================================================================

void ArcadeFlipper::alienFireTick(GameEngine& engine, int& fireTimer,
                               int lane, float z) {
    // The countdown is decremented AND RELOADED **before** any of the gates is
    // evaluated (CORRECTED/added, sec 2.5), so an attempt blocked by the
    // end-of-wave flag, the depth gate or the bullet budget is LOST, not
    // deferred: the cadence restarts as if it had fired.
    fireTimer -= 1;
    if (fireTimer > 0) return;
    fireTimer = AlienFirePeriod;

    if (engine.player.out_animation != 0) return;     // end of wave
    if (engine.player.gameover_animation > 0) return;
    // The shooter's z must be strictly DEEPER than 9.375 -- the deepest 62.5%
    // of the tube, measured from the far end.
    if (z <= AlienFireDepthGate) return;
    // Concurrent budget min(wave >> 3, 3), tested as >= 0, so the real cap is
    // budget + 1: 1 bullet on waves 1-8 rising to 4. Enforced HERE and not by
    // changing SHOT_MAX[ENEMY_SHOT1], which this port's own roster depends on.
    int cap = engine.current_level >> 3;
    if (cap < 0) cap = 0;
    if (cap > AlienFireCapMax - AlienFireCapBase)
        cap = AlienFireCapMax - AlienFireCapBase;
    if (engine.shots_nums[ENEMY_SHOT1] >= cap + AlienFireCapBase) return;

    engine.init_shot(lane, z, ENEMY_SHOT1);
}

} // namespace enemyfam
} // namespace ts
