#pragma once

// ============================================================================
// demo_ai.h — THE ATTRACT-MODE PILOT.
//
// Recovered from the LATER PORT's attract mode (see DOCTRINE.md for the source
// policy: behaviour is read, code and data never are). Its whole autopilot is
// two short routines, and the shape is worth stating because it is much simpler
// than anyone expects an "AI" to be:
//
//   1. THE TARGET IS THE ENEMY NEAREST THE RIM. Its per-frame object sweep
//      keeps the smallest depth it has seen and the lane that enemy is on, and
//      hands that lane to the autopilot. Nothing else is considered -- not
//      threat, not type, not what is about to flip onto you.
//   2. STEER THE SHORT WAY ROUND. Its direction routine compares the lane
//      difference against HALF the web and wraps when the far way is shorter.
//      An open web has no wrap, so it just compares.
//   3. ALIGNED MEANS FIRE. When the claw is already on the target lane the
//      routine returns a sentinel and the autopilot stops moving and shoots.
//
// THE ELEGANT PART, and the reason this reads as deliberate rather than
// hacked: that direction routine is the SAME one its AI-droid companion uses
// for steering, and the "aligned -> fire" sentinel is gated on the attract
// flag. One routine, one extra branch, and the demo gets a pilot for free.
// This port keeps that structure -- the law below is the shared one, and only
// the sentinel belongs to the demo.
//
// WHAT IS DELIBERATELY NOT PORTED: the bit layout. The reference packs its
// answer into a control byte whose two direction bits are its own convention,
// which does not survive the trip -- this port's lanes are indexed the other
// way and ACT_MOVE_LEFT is what INCREMENTS grid_element_pos (player.cpp). So
// the decision is expressed as "which way round the ring", and the caller maps
// that to an action bit. Porting the byte would have inverted the claw exactly
// as the 3DS/PC steering divergence did (input_frame.h).
//
// Layering: pure simulation over GameEngine -- no rendering, no platform, no
// heap, no virtuals. It produces an ORDINARY InputFrame, so nothing downstream
// can tell a demo from a player, which is also how the reference does it (its
// autopilot writes the same control byte the joystick would).
//
// ----------------------------------------------------------------------------
// 4. THE PILOT DODGES (2026-09-26).
//
// Enemy fire is live in attract mode again. That retires the premise this file
// was written against -- "it cannot see a shot coming, so it cannot dodge one"
// -- because every hazard in this game is LANE-ADDRESSED, and the whole web is
// readable from here. The claw is never threatened by something it cannot see;
// it is threatened by things it chose to ignore. So it stops ignoring them.
//
// THE MODEL IS ONE NUMBER PER LANE: `eta[L]`, engine ticks until something can
// kill the claw in lane L, or infinity. Not a category per hazard type -- one
// scalar -- because the decision is the same for a horn, a bullet and a pulsar:
// do not be there when it comes due. That is what makes this uniform rather
// than a pile of special cases, and it is why a family added next month is
// covered the day it is added as long as it reports into the scan.
//
// THE THREE HAZARDS, and why each is what it is:
//
//   HOSTILE SHOTS (ids >= ARCADE_REFLECT_SHOT). Two lethal laws because the
//   tree has two. Ordinary enemy fire is a SWEPT PLANE (collision.cpp:216,
//   `shot.z >= p.z && shot.z + dz <= p.z`): zero window, speed from SHOT_DZ.
//   The arcade reflect -- the beast's horn, a mirror's return -- owns a |dz|
//   window instead and steps its OWN z, so its speed is carried on the shot.
//   A horn never changes lanes, so the lane a beast was shot in is the dangerous
//   one, from the strike down to ExpiryZ. That is the whole reason the claw can
//   dodge a horn at all: it leaves the lane, it does not outrun the horn.
//
//   THE PULSAR. The pulse is ONE global counter shared by every pulsar on the
//   web, so its ETA is computed once, not per enemy. When it lands on the
//   lethal phase the pulsar's own test is lane equality plus `player.z >= 0`
//   and NOTHING else (arcade_pulsar.cpp:321-349) -- no depth window, whole
//   tube, any distance. Jumping is the only escape and this pilot does not
//   jump, so the only answer is to not be in the lane.
//
//   THE PULSAR SPARK. It never moves in z and HOPS one lane every ~4.45
//   engine ticks, so the hazard is not the lane it is in but the RUN of lanes
//   ahead of it -- marked forward, in its own direction, for as far as the
//   horizon reaches. Marking only its own lane would be wrong in a specific and
//   invisible way: the hop period is shorter than the claw's first lane step
//   from rest, so a pilot that reacted only on arrival could never have left.
//
// SAFE MEANS ENEMY-FREE FIRST. When the pilot has to leave a lane it picks a
// lane that is clear of enemies before it picks one that is merely survivable --
// the user's "a tube that does not have enemies in it". Enemy-free is a
// PREFERENCE, not a constraint: with a horn in flight and a full web there is
// often no such lane, and a pilot that cannot find one must still move, so the
// fallback is the lane whose ETA is furthest off. A fallback that freezes is
// worse than a fallback that buys time.
//
// THE HORIZON IS THE WHOLE AI. Everything above reduces to "is eta[L] bigger
// than HAZARD_HORIZON", and that one constant decides whether the demo looks
// capable or twitchy. See its own comment for how it is sized.
// ============================================================================

#include <cstdlib>

#include "engine.h"
#include "input_frame.h"
#include "enemies/arcade_adroid.h"
#include "enemies/arcade_mirror.h"
#include "enemies/arcade_pulsar.h"

namespace ts {
namespace demoai {

// Sentinel: the claw is already on the target lane.
constexpr int STEP_ALIGNED = 0;
constexpr int STEP_INC     = +1;   // toward a HIGHER lane index
constexpr int STEP_DEC     = -1;   // toward a LOWER lane index

// Which way round the ring to reach `target` from `cur`, taking the short way
// on a closed web. Transcribed from the reference's direction routine, with
// its control-byte encoding replaced by a lane-index step (see the header note).
inline int demoStep(int cur, int target, int laneCount, bool goRound) {
    if (cur == target) return STEP_ALIGNED;
    if (!goRound) return (cur < target) ? STEP_INC : STEP_DEC;
    // Closed: go direct when the direct distance is under half the ring.
    const int half = laneCount >> 1;          // its `sar dx,1`
    if (cur < target) return ((target - cur) - half < 0) ? STEP_INC : STEP_DEC;
    return ((cur - target) - half < 0) ? STEP_DEC : STEP_INC;
}

// The lane of the enemy nearest the rim, or -1 if the web is empty. Enemies are
// stored per lane, so the lane is the index rather than a field to read.
inline int demoTargetLane(const GameEngine& e) {
    const int lanes = (int)e.grid.size();

    // ---- A CAPSULE OUTRANKS AN ENEMY (user request) ------------------------
    // DELIBERATE DEVIATION from the recovered pilot described above, which
    // weighs nothing but depth and never goes out of its way for a pickup.
    //
    // The reason is what an attract mode is FOR: a capsule climbing out
    // unclaimed reads as the demo player MISSING it, while a demo that collects
    // reads as one that knows what it is doing -- and collecting is also what
    // drives the visible ladder (laser, jump, droid, zapper), so the demo shows
    // the game's own systems instead of a bare claw shooting flippers.
    //
    // It is safe to keep firing while parked on a capsule's lane: player shots
    // only collide with ids >= ENEMY_SHOT1 (collision.cpp), and POWERUP_SHOT is
    // below that, so the pilot cannot shoot away the thing it came for.
    //
    // HAZARDS OUTRANK THE CAPSULE, and that is a change of mind since this
    // comment was written: the capsule is still the thing to chase, but it is no
    // longer the thing to chase INTO a horn. A demo that collects and then dies
    // reads worse than one that skips a capsule.
    int   powLane = -1;
    float powZ    = 0.0f;
    for (int L = 0; L < lanes; ++L) {
        const GridElement& g = e.grid[L];
        for (int i = 0; i < g.num_shots && i < (int)g.shots.size(); ++i) {
            if (g.shots[i].id != POWERUP_SHOT) continue;
            const float z = g.shots[i].z;
            if (powLane < 0 || z < powZ) { powZ = z; powLane = L; }
        }
    }
    if (powLane >= 0) return powLane;

    int   bestLane = -1;
    float bestZ    = 0.0f;
    for (int L = 0; L < lanes; ++L) {
        const GridElement& g = e.grid[L];
        for (int i = 0; i < g.num_enemies && i < (int)g.enemies.size(); ++i) {
            const float z = g.enemies[i].z;
            if (bestLane < 0 || z < bestZ) { bestZ = z; bestLane = L; }
        }
    }
    return bestLane;
}

// ---- THE HAZARD SCAN ------------------------------------------------------

// Reaction horizon, in engine ticks (~16 ms each). Sized against how fast the
// claw can LEAVE, not how fast the hazard arrives:
//
//   - the claw needs ~11 ticks to COMPLETE its first lane step from a
//     standstill (player.cpp: CLAW_MAX_SPEED = 16/43*9 = 3.3488 phase/tick,
//     ramped over CLAW_RAMP_FRAMES = 24, 9 phase units per lane), then about
//     2.7 ticks per lane at the top of the ramp;
//   - a beast horn closes at 0.25 z/tick and kills inside 0.3125, so a horn
//     taken at a parked flipper's z has roughly 25 ticks to reach the claw.
//
// 24 ticks buys a lane and a half from rest. Shorter and the pilot visibly
// flinches late; longer and it starts treating a shot fired at the far plane --
// minutes of warning -- as a reason to abandon the lane, which reads as panic.
constexpr int HAZARD_HORIZON = 24;

// Engine ticks for the claw to COMPLETE one lane step from a dead stop -- the
// pilot's actual reaction cost, as opposed to the horizon, which is how far
// ahead it looks. Derived from player.cpp rather than guessed: the ramp is
// (i/CLAW_RAMP_FRAMES)*CLAW_MAX_SPEED phase units on tick i, a lane costs 9, so
// sum_{i=1..n} (i/24)*3.3488 >= 9 gives n(n+1) >= 129, n = 11.
//
// This is the number that decides whether a warning was USEFUL. A hazard that
// shows up with less than this on the clock cannot be left no matter how correct
// the scan is -- a pulsar that metamorphoses out of a landed flipper IN PLACE
// (arcade_pulsar.cpp becomeFromFlipper) with the crackle due is exactly that,
// and the reference's answer to it is a JUMP, which this pilot does not make.
constexpr float CLAW_LANE_STEP_TICKS = 11.0f;

// Sized to the engine's own ceiling rather than the live lane_count: the scan
// lives on the stack and this header promises no heap.
constexpr int MAX_LANES = GRID_MAX_ELEMENTS + 1;

// No-eta is a float, not a sentinel int, so it survives the same comparison
// every real eta goes through.
constexpr float NO_ETA = 1.0e9f;

struct LaneScan {
    float eta[MAX_LANES];    // ticks until the claw can be killed in this lane
    bool  enemy[MAX_LANES];  // something live is standing in it
    int   lanes = 0;
};

inline bool laneOk(int L, int lanes) { return L >= 0 && L < lanes && L < MAX_LANES; }

// Engine ticks until a hostile shot reaches the claw in its OWN lane, or NO_ETA
// if it never will. See the header for the two lethal laws.
inline float shotEtaTicks(const Shot& s, float playerZ) {
    if (s.id < ARCADE_REFLECT_SHOT || s.id >= SHOTS_NUM_IDS) return NO_ETA;
    const bool  arcade = (s.id == ARCADE_REFLECT_SHOT);
    const float speed  = arcade ? enemyfam::ArcadeReflectedShot::speed(s)
                               : -SHOT_DZ[s.id];
    if (speed <= 0.0f) return NO_ETA;              // not closing on the claw
    const float win = arcade ? enemyfam::ArcadeReflectedShot::LethalDz : 0.0f;
    if (s.z < playerZ - win) return NO_ETA;        // already through
    const float gap = s.z - (playerZ + win);
    return gap <= 0.0f ? 0.0f : gap / speed;
}

// Engine ticks until the pulse next lands on the lethal phase. ONE global
// counter for every pulsar on the web, so this is computed once per frame.
inline float pulseEtaTicks(int level) {
    using P = enemyfam::ArcadePulsar;
    const int phases = (P::LethalPhase - P::pulsePhase() + P::PulsePhases) % P::PulsePhases;
    return (float)phases * P::pulsePhaseTicks(level);
}

// How far ahead of a hopping spark the horizon reaches, in lanes. The hop period
// is ~4.45 engine ticks and the horizon is 24, so the spark can cross about five
// lanes before the pilot's next decision is worth anything -- but the pilot
// re-scans every tick, so what matters is the lanes it can reach BEFORE the claw
// could have left the lane it is standing in. Six is that number rounded up.
constexpr int SPARK_REACH = 6;

inline LaneScan scanLanes(const GameEngine& e) {
    LaneScan t;
    t.lanes = (int)e.grid.size();
    for (int L = 0; L < MAX_LANES; ++L) { t.eta[L] = NO_ETA; t.enemy[L] = false; }

    const float pz       = e.player.z;
    const float pulseEta = pulseEtaTicks(e.current_level);

    for (int L = 0; L < t.lanes && L < MAX_LANES; ++L) {
        const GridElement& g = e.grid[L];

        for (int i = 0; i < g.num_shots && i < (int)g.shots.size(); ++i) {
            const float eta = shotEtaTicks(g.shots[i], pz);
            if (eta < t.eta[L]) t.eta[L] = eta;
        }

        for (int i = 0; i < g.num_enemies && i < (int)g.enemies.size(); ++i) {
            const Enemy& en = g.enemies[i];
            t.enemy[L] = true;

            if (en.id == ARCADE_PULSAR) {
                // ANY mode, not just DESCEND. ARRIVAL is the collapsed dot still
                // falling in, and the pulse does not wait for it to land: a lane
                // that gets a live pulsar one phase before the lethal one is a
                // lane the claw cannot leave fast enough from. Measured -- the
                // arrival/materialise hand-off is exactly how the pulsar was
                // killing the pilot after the pulse rule landed.
                if (pulseEta < t.eta[L]) t.eta[L] = pulseEta;
            } else if (en.id == ARCADE_ADROID) {
                // The saucer ZAPS the whole lane it is over, and the reference's
                // dodge for it is a JUMP (arcade_adroid.h: the zap lives at the
                // rim plane and a jumping claw is below it). This pilot does not
                // jump, so its only answer is the lane.
                //
                // ANY mode except the dive-in, not just ZAPPAGE. The zap starts
                // on its own countdown out of the glide with no tell the scan can
                // see a tick ahead, and a saucer sitting in a lane one tick before
                // it zaps looks exactly like a saucer that is not going to.
                // UPPWEB is excluded because that one is still above the far
                // plane and cannot reach the rim this horizon.
                if (enemyfam::ArcadeAdroid::mode(en)
                        != enemyfam::ArcadeAdroid::MODE_UPPWEB) {
                    if (0.0f < t.eta[L]) t.eta[L] = 0.0f;
                }
            } else if (en.id == ARCADE_PULSAR_SPARK) {
                const float dz = en.z - pz;
                if (dz <= enemyfam::ArcadePulsarSpark::CatchDz
                    && -dz <= enemyfam::ArcadePulsarSpark::CatchDz) {
                    if (0.0f < t.eta[L]) t.eta[L] = 0.0f;
                    // The forward run, in the direction it is actually heading,
                    // timed by its own hop rather than stamped instantly lethal:
                    // lane L+dir*k is reachable after k hops. Marking the run
                    // with a real eta instead of zero is what keeps a spark from
                    // sterilising six lanes at once and leaving the pilot with
                    // nowhere to go.
                    const int dir = enemyfam::ArcadePulsarSpark::dir(en);
                    const float hop = enemyfam::ArcadePulsarSpark::PropEngineTicks;
                    for (int k = 1; k <= SPARK_REACH; ++k) {
                        const int nxt = L + dir * k;
                        if (!laneOk(nxt, t.lanes)) break;   // open web: it stops
                        const float eta = hop * (float)k;
                        if (eta < t.eta[nxt]) t.eta[nxt] = eta;
                    }
                }
            }
        }
    }
    return t;
}

// The lane to move to from `from`: clear of enemies first, then merely safe,
// then -- if nothing is safe -- the one that buys the most time. Nearest wins
// inside each class, on the ring when the web closes and clamped when it does not.
inline int pickLane(const LaneScan& t, int from, bool goRound) {
    for (int pass = 0; pass < 3; ++pass) {
        int best = -1;
        float bestEta = -1.0f;
        for (int d = 0; d < t.lanes; ++d) {
            for (int s = 0; s < 2; ++s) {
                const int L = from + (s == 0 ? d : -d);
                if (!laneOk(L, t.lanes)) continue;
                if (s == 1 && d == 0) continue;         // same lane, already tried
                const bool safe = t.eta[L] > (float)HAZARD_HORIZON;
                if (pass == 0 && (!safe || t.enemy[L])) continue;
                if (pass == 1 && !safe) continue;
                if (pass == 2) {
                    // Furthest-off hazard wins; `d` only breaks ties, so a lane
                    // the horn reaches in 200 ticks beats a nearer one it
                    // reaches in 3.
                    if (t.eta[L] > bestEta) { bestEta = t.eta[L]; best = L; }
                    continue;
                }
                return L;                              // nearest in this class
            }
        }
        if (pass == 2) return best;
    }
    return -1;
}

// The whole pilot. Steer toward the nearest enemy; fire when aligned, and fire
// when there is nothing to chase -- an idle claw reads as a hung game.
inline InputFrame demoInput(const GameEngine& e) {
    InputFrame f;
    const LaneScan t = scanLanes(e);
    int cur = e.player.grid_element_pos;
    if (!laneOk(cur, t.lanes)) cur = 0;

    int goal = -1;

    // 1. LEAVE A LANE THAT IS ABOUT TO KILL US. This outranks the capsule and
    //    outranks whatever the claw was lining up.
    if (t.eta[cur] <= (float)HAZARD_HORIZON) goal = pickLane(t, cur, e.grid_level_go_round);

    // 2. OTHERWISE THE RECOVERED PILOT: nearest capsule, else nearest enemy to
    //    the rim -- but not INTO a hazard. A beast that has just shed, or a
    //    pulsar whose phase is coming due, makes its own lane a lane to shoot
    //    from only after it stops being a lane to die in.
    if (goal < 0) {
        int target = demoTargetLane(e);
        if (target >= 0 && laneOk(target, t.lanes)
            && t.eta[target] <= (float)HAZARD_HORIZON) {
            target = pickLane(t, cur, e.grid_level_go_round);
        }
        goal = target;
    }

    const int step = (goal < 0)
        ? STEP_ALIGNED
        : demoStep(cur, goal, e.lane_count, e.grid_level_go_round);

    if (step == STEP_ALIGNED) {
        f.held |= ACT_SHOOT;
    } else {
        // ACT_MOVE_LEFT is what INCREMENTS grid_element_pos in this port
        // (player.cpp: leftdx drives animation_phase up). Naming it here rather
        // than trusting the word "left" is the point of the header's note.
        f.held |= (step == STEP_INC) ? ACT_MOVE_LEFT : ACT_MOVE_RIGHT;
    }

    // 3. THE SUPER ZAPPER SHOW (user request 2026-09-26, refined same day).
    //    ONE zap per level/life, spent from the rack the moment the crowd is
    //    big enough to be worth clearing:
    //
    //      - the trigger count is a 50/50 roll of 3 or 4, made once per level
    //        when the level first appears (keyed on current_level, so the
    //        attract slice's random level pick gets a fresh flip every run);
    //      - 4 IS THE GUARANTEED TRIGGER. If the roll said 3 and the count
    //        never sat on 3 long enough to fire -- the pilot was mid-dodge,
    //        the third enemy died to a stray shot -- it fires the moment 4
    //        are up. The zap never gets skipped for want of an exact count;
    //      - exactly ONE press per level/life (zapFired), so the show is one
    //        screen clear, not a held trigger chewing the rack. The bit is
    //        set for a single frame and the level/life latch stops any retry.
    //
    //    The latch RE-ARMS on a life change: the rack is restocked at every
    //    death (engine.cpp ZAPPER_STOCK_PER_LEVEL), and the user's promise is
    //    one zap per level AND per life -- a new life gets its own show with
    //    the same level's flip, not a dead latch.
    //
    //    zapp_stock >= 2, not > 0: the last rack unit is the SINGLE-KILL zap
    //    (player.cpp: zapp_single when stock == 1), which is not the screen
    //    clear this show is for. The pilot spends one slot and keeps the last
    //    one in reserve.
    static int  zapLevel = -1;
    static int  zapAt    = 3;
    static int  zapLife  = -1;   // the lives value this level's latch belongs to
    static bool zapFired = false;
    if (zapLevel != e.current_level) {
        zapLevel = e.current_level;
        zapAt    = (std::rand() % 2 == 0) ? 3 : 4;
        zapLife  = e.player.lives;
        zapFired = false;
    } else if (zapLife != e.player.lives) {
        zapLife  = e.player.lives;   // died and restocked: this life gets its show
        zapFired = false;
    }

    int liveEnemies = 0;
    for (int L = 0; L < t.lanes && L < MAX_LANES; ++L)
        if (t.enemy[L]) ++liveEnemies;

    if (!zapFired
        && liveEnemies >= zapAt
        && e.player.zapp_stock >= 2
        && e.player.animation_zapp == 0) {
        f.held |= ACT_ZAPPER;
        zapFired = true;   // one press per level/life; re-arms on level or life change
    }
    return f;
}

} // namespace demoai
} // namespace ts
