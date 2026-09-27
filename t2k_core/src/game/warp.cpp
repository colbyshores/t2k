#include "warp.h"
#include "engine.h"

#include <cmath>
#include <cstdio>

// =============================================================================
// WARP bonus rounds — shared simulation (no rendering).
// See docs/design/bonus_rounds.md for the design contract and warp.h for the
// state layout. Perf doctrine: fixed arrays, no heap, no exceptions, 16 ms
// fixed-step accumulator, everything O(gate_count) per step.
// =============================================================================

namespace ts {

namespace {

// ---- deterministic course RNG ----------------------------------------------
// The texture DSL's own LCG (textures.h class LCG, recovered from the shipped
// exe at 0x42da08): state *= 0x015a4e35, take bits 16..31. Re-stated here
// rather than including rendering/textures.h (game/ must not depend on
// rendering/). Seeded from (roundType*8 + (playCount & 7)) — 8 deterministic
// courses per round type, same course every run, zero reference bytes — mixed
// through a golden-ratio constant so seed 0 (GATES, first play) is not the
// LCG's absorbing state.
struct WarpRng {
    uint32_t s;
    explicit WarpRng(uint32_t seed) : s(seed) {}
    int next() { s *= 0x015a4e35u; return (int)((s >> 16) & 0xFFFFu); }
};

inline uint32_t course_seed(int roundType, int playCount) {
    uint32_t key = (uint32_t)(roundType * 8 + (playCount & 7));
    return 0x2A5F119Bu ^ (key * 0x9E3779B9u + 0x015a4e35u);
}

// ---- tuning constants (documented per the stage-1 contract) -----------------
// GATES flight. Horizontal is the player's axis (left/right seam input):
// accel 0.14/frame², friction 0.95 on velocity, cap ±2.0 units/frame — at the
// reference-pace 8-tick gate spacing (256 z-units / 1.333 z-per-frame = 192
// frames) that reaches well past the ±64 gate lattice between gates, which is
// the reference FEEL: always reachable, never trivial (the course generator
// tightens late-course gaps to keep it honest). Vertical authority is
// ±1.0 unit/frame (half the horizontal, "vertical stronger than horizontal"
// refers to the reference's impulses, which overshoot the stick clamp while
// coasting: an impulse sets |vel| above the cap and friction 0.985 lets it
// carry ~100+ units before control resumes).
constexpr float H_ACC  = 0.14f;
constexpr float H_FRIC = 0.95f;
constexpr float H_CAP  = 2.0f;
constexpr float H_POS_MAX = 96.0f;   // soft wall past the ±64 gate lattice

constexpr float V_CAP     = 1.0f;
constexpr float V_ACC     = 0.12f;   // stick slew per frame
constexpr float V_FRIC    = 0.985f;  // coast decay while impulse-overshooting
// Impulse-gate vertical kick: solved from the altitude delta the course
// actually asks for and the frames available before the next gate (measured:
// a fixed kick either flew 60+ units past small clusters or fell short of big
// ones at escalated speed — the smoke harness caught both), clamped to a
// floor that still reads as a kick and a hard ceiling the coast brake absorbs.
constexpr float V_IMP_MIN  = 1.3f;
constexpr float V_KICK_CAP = 4.5f;
constexpr float V_POS_MAX = 120.0f;
// One impulse's worth of altitude change at unhurried pacing; the generator
// additionally bounds it by what V_KICK_CAP can cover in the frames the
// escalated speed leaves between the impulse gate and the cluster.
constexpr float Y_CLUSTER_DELTA_MAX = 96.0f;

// Distance covered by an impulse coast of v0 = 1 over n frames at V_FRIC decay.
inline float coast_sum(float n) {
    return (V_FRIC * (1.0f - std::pow(V_FRIC, n))) / (1.0f - V_FRIC);
}

// Horizontal distance reachable from a STANDING START in n frames with the
// real H_ACC/H_FRIC/H_CAP kinematics (arriving hot — the catch box tests
// position only). Fitted against the exact per-frame integration: quadratic
// through the accel ramp, linear once the cap is reached (~28 frames).
inline float h_reach(float n) {
    return (n <= 20.0f) ? 0.05f * n * n : 20.0f + 1.9f * (n - 20.0f);
}

// Vertical stick reach over n frames: V_ACC slew to the ±1 cap (~9 frames),
// then capped — approximately (n - 4) units for n ≥ 9.
inline float v_reach(float n) {
    float d = n - 4.0f;
    return d > 0.0f ? d : 0.0f;
}

// Course pacing — REFERENCE-DERIVED (second proof verdict: much slower start).
// The arcade reference flies at base grndvel = 0.5 world-units/frame and a
// course tick is 12 world units: ONE TICK PER 24 FRAMES. This sim's tick is
// WARP_TICK_Z = 32 z-units, so the reference cadence maps to 32 / 24 = 1.333
// z-units per 16 ms step. (The old 4.0 was one tick per 8 frames — 3x the
// reference tick rate.) An 8-tick gap is now 192 frames ≈ 3.1 s per gate.
// Difficulty still adds 10% base speed per prior play of the same round type
// (capped at the 8th play, where the course table wraps), and boost gates
// still compound ×1.125 — both untouched by the re-pace.
constexpr float GATE_REF_FRAMES_PER_TICK = 24.0f;   // the arcade reference: 12 units / 0.5 per frame
constexpr float GATE_BASE_SPEED  = WARP_TICK_Z / GATE_REF_FRAMES_PER_TICK;  // 1.333
constexpr float SPEED_PER_PLAY   = 0.10f;
// Intro run-up before gate 0. 8 ticks × 24 frames = 192 frames ≈ 3.1 s at
// play 0 — the same wall-clock first-gate arrival the old 3x-fast build had
// with 24 ticks, so the round still opens promptly while the course itself
// runs at reference pace (gate 0 is also visible almost immediately: it
// spawns at 256 z against the 255 z view limit).
constexpr int   GATE_INTRO_TICKS = 8;

// RAIL's own backdrop pace — DECOUPLED from GATE_BASE_SPEED (side effect of
// the re-pace above, found 2026-08-19): RAIL has no gates, so its speed feeds
// exactly one thing — W.dist, whose only RAIL consumer is the warpStarRush
// backdrop scatter (renderer.cpp, rushMul 0.8). The 4.0 → 1.333 GATES fix
// silently slowed that star drift to a third. 4.0 is the value RAIL's
// backdrop was tuned and play-tested at; the tunnel rings/emitter particles
// ride warpRailFlow_ (rms-integrated) and never saw GATE_BASE_SPEED.
constexpr float RAIL_BACKDROP_SPEED = 4.0f;

// GATES opens just BELOW the river surface (world y = 0), so the opening
// frames read surface-overhead — the reference starts just under the plane.
// With the autopilot removed this is now FREE FLIGHT: the ship STAYS at −12
// until the player climbs (nothing recenters). −12 is well inside stick
// reach of gate 0's altitude across the ~3.1 s intro (the ±1-cap stick
// covers it in ~16 of the 192 intro frames), and climbing up through the
// plane is the round's signature crossing — now the player's own first move.
constexpr float GATES_START_Y = -12.0f;

// Scoring thirds over the |dx|+|dy| catch error (box is ±8 per axis, so the
// worst catchable error is 16): centre-best 750 / 500 / 250.
constexpr float CATCH_ERR_MAX = 2.0f * WARP_CATCH_BOX;

// The YES rule: every catch plays the sexy-yes voice twice; pitch steps up at
// every speed-boost gate. The bank now ships the reference's two alternating
// samples (SFX_SEXY_YES1/2, asset files 27/28), so the pair is the literal law:
// both voices per catch, WHICH SAMPLE LEADS flipping each catch (WarpState::
// yes_flip). This replaced the earlier detune substitute (one SFX_YES pushed
// twice, +-3% apart) that stood in while only one yes sample shipped. The
// climb-out chord (SfxId::YES, game_step.cpp) is a different event and keeps
// its own sample untouched.
constexpr float YES_PITCH_STEP = 0.05f;   // per boost section

// RAIL. Roll cap 4/256 rim units per frame (the reference cap); accel/friction
// chosen to reach the cap in ~14 frames and stop in ~12 — the same
// weight-with-grip feel as the GATES horizontal axis.
constexpr float R_ACC  = 0.35f;
constexpr float R_FRIC = 0.94f;
constexpr float R_CAP  = 4.0f;
constexpr int   RAIL_BASE_STEP_FRAMES = 34;   // 64 steps ≈ 35 s at play 0
constexpr int   RAIL_MIN_STEP_FRAMES  = 20;
constexpr float TAIL_START = 3.0f;

// Outcome envelopes. FAIL_FADE_STEPS is the fade's FULL-SCALE denominator, not
// its length: the fade runs 2x after BONUS_LOSE_SOUND_STEPS, so it lands at 205
// steps (≈3.28 s), not at the 300-step / 4.8 s full scale. Measured, not
// inferred: t2k_core/tools/warp_window_check.sh drives the real sim and baselines
// both windows. The win white-out is a shorter crescendo (~1.4 s).
constexpr int   FAIL_FADE_STEPS = 300;
constexpr float WHITE_ENV_STEP  = 1.0f / 90.0f;
// The fail "aww" (SFX_BONUS_LOSE) is 14070 samples @ 8 kHz ≈ 1.76 s ≈ 110
// sim steps at 16 ms. The fail fade runs at normal rate through the sound,
// then at 2x so the post-sound tail is halved — it was lingering ~3 s after
// the aww (user, 2026-09-18). If the aww asset changes length, update this
// to its step count.
constexpr int   BONUS_LOSE_SOUND_STEPS = 110;

// Defensive overall cap: no round legitimately runs this long (GATES ends at
// the victory gate, RAIL at step 64, any miss fails instantly). If it is ever
// hit something is wrong — treat as a loss so warp_level_end always arrives.
constexpr int WARP_TIMEOUT_MS = 240000;

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Wrapped rim distance in 0..256 space.
inline float rim_dist(float a, float b) {
    float d = a - b;
    while (d > 128.0f)  d -= 256.0f;
    while (d < -128.0f) d += 256.0f;
    return d < 0.0f ? -d : d;
}

// ---- course generation -------------------------------------------------------

// GATES: 52 gates + victory. Grammar per the design doc: gap ticks 8 early,
// tightening to 3..8 later in the course, x on a 16-unit lattice within ±64
// (per-gap delta bounded by what the capped stick can actually traverse at the
// escalating speed), y within ±112 but only via up/down-impulse gates placed
// BEFORE any cluster whose |y| exceeds the stick-reachable ±40, a speed boost
// roughly every 8–10 gates, victory last.
void generate_gate_course(WarpState& s) {
    WarpRng rng(course_seed(s.round, s.play_count));

    float tick_pos = (float)GATE_INTRO_TICKS;
    float x = 0.0f;        // lattice walk
    float yBand = 0.0f;    // current cluster altitude
    float speed_est = s.base_speed;   // escalates at boost gates
    int   since_boost = 0;
    int   next_boost_at = 8 + rng.next() % 3;   // boosts every 8..10 gates
    int   cluster_left = 4 + rng.next() % 4;    // gates until a new y cluster
    float pending_y = 0.0f;                     // cluster target being entered
    bool  pending_impulse = false;
    bool  after_impulse = false;                // force a full 8-tick gap next

    s.gate_count = 0;
    for (int i = 0; i < WARP_COURSE_GATES && s.gate_count < WARP_MAX_GATES - 1; ++i) {
        // Gap in ticks: 8 early, tightening later — except right after an
        // impulse gate, whose climb needs the full gap.
        int gap = 8;
        if (i >= 34)      gap = 3 + rng.next() % 6;   // 3..8 late course
        else if (i >= 20) gap = 6 + rng.next() % 3;   // 6..8 mid course
        if (after_impulse) gap = 8;
        after_impulse = false;
        if (i > 0) tick_pos += (float)gap;

        WarpGate& g = s.gates[s.gate_count++];
        g.z     = tick_pos * WARP_TICK_Z;
        g.type  = WGATE_PLAIN;
        g.alive = true;

        // Worst-case frames to the NEXT gate: its gap is not generated yet,
        // so use the smallest gap its course region can roll. This is what
        // bounds every "the player must get there" decision below.
        int next_min_gap = 8;
        if (i + 1 >= 34)      next_min_gap = 3;
        else if (i + 1 >= 20) next_min_gap = 6;
        float next_min_frames = (float)next_min_gap * WARP_TICK_Z / speed_est;

        // ---- y clusters + impulse-gate placement --------------------------
        if (pending_impulse) {
            // The gate right after an impulse gate lands ON the new altitude.
            yBand = pending_y;
            pending_impulse = false;
        } else if (--cluster_left <= 0) {
            cluster_left = 4 + rng.next() % 5;
            // New cluster altitude on the 16-unit lattice within ±112, never
            // more than one impulse's worth away from the current band — and
            // an impulse's worth SHRINKS with escalated speed (fewer frames
            // between the impulse gate and the cluster), so bound the delta by
            // what V_KICK_CAP actually covers over the guaranteed 8-tick gap.
            // Lattice-preserving: every bound is floored to a multiple of 16.
            float n_frames = 8.0f * WARP_TICK_Z / speed_est;
            float d_max = 0.85f * V_KICK_CAP * coast_sum(n_frames);
            d_max = std::floor(d_max / 16.0f) * 16.0f;
            if (d_max > Y_CLUSTER_DELTA_MAX) d_max = Y_CLUSTER_DELTA_MAX;
            if (d_max < 48.0f) d_max = 48.0f;
            float target = (float)((rng.next() % 15) - 7) * 16.0f;
            target = clampf(target, -WARP_Y_RANGE, WARP_Y_RANGE);
            target = yBand + clampf(target - yBand, -d_max, d_max);
            // Direct (impulse-free) moves are bounded by what the ±1 stick
            // can actually cover before the next gate at the CURRENT speed —
            // late-course 3-tick gaps shrink this to zero, at which point any
            // altitude change must ride an impulse gate (whose following gap
            // is forced to a full 8 ticks above).
            float y_direct = v_reach(next_min_frames) * 0.8f;
            y_direct = std::floor(y_direct / 16.0f) * 16.0f;
            if (y_direct > WARP_Y_REACH) y_direct = WARP_Y_REACH;
            if (target > yBand + y_direct || target < yBand - y_direct) {
                // Off-axis beyond stick reach: THIS gate becomes the impulse
                // gate (still at the current band, en route), the cluster
                // starts at the next gate.
                g.type = (target > yBand) ? WGATE_UP : WGATE_DOWN;
                pending_y = target;
                pending_impulse = true;
                after_impulse = true;
            } else {
                yBand = target;
            }
        }
        g.y = yBand;

        // ---- x lattice walk, bounded by reachability -----------------------
        // Bounded by the real accel-ramp kinematics over the worst-case NEXT
        // gap (h_reach), 0.75 margin, quantized to the 16-unit lattice. steps
        // may legitimately be ZERO at escalated speed on a 3-tick gap — the
        // course goes straight there, exactly like the y freeze above.
        int steps = (int)(h_reach(next_min_frames) * 0.75f / 16.0f);
        if (steps > 4) steps = 4;
        float dx = (steps > 0)
                 ? (float)((rng.next() % (2 * steps + 1)) - steps) * 16.0f
                 : 0.0f;
        x = clampf(x + dx, -WARP_X_RANGE, WARP_X_RANGE);
        g.x = x;

        // ---- speed boosts ---------------------------------------------------
        if (g.type == WGATE_PLAIN && ++since_boost >= next_boost_at) {
            g.type = WGATE_BOOST;
            since_boost = 0;
            next_boost_at = 8 + rng.next() % 3;
            speed_est += speed_est * 0.125f;   // mirror the play-time boost
        }
    }

    // Victory gate: 12 ticks after the last regular gate, back toward centre
    // and on the current band so it is always reachable.
    WarpGate& v = s.gates[s.gate_count++];
    tick_pos += 12.0f;
    v.z     = tick_pos * WARP_TICK_Z;
    v.x     = clampf(x * 0.5f, -WARP_X_RANGE, WARP_X_RANGE);
    v.y     = yBand;
    v.type  = WGATE_VICTORY;
    v.alive = true;
}

// RAIL: 64 rim positions from smoothed signed LCG drifts, bounded so the track
// never drifts faster than the capped roll can follow (cap * step_frames,
// with a 0.55 safety margin).
void generate_rail_course(WarpState& s) {
    WarpRng rng(course_seed(s.round, s.play_count));

    int raw[WARP_RAIL_STEPS];
    for (int i = 0; i < WARP_RAIL_STEPS; ++i) raw[i] = (rng.next() % 97) - 48;

    int maxd = (int)(R_CAP * (float)s.rail_step_frames * 0.55f);
    if (maxd > 48) maxd = 48;

    int pos = 128;
    s.rail_track[0] = (uint8_t)pos;
    for (int i = 1; i < WARP_RAIL_STEPS; ++i) {
        int a = raw[i - 1], b = raw[i], c = raw[(i + 1) % WARP_RAIL_STEPS];
        int drift = (a + b + c) / 3;               // three-tap smoothing
        if (drift > maxd)  drift = maxd;
        if (drift < -maxd) drift = -maxd;
        pos = (pos + drift) & 255;
        s.rail_track[i] = (uint8_t)pos;
    }
}

// ---- shared event helpers ----------------------------------------------------

// Stamp the shatter chant clock (GameEngine::yes_beat_ms) so a STYLE_YES sign
// can ride the catch. The array is a 6-deep batch, not a ring; when a round's
// catches outrun it, start a fresh batch — signs from the previous batch are
// past their life by then (catches are ~1 s apart, sign life ≈ 1 s).
void stamp_yes_beat(GameEngine& engine, int t_abs) {
    if (engine.yes_beat_count >= GameEngine::YES_BEAT_MAX)
        engine.yes_beat_count = 0;
    engine.yes_beat_ms[engine.yes_beat_count++] = t_abs;
}

// THE YES RULE: every gate catch plays the sexy-yes voice TWICE, pitch
// escalating per boost section. Reading of the doc's "plays the voice twice;
// the sample alternates": BOTH samples are pushed per catch (the voice heard
// twice), and which of the two LEADS flips each catch (the alternation) —
// catch N is YES1(0.9)+YES2(0.7), catch N+1 is YES2(0.9)+YES1(0.7), so the
// dominant voice audibly alternates while the pair's two-voice texture stays.
// The 0.9/0.7 volume stagger is kept from the detune-era pair; the detune
// itself is gone — the two DIFFERENT samples are the variation now. Both
// voices carry the same escalating yes_pitch. NB the desktop SFX backend
// drops one-shot pitch today (known parity gap, tracked); the 3DS honours it.
void push_yes_pair(GameEngine& engine, WarpState& s) {
    const SfxId lead   = s.yes_flip ? SfxId::SEXY_YES2 : SfxId::SEXY_YES1;
    const SfxId second = s.yes_flip ? SfxId::SEXY_YES1 : SfxId::SEXY_YES2;
    s.yes_flip = !s.yes_flip;
    engine.sfx.push(lead,   SfxAction::ONE_SHOT, s.yes_pitch, 0.9f);
    engine.sfx.push(second, SfxAction::ONE_SHOT, s.yes_pitch, 0.7f);
}

void begin_win(GameEngine& engine, WarpState& s, int t_abs) {
    s.phase = WARP_PHASE_WIN;
    s.won = true;
    s.white_env = 0.0f;
    // The bonus round ENDS: the powerup cue, the same one the round began on
    // (Aesthetic Contract: a shared trigger point on both targets), and the
    // screen flash.
    engine.sfx.push(SfxId::POWER);
    flash_raise(engine.flash, FLASH_BONUS_END, t_abs);
    // Payout now (popups/1up land during the white-out); the LEVEL advance
    // waits for the envelope to finish (warp_level_end).
    add_warp_score(engine, s, t_abs);
    // "excellent" chorus, twice, slightly detuned (design doc).
    engine.sfx.push(SfxId::GROOVY, SfxAction::ONE_SHOT, 1.0f, 1.0f);
    engine.sfx.push(SfxId::GROOVY, SfxAction::ONE_SHOT, 0.97f, 0.85f);
    engine.trigger_shatter(SHATTER_STYLE_SLAM, "warp 5 levels!");
}

void begin_fail(GameEngine& engine, WarpState& s, int t_abs) {
    s.phase = WARP_PHASE_FAIL;
    s.fail_ctr = 0;
    s.fail_fade = 0.0f;
    // No award on a fail — fade out, then the normal +1 advance. The loss now
    // sounds like a loss: the in-house "aww" (assets/sfx/custom/aww), at full
    // volume, instead of the win's powerup cue, so the fail is audibly distinct
    // from begin_win. The dim flash stays.
    engine.sfx.push(SfxId::BONUS_LOSE, SfxAction::ONE_SHOT, 1.0f, 1.0f);
    flash_raise(engine.flash, FLASH_BONUS_END, t_abs, 0.3f);
}

// Gate-catch accuracy score: centre-best 750/500/250 by |dx|+|dy| thirds
// (the arcade reference's inverted table is recorded as a bug in the design
// doc).
int catch_points(float err) {
    if (err <= CATCH_ERR_MAX * (1.0f / 3.0f)) return 750;
    if (err <= CATCH_ERR_MAX * (2.0f / 3.0f)) return 500;
    return 250;
}

// ---- per-step sims ------------------------------------------------------------

void step_gates(GameEngine& engine, WarpState& s,
                bool left, bool right, int t_abs) {
    // -- horizontal: the player's axis (left = -x; renderers mirror to taste) --
    if (left)  s.axis_x.vel -= H_ACC;
    if (right) s.axis_x.vel += H_ACC;
    s.axis_x.vel = clampf(s.axis_x.vel * H_FRIC, -H_CAP, H_CAP);
    s.axis_x.value += s.axis_x.vel;
    if (s.axis_x.value >  H_POS_MAX) { s.axis_x.value =  H_POS_MAX; s.axis_x.vel = 0; }
    if (s.axis_x.value < -H_POS_MAX) { s.axis_x.value = -H_POS_MAX; s.axis_x.vel = 0; }

    // -- vertical: PURE INERTIAL stick control (rules fidelity — the second
    // proof verdict, docs/design/bonus_rounds.md). The reference law (the later port
    // Inertcon / the arcade reference ixcon-iycon) is accel while held, friction, velocity
    // cap, position clamp — NOTHING tracks a gate and nothing recenters. The
    // vertical AUTOPILOT this sim used to run when no key was held is REMOVED,
    // and with it the coast's target-crossing brake (both were assists keyed
    // on course knowledge the player is not supposed to get for free): the
    // ship now stays wherever the player leaves it.
    //
    // Impulse-gate SHOVES stay — they are gate effects, not assists (the
    // reference's zup/zdn set vertical velocity exactly like this). A kick
    // legitimately exceeds V_CAP; while that coast is live the cap is not
    // applied — V_FRIC bleeds it back down, and stick accel still applies on
    // top (push along the kick to extend it, against it to brake). Position
    // clamps to ±V_POS_MAX unconditionally.
    //
    // SIGN EVIDENCE (up = +y): there is no per-backend warp projection left to
    // disagree about — both backends consume ONE shared CPU projection into the
    // 1.3333 x 1.0 UI box (railgeom::View::project, rail_geometry.h, maps
    // uy = ANCHOR_Y + (wy - sy) * s; the gate layer's compressed form is in
    // warp_geometry.cpp, cy = ANCHOR_Y + GATE_LATERAL_K * (gate.y - v.sy) * s).
    // The ship offset is SUBTRACTED inside the builder (View::sx/sy), so nothing
    // downstream translates by axis_y. That box is y-UP on both targets — the
    // 3DS draws it through Mtx_OrthoTilt(0, UI_W, 0, 1, ...) (c3d/10_ui_seam.inc,
    // "the ONE projection this screen uses"), the PC through orthoUiVk
    // (renderer_vk.cpp, "0..1.3333 x 0..1.0 with y UP") — so a larger world y is
    // a larger uy is higher on screen, on both. WGATE_UP's impulse kick already
    // sets vel = +mag under the same convention; both renderers and the sim agree.
    bool coasting = (s.axis_y.vel > V_CAP + 0.01f) || (s.axis_y.vel < -V_CAP - 0.01f);
    if (s.in_up)   s.axis_y.vel += V_ACC;
    if (s.in_down) s.axis_y.vel -= V_ACC;
    s.axis_y.vel *= V_FRIC;
    if (!coasting) s.axis_y.vel = clampf(s.axis_y.vel, -V_CAP, V_CAP);
    s.axis_y.value = clampf(s.axis_y.value + s.axis_y.vel, -V_POS_MAX, V_POS_MAX);

    // -- world scroll: every gate approaches by `speed` per step ---------------
    s.dist += s.speed;
    for (int i = s.next_gate; i < s.gate_count; ++i) s.gates[i].z -= s.speed;

    // -- catch test as the front gate crosses the near plane -------------------
    if (s.next_gate >= s.gate_count) { begin_win(engine, s, t_abs); return; }
    WarpGate& g = s.gates[s.next_gate];
    if (g.z > WARP_CATCH_Z) return;

    float dx = s.axis_x.value - g.x;
    float dy = s.axis_y.value - g.y;
    float adx = dx < 0 ? -dx : dx;
    float ady = dy < 0 ? -dy : dy;

    if (adx > WARP_CATCH_BOX || ady > WARP_CATCH_BOX) {
        // Missing ONE gate fails the round instantly (reference rule).
        g.alive = false;
        begin_fail(engine, s, t_abs);
        return;
    }

    // ---- caught -------------------------------------------------------------
    g.alive = false;
    s.next_gate += 1;

    if (g.type == WGATE_VICTORY) {
        begin_win(engine, s, t_abs);
        return;
    }

    if (g.type == WGATE_BOOST) {
        // speed += speed/8, escalation section up, yes pitch steps up.
        s.speed += s.speed * 0.125f;
        s.section += 1;
        s.yes_pitch = 1.0f + YES_PITCH_STEP * (float)s.section;
        engine.trigger_shatter(SHATTER_STYLE_STREAK, "speed boost");
    } else if (g.type == WGATE_UP || g.type == WGATE_DOWN) {
        // Kick solved from the altitude delta the next gate actually asks for
        // and the frames the current speed leaves to get there (8% margin —
        // ≤ 7.7 units on the largest ±96 cluster delta, inside the ±8 catch
        // box; the player's own stick trims the rest, there is no autopilot
        // brake any more). Overshoots the stick clamp and coasts; see the
        // vertical sim above.
        if (s.next_gate < s.gate_count) {
            const WarpGate& ng = s.gates[s.next_gate];
            float dyn = ng.y - s.axis_y.value;
            float n_frames = (ng.z - WARP_CATCH_Z) / s.speed;
            if (n_frames < 1.0f) n_frames = 1.0f;
            float mag = clampf(1.08f * (dyn < 0 ? -dyn : dyn) / coast_sum(n_frames),
                               V_IMP_MIN, V_KICK_CAP);
            s.axis_y.vel = (g.type == WGATE_UP) ? mag : -mag;
        }
    }

    // Accuracy score through the ordinary scoring path (50k 1-up rule intact),
    // popup via the pixel-shatter path — drama escalates with accuracy.
    int pts = catch_points(adx + ady);
    engine.award_score(t_abs, pts);
    if (g.type != WGATE_BOOST) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%d", pts);
        engine.trigger_shatter(pts == 750 ? SHATTER_STYLE_SLAM
                             : pts == 500 ? SHATTER_STYLE_WAVE
                                          : SHATTER_STYLE_CASCADE, buf);
    }

    s.catch_ms = t_abs;
    s.catch_x = g.x;
    s.catch_y = g.y;
    s.catch_score = pts;
    stamp_yes_beat(engine, t_abs);
    push_yes_pair(engine, s);
    // A caught gate flashes; the 750-point bullseye flashes hardest.
    flash_raise(engine.flash, FLASH_BONUS_CATCH, t_abs, pts == 750 ? 1.5f : pts == 500 ? 1.2f : 1.0f);
}

void step_rail(GameEngine& engine, WarpState& s,
               bool left, bool right, int t_abs) {
    // -- inertial roll around the rim (0..256, wraps; cap 4/256 per frame) ----
    if (left)  s.roll.vel -= R_ACC;
    if (right) s.roll.vel += R_ACC;
    s.roll.vel = clampf(s.roll.vel * R_FRIC, -R_CAP, R_CAP);
    s.roll.value += s.roll.vel;
    while (s.roll.value < 0.0f)    s.roll.value += 256.0f;
    while (s.roll.value >= 256.0f) s.roll.value -= 256.0f;

    s.dist += s.speed;

    // -- step clock -------------------------------------------------------------
    s.rail_frame_ctr += 1;
    s.rail_step_frac = (float)s.rail_frame_ctr / (float)s.rail_step_frames;
    if (s.rail_frame_ctr < s.rail_step_frames) return;
    s.rail_frame_ctr = 0;
    s.rail_step_frac = 0.0f;

    // -- evaluate the step just completed ----------------------------------------
    float d = rim_dist(s.roll.value, (float)s.rail_track[s.rail_step]);
    if (d <= WARP_RAIL_WINDOW) {
        s.tail += 0.5f;
        if (s.tail > WARP_TAIL_MAX) s.tail = WARP_TAIL_MAX;
        // Per-step score by tail band: 1 / 4 / 8 / 30 (quarters of the meter).
        int pts = (s.tail >= 24.0f) ? 30 : (s.tail >= 16.0f) ? 8
                : (s.tail >= 8.0f)  ? 4  : 1;
        engine.award_score(t_abs, pts);
    } else {
        s.tail -= 1.0f;
        if (s.tail <= 0.0f) {
            s.tail = 0.0f;
            begin_fail(engine, s, t_abs);   // tail empty = fail
            return;
        }
    }

    s.rail_step += 1;
    s.section = s.rail_step >> 3;   // escalation index for the renderers
    if (s.rail_step >= WARP_RAIL_STEPS) {
        begin_win(engine, s, t_abs);   // survived all 64 steps
    }
}

// One 16 ms step at absolute timestamp t_abs.
void move_warp_step(GameEngine& engine, WarpState& s,
                    bool left, bool right, int t_abs) {
    if (s.warp_level_end) return;
    // The screen flash decays per SIM step here exactly as game_tick does
    // in gameplay (fx_events.h): same 16 ms cadence, same curve.
    flash_tick(engine.flash);

    switch (s.phase) {
    case WARP_PHASE_WIN:
        // White-out crescendo, then the level handover. Advance semantics are
        // the debugged outcome contract: win vaults +5 total (+4 on top of the
        // normal +1) below the skip ceiling, +1 at/above it (the round still
        // paid out), capped at 100 — which is exactly the ENDING trigger and
        // must NOT clamp to 99 (a winning late warp has to be able to finish
        // the game; see the history in git for the full derivation).
        s.white_env += WHITE_ENV_STEP;
        if (s.white_env >= 1.0f) {
            s.white_env = 1.0f;
            engine.current_level +=
                (engine.current_level >= WARP_SKIP_CEILING_LEVEL) ? 1 : 5;
            if (engine.current_level > 100) engine.current_level = 100;
            s.warp_level_end = true;
        }
        return;

    case WARP_PHASE_FAIL: {
        // Lose fade: normal rate through the "aww", then 2x so the post-sound
        // tail is halved (see BONUS_LOSE_SOUND_STEPS). Completes ~205 steps
        // (≈3.3 s) instead of 300, then the normal +1.
        s.fail_ctr += 1;
        if (s.fail_ctr <= BONUS_LOSE_SOUND_STEPS) {
            s.fail_fade = (float)s.fail_ctr / (float)FAIL_FADE_STEPS;
        } else {
            s.fail_fade = (float)BONUS_LOSE_SOUND_STEPS / (float)FAIL_FADE_STEPS
                        + 2.0f * (float)(s.fail_ctr - BONUS_LOSE_SOUND_STEPS)
                                  / (float)FAIL_FADE_STEPS;
            if (s.fail_fade > 1.0f) s.fail_fade = 1.0f;
        }
        if (s.fail_fade >= 1.0f) {
            engine.current_level += 1;
            s.warp_level_end = true;
        }
        return;
    }

    default:
        break;
    }

    // Defensive timeout (see constant note): force the lose path.
    if (t_abs - s.warp_level_time >= WARP_TIMEOUT_MS) {
        begin_fail(engine, s, t_abs);
        return;
    }

    if (s.round == WARP_ROUND_RAIL) step_rail(engine, s, left, right, t_abs);
    else                            step_gates(engine, s, left, right, t_abs);
}

} // namespace

// =============================================================================
// Public entry points (seam-stable signatures — see warp.h).
// =============================================================================

void init_warp(GameEngine& engine, WarpState& state, int time) {
    state = WarpState{};   // full reset; fixed arrays value-initialize
    state.warp_level_time = time;
    state.sim_time = time;
    // The bonus round BEGINS: the powerup cue + the screen flash, raised here
    // (the round's first frame on both targets) and nowhere else.
    engine.sfx.push(SfxId::POWER);
    flash_raise(engine.flash, FLASH_BONUS_BEGIN, time);

    // THE ROUND SLOT TABLE: the round rotates with the 16-level colour band
    // (design doc). This is the extension point -- a new round is a new
    // WARP_ROUND_* id and its entries here, and nothing else in init_warp
    // changes. Today both slots that exist alternate every band.
    static const int kRoundByBand[4] = {
        WARP_ROUND_GATES, WARP_ROUND_RAIL, WARP_ROUND_GATES, WARP_ROUND_RAIL,
    };
    state.round = kRoundByBand[(engine.current_level >> 4) & 3];

    // Persistent per-round-type play counter (GameEngine::warp_plays — reset
    // with the run in init_gameplay, deliberately NOT in reinit_gameplay,
    // which runs after every warp): count & 7 picks the course, the raw count
    // scales base speed.
    state.play_count = engine.warp_plays[state.round];
    engine.warp_plays[state.round] += 1;
    int pc = state.play_count;
    if (pc > 7) pc = 7;

    if (state.round == WARP_ROUND_RAIL) {
        state.rail_step_frames = RAIL_BASE_STEP_FRAMES - 2 * pc;
        if (state.rail_step_frames < RAIL_MIN_STEP_FRAMES)
            state.rail_step_frames = RAIL_MIN_STEP_FRAMES;
        state.base_speed = RAIL_BACKDROP_SPEED;   // backdrop scroll only on RAIL
                                                  // (own constant — see its block)
        state.speed = state.base_speed;
        state.tail = TAIL_START;
        state.roll.value = 128.0f;            // start on the track's origin
        generate_rail_course(state);
    } else {
        state.base_speed = GATE_BASE_SPEED * (1.0f + SPEED_PER_PLAY * (float)pc);
        state.speed = state.base_speed;
        generate_gate_course(state);
        // Open just below the river (see GATES_START_Y).
        state.axis_y.value = GATES_START_Y;
    }

    // Backdrop is the renderers' business; the sim just guarantees black.
    engine.bg_color[0] = 0.0f;
    engine.bg_color[1] = 0.0f;
    engine.bg_color[2] = 0.0f;
    engine.bg_color[3] = 1.0f;
}

void move_warp(GameEngine& engine, WarpState& state,
               bool left, bool right, bool up, bool down, int time) {
    // Vertical seam: mirror this call's held flags into the state (step_gates
    // reads them per step — they are the ONLY vertical control law; RAIL
    // ignores them) and latch in_vert_live the first time real vertical
    // input arrives this round — renderer hint state only (see warp.h).
    state.in_up   = up;
    state.in_down = down;
    if (up || down) state.in_vert_live = true;

    // Same 16 ms fixed-step accumulator the old sim used (the reference build MoveWarp),
    // with a BOUND on how far one call may catch up. The accumulator is fed
    // the WALL CLOCK, so any interval the front-end does not step it -- a
    // pause, a swapchain rebuild, the 20-30 s texture warm -- accrues as debt
    // and is then replayed in a single frame: an 8 s pause ran ~500 steps at
    // once and the round ended the instant it resumed. Beyond the bound the
    // debt is DROPPED, not banked (state.sim_time re-bases on `time`), which
    // is the standard spiral-of-death guard and the only sane reading of "the
    // sim was not running then". Well clear of any real frame: at 60 Hz a
    // call steps once, and even a 30 Hz dip steps twice.
    constexpr int MAX_CATCHUP_STEPS = 8;      // 128 ms
    if (time - state.sim_time > MAX_CATCHUP_STEPS * 16)
        state.sim_time = time - 16;
    while (state.sim_time + 16 <= time && !state.warp_level_end) {
        state.sim_time += 16;
        move_warp_step(engine, state, left, right, state.sim_time);
    }
}

void resume_warp(WarpState& state, int pausedMs) {
    if (pausedMs <= 0) return;
    state.sim_time        += pausedMs;
    state.warp_level_time += pausedMs;
}

void add_warp_score(GameEngine& engine, WarpState& state, int time) {
    // Design contract: the win pays 20,000 points through the ordinary scoring
    // path (award_score keeps the 50k 1-up threshold rule, same as every other
    // scoring site) PLUS one unconditional extra life — replacing the old
    // port's 50,000 + time/3. Latched so a double call cannot double-pay.
    if (state.scored) return;
    state.scored = true;
    engine.award_score(time, 20000);
    engine.init_1up(time);
}

} // namespace ts
