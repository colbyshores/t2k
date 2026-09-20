#pragma once

#include <cstdint>

namespace ts {

struct GameEngine;

// =============================================================================
// WARP bonus rounds (docs/design/bonus_rounds.md).
//
// Replaces the Tsunami-derived spline-tube warp stage with re-authored rounds
// that follow the arcade reference's RULES (recovered behaviourally; no
// reference data copied — course data is procedurally generated):
//
//   GATES (round 0) — inertial 2-axis flight through 52 octagonal gates + a
//                     victory gate. Missing ONE gate fails instantly.
//   RAIL  (round 1) — roll around a tube rim staying on a winding track;
//                     the comet tail is the life meter.
//
// THIS IS A SLOT SYSTEM AND THE THIRD SLOT IS EMPTY ON PURPOSE. A third round
// (GHOST) was written and then REMOVED ENTIRELY on 2026-09-02 (user call:
// "completely get rid of the 3rd bonus system, scrub it from the codebase...
// There should still be a slot system in case I want to add one back in but
// as of right now, I dont have any fresh ideas and want to keep my code clean
// and easy to read. We shouldnt have code smell that is gated off and never
// accessed."). It had been dark behind kRoundByBand since 2026-08-19 -- code
// in both renderers, a course grammar, an echo pool and per-round state that
// nothing could reach. Git holds it (before this commit) if it is ever wanted.
//
// ADDING A ROUND BACK is: a WARP_ROUND_* id, bump WARP_ROUND_COUNT (which
// sizes GameEngine::warp_plays), give it entries in kRoundByBand, and branch
// it in init_warp/move_warp and each backend's ts_render_warp. Nothing else
// is round-specific.
//
// Selection: kRoundByBand[(level >> 4) & 3] — the round rotates with the
// 16-level colour band. Difficulty: a persistent per-round play counter
// (GameEngine::warp_plays) picks one of 8 deterministic courses (count & 7,
// both rounds) and tightens the round each recurrence — GATES by scaling
// base_speed (SPEED_PER_PLAY), RAIL by shortening its step clock
// (rail_step_frames). RAIL's base_speed is the fixed, backdrop-only
// RAIL_BACKDROP_SPEED — see its block in warp.cpp.
//
// SEAM (main.cpp / main_3ds.cpp are the only callers; both frontends were
// updated together when the vertical axis went live — parity policy):
//   init_warp(engine, engine.warp, wallMs)                        — once on entry
//   move_warp(engine, engine.warp, left, right, up, down, wallMs) — per frame
//   engine.warp.warp_level_end                                    — polled to exit
// =============================================================================

// Round ids (also index GameEngine::warp_plays). A new round appends here
// and bumps the count — see the slot note in the header block.
constexpr int WARP_ROUND_GATES = 0;
constexpr int WARP_ROUND_RAIL  = 1;
constexpr int WARP_ROUND_COUNT = 2;

// Phase of the round's little state machine.
constexpr int WARP_PHASE_PLAY = 0;
constexpr int WARP_PHASE_WIN  = 1;   // white-out envelope ramping
constexpr int WARP_PHASE_FAIL = 2;   // fail fade counting

// Gate types (GATES courses).
constexpr int WGATE_PLAIN   = 0;
constexpr int WGATE_UP      = 1;   // up-impulse: catching it kicks vertical vel
constexpr int WGATE_DOWN    = 2;   // down-impulse
constexpr int WGATE_BOOST   = 3;   // speed boost: speed += speed/8, section += 1
constexpr int WGATE_VICTORY = 4;   // catching it wins the round

// Course/course-geometry constants shared with the renderers.
constexpr int   WARP_MAX_GATES    = 56;     // 52 + victory + headroom
constexpr int   WARP_COURSE_GATES = 52;     // regular gates before the victory gate
constexpr int   WARP_RAIL_STEPS   = 64;     // RAIL course length
constexpr float WARP_TICK_Z       = 32.0f;  // one course tick = fixed z distance
constexpr float WARP_GATE_VIEW_Z  = 255.0f; // renderers draw gates with z <= this
constexpr float WARP_CATCH_Z      = 2.0f;   // catch test fires as z crosses this
constexpr float WARP_CATCH_BOX    = 8.0f;   // ±box on both axes at the near plane
constexpr float WARP_X_RANGE      = 64.0f;  // gate x lattice bound
constexpr float WARP_Y_RANGE      = 112.0f; // gate y bound (impulse-reachable)
constexpr float WARP_Y_REACH      = 40.0f;  // stick-reachable |y| without impulses
constexpr float WARP_RAIL_WINDOW  = 32.0f;  // on-track window, /256 rim units
constexpr float WARP_TAIL_MAX     = 31.0f;  // comet-tail meter cap

// One course gate (GATES). z is the RELATIVE distance ahead of the ship
// and decrements by `speed` every 16 ms step (the reference's z 255 -> 0
// approach); gates further than WARP_GATE_VIEW_Z simply haven't appeared yet.
struct WarpGate {
    float z    = 0.0f;
    float x    = 0.0f;
    float y    = 0.0f;
    int   type = WGATE_PLAIN;
    bool  alive = false;
};

// One inertial axis {value, vel}.
struct WarpAxis {
    float value = 0.0f;
    float vel   = 0.0f;
};

struct WarpState {
    // ---- seam fields (frontends poll these; names preserved) --------------
    bool warp_level_end  = false;
    int  warp_level_time = 0;      // absolute ms captured at init_warp
    // Fixed-16ms-step accumulator (same pattern the old sim used): absolute ms
    // simulated so far; move_warp steps while sim_time + 16 <= time.
    int  sim_time = 0;
    bool scored   = false;         // payout latch (add_warp_score)

    // ---- round identity ----------------------------------------------------
    int  round      = WARP_ROUND_GATES;   // WARP_ROUND_*
    int  play_count = 0;                  // this run's play index for the round
    int  phase      = WARP_PHASE_PLAY;    // WARP_PHASE_*
    bool won        = false;

    // ---- GATES course + flight ---------------------------------------------
    WarpGate gates[WARP_MAX_GATES];
    int  gate_count = 0;
    int  next_gate  = 0;           // front-most alive gate index

    WarpAxis axis_x;               // ship lateral offset (world units)
    WarpAxis axis_y;               // ship vertical offset (world units)

    // Vertical input seam — LIVE (both frontends pass up/down to move_warp).
    // in_up/in_down mirror the held flags of the most recent move_warp call
    // and are the ONLY vertical control law: pure inertial (accel while held,
    // friction, velocity cap, position clamp — the later port Inertcon /
    // the arcade reference ixcon-iycon). There is NO autopilot and NO recentering — the
    // ship stays wherever the player leaves it (rules fidelity, second proof
    // verdict in docs/design/bonus_rounds.md). Gate impulse SHOVES
    // (WGATE_UP/WGATE_DOWN setting vertical velocity) are gate effects, not
    // assists, and stay.
    // in_vert_live LATCHES true the first time real vertical input arrives
    // this round (reset by init_warp's full `state = WarpState{}`). NOTHING
    // READS IT TODAY — no renderer, backend or UI file mentions it and there
    // is no "climb!" prompt. It is kept as the latch half of the vertical
    // seam the river round is designed around (docs/design/bonus_rounds.md),
    // and any future use is RENDER-SIDE HINT ONLY: it must never gate the sim.
    bool in_up = false, in_down = false, in_vert_live = false;

    float speed      = 0.0f;       // z-units per 16 ms step (current)
    float base_speed = 0.0f;       // round-start speed (difficulty-scaled)
    float dist       = 0.0f;       // course distance travelled (backdrop scroll)
    int   section    = 0;          // escalation index: boost count (GATES)
                                   // or rail_step >> 3 (RAIL)
    float yes_pitch  = 1.0f;       // current chant pitch multiplier (renders/SFX)
    bool  yes_flip   = false;      // which sexy-yes sample LEADS the next catch's
                                   // two-voice pair (false = SEXY_YES1 first);
                                   // flipped by push_yes_pair per catch, reset
                                   // with the rest of the round by init_warp

    // Last catch, for the renderers' radial shock pulse (the popup + score are
    // already raised by the sim itself).
    int   catch_ms   = -1000000;   // sim_time at last catch
    float catch_x    = 0.0f;
    float catch_y    = 0.0f;
    int   catch_score = 0;         // 750 / 500 / 250

    // ---- RAIL ---------------------------------------------------------------
    uint8_t rail_track[WARP_RAIL_STEPS] = {};   // rim position 0..255 per step
    int   rail_step        = 0;    // steps completed, 0..WARP_RAIL_STEPS
    float rail_step_frac   = 0.0f; // progress within the current step, 0..1
    int   rail_step_frames = 0;    // frames per step (difficulty-scaled)
    int   rail_frame_ctr   = 0;
    WarpAxis roll;                 // rim position 0..256 (wraps), inertial
    float tail = 3.0f;             // comet-tail meter (3 start, cap 31)

    // ---- outcome envelopes (render read-only) -------------------------------
    float white_env = 0.0f;        // 0..1 win white-out crescendo
    float fail_fade = 0.0f;        // 0..1 lose fade (fail_ctr / 300)
    int   fail_ctr  = 0;           // 16 ms steps into the fail fade (-> 300)
};

// Build the round for engine.current_level: select the round type, bump the
// per-round play counter, generate the deterministic course, reset state.
void init_warp(GameEngine& engine, WarpState& state, int time);

// Advance the sim up to absolute `time`, stepping in 16 ms increments.
// left/right are the held steering inputs (the frontends' corrected mapping);
// up/down are the held VERTICAL inputs (GATES altitude — raw, never
// inverted: invert_move is a lateral preference only). up = +y = screen-up in
// BOTH renderers (see step_gates in warp.cpp for the projection evidence).
// RAIL ignores them: its roll IS the left/right axis and has no vertical.
void move_warp(GameEngine& engine, WarpState& state,
               bool left, bool right, bool up, bool down, int time);

// Resume a bonus round after a PAUSE (ui/pause_fx.h). The round's clocks are
// ABSOLUTE wall-clock stamps -- sim_time is a fixed-step accumulator and
// warp_level_time is the round's origin for WARP_TIMEOUT_MS -- so an interval
// the sim was not stepped must be given back to BOTH or the round both
// replays the pause and ages by it. Shared, because a pause that is
// transparent on one target and lethal on the other is not the same game.
void resume_warp(WarpState& state, int pausedMs);

// Win payout (design contract): 20,000 points through the ordinary scoring
// path + one extra life (init_1up). Latched by state.scored; the level
// advance itself happens when warp_level_end is raised.
void add_warp_score(GameEngine& engine, WarpState& state, int time);

} // namespace ts
