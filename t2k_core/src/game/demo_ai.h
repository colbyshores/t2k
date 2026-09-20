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
// ============================================================================

#include "engine.h"
#include "input_frame.h"

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

// The whole pilot. Steer toward the nearest enemy; fire when aligned, and fire
// when there is nothing to chase -- an idle claw reads as a hung game.
inline InputFrame demoInput(const GameEngine& e) {
    InputFrame f;
    const int target = demoTargetLane(e);
    const int step = (target < 0)
        ? STEP_ALIGNED
        : demoStep(e.player.grid_element_pos, target,
                   e.lane_count, e.grid_level_go_round);

    if (step == STEP_ALIGNED) {
        f.held |= ACT_SHOOT;
    } else {
        // ACT_MOVE_LEFT is what INCREMENTS grid_element_pos in this port
        // (player.cpp: leftdx drives animation_phase up). Naming it here rather
        // than trusting the word "left" is the point of the header's note.
        f.held |= (step == STEP_INC) ? ACT_MOVE_LEFT : ACT_MOVE_RIGHT;
    }
    return f;
}

} // namespace demoai
} // namespace ts
