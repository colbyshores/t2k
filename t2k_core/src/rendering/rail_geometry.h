#pragma once

// ============================================================================
// rail_geometry.h — the RAIL bonus round's PURE MATH, shared by both backends.
//
// The RAIL round (docs/design/bonus_rounds.md, "RAIL — the particle rave
// tunnel"; frozen reference: docs/validation/rail-v5-*.png + rail-v6-retire-*)
// is entirely a DOT CLOUD: ~20 receding rings of jittered dots whose 16 rim
// sectors ARE the FFT's bands[], a stochastic wall emitter driven by those
// bands, a green (SPIKE_COLOR) dot-swath winding along the wall, and a comet
// whose particle-shower tail LENGTH is the life meter. Everything below is the
// build half of the build/submit seam the GL backend has always had — lifted
// here VERBATIM so the Citro3D twin consumes the same math and the two targets
// cannot drift (TARGET PARITY IS POLICY; contract: docs/design/rail_c3d_twin.md
// decision D8).
//
// Discipline (DOCTRINE.md "C with classes"):
//   * no GL, no citro3d, no renderer includes — plain data + free functions;
//   * fixed pools with hard caps and a drop-not-grow guard, zero heap, zero
//     virtuals, -fno-exceptions/-fno-rtti clean;
//   * deterministic hash (hash01) instead of stored per-dot state, so the
//     scatter streams WITH the wall and costs no memory.
//
// The backends only SUBMIT: GL walks the pool through renderDot in UI space;
// C3D CPU-projects each dot into per-eye particleVbo quads (D7's stereo shear
// rides RailDot::z, which GL never reads — so GL stays bit-identical).
// ============================================================================

#include <cstdint>

#include "../audio/audio_features.h"   // AUDIO_BAND_COUNT — the 16 rim sectors

namespace ts {

struct GameEngine;
struct WarpState;

namespace railgeom {

// ---- UI-space CPU perspective ----------------------------------------------
// The whole warp screen is CPU-projected into a 1.3333 x 1.0 UI box:
//     s(z) = focal / (z + eyeZ);  ux = ANCHOR_X + (wx - sx)·s
// (Shared with the GL warp's gates/river/stars, which is why the box constants
// live here rather than beside the RAIL block: one definition, both backends.)
constexpr float UI_W     = 1.3333f;
constexpr float ANCHOR_X = UI_W * 0.5f;
constexpr float ANCHOR_Y = 0.44f;
constexpr float EYE_Z    = 2.0f;
constexpr float FOCAL    = 0.75f;
constexpr float TWO_PI   = 6.2831853f;

// The pre-WYSIWYG TELEPHOTO law. GATES moved its eye to the catch plane so the
// hoop envelops the screen before judgment (bonus_rounds.md, fifth river
// verdict); RAIL kept the old law wholesale because its dot-tunnel framing is
// user-approved and there is no catch box here to be honest about. The design
// block sanctions exactly this gates-only split — RAIL's View parks here.
constexpr float TELE_FOCAL = 0.344f;
constexpr float TELE_EYE_Z = 40.0f;

struct View {
    float sx = 0.0f, sy = 0.0f;   // ship offset — subtracted, so the WORLD scrolls
    float focal = FOCAL;          // near law by default; RAIL parks these at
    float eyeZ  = EYE_Z;          // TELE_FOCAL/TELE_EYE_Z (plain data, no dispatch)
    float scaleAt(float z) const { return focal / (z + eyeZ); }
    void project(float wx, float wy, float z, float& ux, float& uy) const {
        const float s = scaleAt(z);
        ux = ANCHOR_X + (wx - sx) * s;
        uy = ANCHOR_Y + (wy - sy) * s;
    }
};

// Outcome-envelope tint, applied to every colour this screen emits: wm mixes
// toward white (the win white-out crescendo), gain scales intensity (>1 while
// the white-out blooms, ->0 through the fail fade). Riding vertex colour on
// geometry already being drawn is the level-transition flash rule — the same
// mechanism BOTH backends can afford, so the two read as one game.
struct EnvTint {
    float wm   = 0.0f;
    float gain = 1.0f;
    void apply(float& r, float& g, float& b, float& a) const {
        r += (1.0f - r) * wm;
        g += (1.0f - g) * wm;
        b += (1.0f - b) * wm;
        a *= gain;
        if (a > 1.0f) a = 1.0f;
        if (a < 0.0f) a = 0.0f;
    }
};

inline float wclamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

inline void whiten(float w, float& r, float& g, float& b) {
    r += (1.0f - r) * w;
    g += (1.0f - g) * w;
    b += (1.0f - b) * w;
}

// Deterministic per-index hash (the texture DSL's LCG multiplier), for star
// placement and sparkle flicker — no storage, stable frame to frame.
inline float hash01(uint32_t n) {
    n = n * 0x015a4e35u + 0x9E3779B9u;
    n ^= n >> 13;
    n *= 0x85ebca6bu;
    n ^= n >> 16;
    return (float)(n & 0xFFFFFFu) / 16777216.0f;
}

// RAIL rim-position -> angle. 0..256 rim units around the tunnel; position 128
// (the track origin and the roll start) lands at the BOTTOM, arcade-style.
inline float rimTheta(float p) { return p * (TWO_PI / 256.0f) + 1.5707963f; }

// ---- RAIL particle tunnel ---------------------------------------------------
// The tunnel is DOTS (the arcade-reference rail look): RAIL_RINGS receding
// rings of RAIL_RING_DOTS soft additive dots converging on a dark centre. The
// 16 rim sectors map 1:1 onto the FFT's bands[] — the tunnel IS the spectrum
// analyzer (the rave is real). Every dot goes through RailDotPool: one fixed
// pool with a hard cap and a drop-not-grow guard (the DynamicBatch rule),
// filled by railBuildDots as pure math and submitted in one loop by whichever
// backend is compiled in.
constexpr float RAIL_R            = 40.0f;   // tunnel wall radius, world units
constexpr int   RAIL_RINGS        = 20;      // rings deep
constexpr int   RAIL_RING_DOTS    = 48;      // dots around each ring
constexpr float RAIL_RING_SPACING = 15.0f;   // z-units between rings
constexpr float RAIL_Z_NEAR       = 6.0f;    // nearest ring plane
constexpr float RAIL_FLOW_BASE    = 0.028f;  // z-units/ms: silence-floor stream
constexpr float RAIL_FLOW_RMS     = 0.075f;  // + rms × this (forward flow)
constexpr float RAIL_RING_PULSE   = 0.06f;   // beat ring-radius pulse depth …
constexpr float RAIL_RING_PULSE_SAFE = 0.02f;// … capped under audio_safe_mode
constexpr float RAIL_HUE_SPREAD   = 0.10f;   // per-sector rainbow tilt — the
                                             // BASE of each sector's hue target
                                             // (see the RAIL_HUE_* block below)
// ---- FFT-LERPED SECTOR HUES (fourth rail verdict, change 1) -----------------
// The sector rainbow is LIVE: each of the 16 sectors carries a persistent
// displayed hue offset (RailFxState::hueOff) that LERPS toward a target driven
// by its own band, plus a slow global hue phase integrated from rms
// (RailFxState::huePhase — the same integration law the river's melt uses).
//
//   THE COLOUR LAW (per frame, wall-clock dt, frame-rate independent):
//     target[i] = RAIL_HUE_SPREAD·sin(2πi/16)          (the base spatial tilt)
//               + RAIL_HUE_BAND_GAIN · bands[i]        (loud band rotates its
//                                                       sector further round)
//     off[i]   += (target[i] − off[i]) · (1 − exp(−dt / RAIL_HUE_LERP_TAU_S))
//     phase    += (RAIL_HUE_PHASE_MIN + RAIL_HUE_PHASE_RMS · rms) · dt
//     sectorHue[i] = railExcludeTrackHue(levelHue + phase + off[i])
//                    (the green exclusion — fifth verdict item 1, block below —
//                     runs on the DISPLAYED sum only; off[] stays unclamped)
//
// The lerp is the musicality: a band spike pulls its sector's hue over ~τ and
// releases it back to the resting rainbow as the band decays — hues BREATHE
// with the mix, they never snap, and silence relaxes to the old static tilt
// (drifting only at the slow PHASE_MIN floor). audio_safe_mode caps the shift
// DEPTH (band gain) and the phase RATE the way every pulse depth is capped.
// This is the same RECORDED hue-is-identity exception class as the river's
// melt (docs/design/bonus_rounds.md, fourth rail verdict): on RAIL the hues
// shift with the music by design. Everything carrying the rainbow — lattice
// dots, emitter particles, the comet head/tail — reads sectorHue, so the
// whole scene shifts together.
constexpr float RAIL_HUE_LERP_TAU_S     = 0.30f;  // displayed-hue lerp time const
constexpr float RAIL_HUE_BAND_GAIN      = 0.12f;  // hue turns at band = 1 …
constexpr float RAIL_HUE_BAND_GAIN_SAFE = 0.05f;  // … capped under audio_safe_mode
constexpr float RAIL_HUE_PHASE_MIN      = 0.002f; // global phase floor, turns/s
constexpr float RAIL_HUE_PHASE_RMS      = 0.030f; // + rms × this, turns/s …
constexpr float RAIL_HUE_PHASE_RMS_SAFE = 0.010f; // … capped under audio_safe_mode

// ---- GREEN IS RESERVED FOR THE TRACK (fifth rail verdict, item 1) -----------
// The track swath is SPIKE_COLOR — the shared gameplay-legible green — and no
// live-shifting wall/comet hue may ever wander into its family, or the drift
// could paint a wall sector track-green right beside the swath and obfuscate
// where the player must go. Gameplay legibility outranks the rave; this is the
// same reasoning that made the spikes green in the first place.
//
// RAIL_TRACK_HUE is SPIKE_COLOR's own HSV hue, hard-derived from
// line_geometry.h's constant (R 0.25, G 1.00, B 0.35): max = G, min = R,
// delta = 0.75, so
//   h = ((B − R)/delta + 2) / 6 = ((0.35 − 0.25)/0.75 + 2) / 6
//     = 2.13333/6 = 0.3555556 turns   (= 128.0°)
// in webHsv2rgb's convention (hue in TURNS, 0..1, wrapped internally).
constexpr float RAIL_TRACK_HUE       = 0.3555556f;
// Forbidden-arc HALF-width, in hue turns (35° each side of track green — the
// arc covers everything a 240p eye could read as "the green one").
constexpr float RAIL_GREEN_EXCLUSION = 35.0f / 360.0f;

// The exclusion law: a displayed hue that lands inside the arc is pushed to
// the NEAREST arc edge. It runs at the FINAL displayed-hue computation only —
// AFTER level hue + global phase + per-sector offset are summed (see
// railSectorPalette) — so no combination of drift terms can ever land in the
// arc, while the LERP STATE (RailFxState::hueOff/huePhase) stays untouched:
// clamping the state would make hues STICK to the boundary instead of passing
// behind it and re-emerging on the other side. Push-to-nearest-edge is
// deterministic and CONTINUOUS at both arc edges (a hue drifting toward the
// arc slides onto the near edge with no jump); reflecting or re-randomising
// would flicker as a hue crossed the arc's centre.
float railExcludeTrackHue(float h);

constexpr int   RAIL_TRACK_Q      = 8;       // track-swath quanta per course step
constexpr int   RAIL_TRACK_DOTS   = 4;       // swath dots per quantum

// ---- COMET TAIL — A LIVE EMITTER, NOT A DRAWING (fifth verdict, item 2) ----
// The fourth-verdict tail was re-plotted every frame from the roll history,
// and the user's frame showed it reading as a STATIC BEAM. Replaced with a
// dedicated fixed particle pool (drop-not-grow, beside RailDotPool and
// RailEmitterPool): particles SPAWN continuously at the comet head's rim
// seat, inherit the outward radial velocity AND the head's tangential
// (roll-swing) velocity at birth — a hard roll flings the stream sideways —
// then live their own lives: per-particle jitter, fade + shrink + slight
// deceleration over a hash-varied 0.4–0.9 s lifetime. The tail BOILS.
//
// THE METER RULE SURVIVES — how W.tail maps to visible length: outward birth
// speed is chosen per particle as
//     v0 = REACH / (life · (1 − DECEL/2)),   REACH = W.tail · RAIL_TAIL_REACH_K
// where (1 − DECEL/2) is the mean of the linear speed ramp 1 → (1 − DECEL)
// a particle rides over its life, so ∫v·dt = REACH exactly: every particle
// dies ≈ REACH world units out REGARDLESS of its own lifetime, and the
// shower's visible extent is W.tail · 1.8 world units — the same reach the
// old drawn tail had (REACH_K = the old per-step 1.8). Short-lived particles
// just fly faster; the tail edge stays the meter. Spawn RATE also scales
// with the meter (a long tail is denser as well as longer) plus a burst on
// the beat's rising edge. The bright HEAD is untouched (fourth verdict,
// change 3 — it IS the avatar).
constexpr int   RAIL_TAIL_MAX           = 256;   // hard pool cap
constexpr float RAIL_TAIL_SPAWN_BASE_HZ = 60.0f; // spawn floor, particles/s …
constexpr float RAIL_TAIL_SPAWN_RATE    = 170.0f;// … + this × (W.tail / cap):
                                                 // the meter buys density too
constexpr int   RAIL_TAIL_BEAT_N        = 20;    // beat-edge burst …
constexpr int   RAIL_TAIL_BEAT_N_SAFE   = 8;     // … capped under audio_safe_mode
constexpr float RAIL_TAIL_LIFE_LO       = 0.4f;  // life seconds, 0.4 .. 0.9
constexpr float RAIL_TAIL_LIFE_SPAN     = 0.5f;
constexpr float RAIL_TAIL_REACH_K       = 1.8f;  // world units of reach per
                                                 // meter point (= old OUT_STEP)
constexpr float RAIL_TAIL_DECEL         = 0.55f; // speed ramps to 1−this at death
constexpr float RAIL_TAIL_BEAT_STRETCH  = 0.40f; // beat kick: extra birth
                                                 // speed (= extra reach), × beatK
constexpr float RAIL_TAIL_SWING_K       = 1.0f;  // tangential inheritance gain
constexpr float RAIL_TAIL_JIT_POS       = 2.0f;  // birth rim jitter, rim units
constexpr float RAIL_TAIL_JIT_VEL       = 9.0f;  // per-axis velocity jitter, u/s
constexpr float RAIL_TAIL_JIT_Z         = 4.0f;  // birth depth jitter span
constexpr float RAIL_TAIL_HEAT          = 0.75f; // whiten at birth (hot at the
                                                 // head), cooling over life
// Roll-swing conversion: W.roll.vel is rim-units per 16 ms sim step
// (warp.cpp R_CAP = 4), so tangential world speed at radius r is
// vel × 62.5 steps/s × (2π/256) rad per rim-unit × r.
constexpr float RAIL_TAIL_ROLL_TO_RAD_S = 62.5f * (TWO_PI / 256.0f);
constexpr float RAIL_TAIL_SIZE0  = 0.075f; // dot size at birth, × rim UI scale…
constexpr float RAIL_TAIL_SIZE1  = 0.020f; // …shrinking to this at death
constexpr float RAIL_TAIL_R0     = 0.92f;  // avatar radius, × ringR (the rim seat)
constexpr float RAIL_HEAD_HALO_K = 0.17f;  // head halo size, × rim UI scale
constexpr float RAIL_HEAD_MID_K  = 0.09f;  // whitened mid glow
constexpr float RAIL_HEAD_CORE_K = 0.045f; // white-hot core
constexpr float RAIL_HEAD_BEAT   = 0.25f;  // head beat-pulse depth (× beatK)
constexpr float RAIL_Z_PLAYER    = 8.0f;   // the player plane (head + tail
                                           // births + the track's current step)

// ---- END-OF-ROUND RETIREMENT (sixth rail verdict) ---------------------------
// On WIN or FAIL the track swath and the comet retire gracefully instead of
// cutting/fading in place, riding INSIDE the sim's existing outcome windows
// (win white-out: 90 × 16 ms sim steps ≈ 1.44 s; fail fade: 300 steps = 4.8 s
// — warp.cpp WHITE_ENV_STEP / FAIL_FADE_STEPS), so RAIL_RETIRE_S is sized
// under the SHORTER (win) window and the retreat always completes before the
// round ends. A retirement clock t_r (0..1, RailFxState::retireT, wall-clock
// integrated) starts at the PLAY→WIN/FAIL transition:
//   · THE SWATH RETREATS INTO THE CENTRE: each swath dot's alpha is gated by
//     a receding cutoff front over its normalized distance from the player
//     plane — nearest-to-player dies FIRST, so the visible trail recedes down
//     the tunnel into the vanishing point. The front is softened over
//     RAIL_RETIRE_SOFT of the normalized depth (never a hard shear), and the
//     cutoff runs −SOFT → 1 so t_r = 0 multiplies by exactly 1.0 — the
//     PLAY-phase look is bit-identical (the frozen rail-v5 look).
//   · THE COMET COLLAPSES: spawning stops at the transition (railTailStep
//     gates on phase == PLAY); live tail particles age RAIL_RETIRE_TIP_K
//     times faster per unit of normalized distance from the head (tip-first —
//     the tail shrinks back into the head); the head's three dots fade LAST,
//     from t_r = RAIL_RETIRE_HEAD_T to extinction at t_r = 1.
// Coverage recedes rather than dims, so the motion reads THROUGH the win
// white-out (whitening moves colour, not alpha) and LEADS the 4.8 s fail dim.
constexpr float RAIL_RETIRE_S      = 1.0f;   // retirement clock length, seconds
constexpr float RAIL_RETIRE_SOFT   = 0.10f;  // front fade width, normalized depth
constexpr float RAIL_RETIRE_HEAD_T = 0.60f;  // head fade starts here on the clock
constexpr float RAIL_RETIRE_TIP_K  = 4.0f;   // extra tail age-rate × dist-from-head

constexpr int   RAIL_DOT_MAX      = 4000;    // hard cap on dots per frame

// RANDOMIZED, NOT A LATTICE (user verdict on the uniform first proof,
// 2026-08-19): every ring dot carries per-dot deterministic jitter — angle
// within its OWN sector (the band mapping stays truthful), depth within its
// ring gap, size variance, and a slow twinkle phase — all seeded by hash01 on
// the dot's (ring, slot) identity. Nothing is stored: the offsets are constant
// for a dot's whole life, so the scatter streams WITH the wall instead of
// shimmering, and the tunnel silhouette (the circle) survives untouched.
constexpr float RAIL_JIT_DEPTH      = 0.90f; // ± half this, in ring gaps of z
constexpr float RAIL_JIT_SIZE_LO    = 0.55f; // size varies 0.55x .. 1.45x
constexpr float RAIL_JIT_SIZE_SPAN  = 0.90f;
constexpr float RAIL_TWINKLE_DEPTH  = 0.30f; // brightness swing (±, slow)
constexpr float RAIL_TWINKLE_HZ_LO  = 0.25f; // per-dot oscillation rate …
constexpr float RAIL_TWINKLE_HZ_SPAN = 0.55f; // … 0.25 .. 0.80 Hz

// One built dot, ready to submit. x/y/size are UI-space (the 1.3333 x 1.0 box
// above); z is the dot's WORLD depth down the tunnel — write-only on GL (which
// draws straight into the UI ortho), and the input to the C3D twin's per-eye
// stereo shear (rail_c3d_twin.md D7). Carrying it costs GL one store per dot
// and keeps the two backends on ONE builder.
struct RailDot { float x, y, size, r, g, b, a, z; };
struct RailDotPool {
    RailDot d[RAIL_DOT_MAX];
    int n = 0;
    // Drop-not-grow: past the cap dots are silently dropped — the
    // DynamicBatch rule every sibling pool here carries.
    inline void emit(float x, float y, float size,
                     float r, float g, float b, float a, float z) {
        if (n >= RAIL_DOT_MAX) return;
        RailDot& o = d[n++];
        o.x = x; o.y = y; o.size = size;
        o.r = r; o.g = g; o.b = b; o.a = a; o.z = z;
    }
};

// ---- RAIL stochastic emitter (THE MUSIC IS THE EMITTER) ---------------------
// A fixed pool of short-lived wall particles. Per frame each of the 16 wall
// sectors emits at a rate proportional to ITS band level (a band spike = a
// visible burst from that sector of the wall); the beat's rising edge fires an
// additional all-sector burst. Particles are born ON the wall at a random
// angle inside their sector at a random depth, stream toward the eye
// inheriting the tunnel flow speed (with per-particle velocity jitter), drift
// slightly outward, and fade over a hash-varied 0.6–1.4 s life. Randomness is
// the LCG/hash01 idiom: the spawn DECISION is frame-seeded (engine.time), the
// spawn PARAMETERS are sequence-seeded (spawnSeq) — Date-free, no <random>,
// no heap. Same pool discipline as RailDotPool: fixed storage, drop-not-grow.
constexpr int   RAIL_EMIT_MAX          = 512;   // hard pool cap
// Emission rates, particles/second PER SECTOR. The silence floor keeps a calm
// baseline drizzle when audio is all-zero (silence must look like calm, not a
// freeze — the same legal-calm rule the flow base obeys). audio_safe_mode caps
// the audio-driven half exactly the way RAIL_RING_PULSE_SAFE caps pulse depth.
constexpr float RAIL_EMIT_BASE_HZ      = 1.5f;  // silence-floor rate
constexpr float RAIL_EMIT_BAND_HZ      = 22.0f; // + band × this …
constexpr float RAIL_EMIT_BAND_HZ_SAFE = 8.0f;  // … capped under audio_safe_mode
constexpr int   RAIL_EMIT_BEAT_N       = 2;     // beat burst, per sector …
constexpr int   RAIL_EMIT_BEAT_N_SAFE  = 1;     // … capped under audio_safe_mode
constexpr float RAIL_EMIT_BEAT_EDGE    = 0.25f; // beat-envelope rising-edge step
constexpr float RAIL_EMIT_LIFE_LO      = 0.6f;  // life seconds, 0.6 .. 1.4
constexpr float RAIL_EMIT_LIFE_SPAN    = 0.8f;
constexpr float RAIL_EMIT_Z_LO         = 25.0f; // birth depth range
constexpr float RAIL_EMIT_Z_SPAN       = 190.0f;
constexpr float RAIL_EMIT_VZ_LO        = 0.85f; // × tunnel flow speed …
constexpr float RAIL_EMIT_VZ_SPAN      = 0.50f; // … 0.85x .. 1.35x jitter
constexpr float RAIL_EMIT_DRIFT_LO     = 2.0f;  // radial OUTWARD drift, units/s
constexpr float RAIL_EMIT_DRIFT_SPAN   = 5.0f;
constexpr float RAIL_EMIT_ANGVEL       = 3.0f;  // |angular drift| cap, rim-units/s
constexpr float RAIL_EMIT_Z_KILL       = 4.0f;  // past the eye — recycle

struct RailEmitParticle {
    float rimPos;     // rim units (0..256 wrap), born inside its sector
    float radius;     // world units from the tunnel axis
    float z;          // depth
    float vzK;        // × live tunnel flow speed (per-particle jitter)
    float vr;         // radial outward drift, units/s
    float vang;       // angular drift, rim-units/s
    float life, age;  // seconds
    float sizeK;      // per-particle size factor
    float bright;     // per-particle brightness factor
    float heat;       // whiten amount at birth, cools over life
    int   sec;        // the wall sector (= band) that emitted it
    bool  alive;
};
struct RailEmitterPool {
    RailEmitParticle p[RAIL_EMIT_MAX] = {};
    int      cursor   = 0;     // rotating free-slot scan start
    uint32_t spawnSeq = 0;     // per-spawn parameter seed (round-deterministic)
    float    prevBeat = 0.0f;  // beat envelope, for rising-edge detection
};

// The comet-tail particle (fifth verdict, item 2 — the RAIL_TAIL_* block).
// Position/velocity live in the tunnel CROSS-SECTION plane (world units, axis
// at 0,0) because the stream is 2-D by construction: born on the rim, flying
// outward + sideways at a near-constant depth (z is fixed per particle, set
// with a small birth jitter). Colour is captured AT BIRTH from the displayed
// sector palette — already green-exclusion-filtered (item 1) — so a particle
// keeps the hue of the moment it left the head while the live palette drifts on.
struct RailTailParticle {
    float px, py;     // cross-section position, world units
    float z;          // depth (≈ the player plane, jittered at birth)
    float vx, vy;     // birth velocity, world units/s (decelerates over life)
    float life, age;  // seconds
    float sizeK;      // per-particle size factor
    float r, g, b;    // birth colour (exclusion-filtered sector hue)
    bool  alive;
};
struct RailTailPool {
    RailTailParticle p[RAIL_TAIL_MAX] = {};
    int      cursor   = 0;     // rotating free-slot scan start
    uint32_t spawnSeq = 0;     // per-spawn parameter seed (round-deterministic)
    float    prevBeat = 0.0f;  // beat envelope, for rising-edge detection
};

// ---- the round's whole persistent state, ONE struct (D8) --------------------
// Everything RAIL carries between frames: the wall-clock-integrated
// accumulators that used to be RendererGL46 members (flow / flowLastMs /
// hueOff / huePhase / the round stamp / the retirement clock) plus the two
// particle pools. The GL backend used to keep the accumulators as class
// members and the pools as function-local statics — two lifetimes for one
// round's state, and no way for a second backend to share either. One struct,
// one stamp, one reset.
//
// 39,216 B, .bss-resident in whichever backend is compiled in (fixed arrays,
// no heap — the doctrine). With the frame's RailDotPool beside it (128,004 B)
// the round costs 163 KB of .bss and not one allocation.
struct RailFxState {
    // --- integrated accumulators (wall-clock ms, frame-rate independent) ---
    float flow       = 0.0f;   // ring stream position, z-units (wrapped)
    int   flowLastMs = 0;      // engine.time at the last step
    float hueOff[AUDIO_BAND_COUNT] = {};   // per-sector displayed hue offset
    float huePhase   = 0.0f;   // global hue phase, ∫ rms (cyclic 0..1)
    float retireT    = 0.0f;   // end-of-round retirement clock, 0..1
    int   roundStamp = -1;     // WarpState::warp_level_time this state belongs to

    // --- persistent particle pools ---
    RailEmitterPool emitter;   // the wall (music-driven) emitter
    RailTailPool    tail;      // the comet's tail shower

    // --- per-frame derived, written by railStepState, read by railBuildDots ---
    float secR[AUDIO_BAND_COUNT] = {};   // displayed sector palette, already
    float secG[AUDIO_BAND_COUNT] = {};   // green-exclusion filtered (item 1) —
    float secB[AUDIO_BAND_COUNT] = {};   // the ONE place the law runs
    int   dtMs = 0;                      // this frame's clamped wall-clock step
};

// Step the whole round's state ONE frame: round-stamp reset, the clamped
// wall-clock dt, the ring flow, the FFT-lerped sector hues + global phase, the
// retirement clock, the displayed sector palette, then both particle pools.
// `glowT` is the level palette's 512-frame sweep position (the same argument
// webLevelColor takes). Call once per frame, BEFORE railBuildDots — which is
// then a pure function of state + sim + audio.
void railStepState(RailFxState& st, const GameEngine& engine,
                   const WarpState& W, float glowT);

// Build the frame's dot cloud into `pool` (which the caller resets: pool.n = 0).
// PURE MATH — no GPU calls of any kind. Reads the stepped state read-only.
void railBuildDots(RailDotPool& pool, const GameEngine& engine,
                   const WarpState& W, const EnvTint& env,
                   const RailFxState& st);

// ============================================================================
// VLM FEEDBACK CHAMBER — the loop law + the MELT-O-VISION mesh (WARP rounds
// ONLY — never normal gameplay). Lifted here from renderer.cpp's warpgl block
// so BOTH backends read ONE copy (rail_c3d_twin.md D8 names exactly this set:
// "FB_* constants + static_asserts … warpFeedbackMeshBuild + feedbackSafeZoom").
// The GL oracle closes the loop through two backbuffer CAPTURES; the C3D twin
// closes it inside a dedicated offscreen chamber with an EXPLICIT inject (D3).
// Different plumbing, identical recurrence — and identical only because every
// constant and the whole mesh build below are shared source, not two copies.
//
// Loop law (vault: feedback-buffer-loop-gain-is-inject-over-one-minus-decay):
// steady brightness ≈ FB_INJECT / (1 − FB_DECAY·overlap). The two knobs are
// halves of one number — TUNE THEM TOGETHER; the saturation failure signature
// is a flat grey wash with all spatial contrast dissolved, not "too bright".
// The warp itself is the relief valve: the outward zoom keeps history sliding
// out from under its own emitters, so |zoom−1| is pushed off zero CENTRALLY
// (identity warp = runaway integrator = photosensitivity hazard — an
// audio-driven pulse cancelling the base zoom must not be able to reach it).
// The warp is parameterised by its FIXED POINT — a displacement would settle
// at d/(1−zoom), a 40–80× amplification of a "small nudge".
// EDGE-REACH RETUNE (design block "THE FEEDBACK MUST REACH THE SCREEN EDGES",
// user verdict 2026-08-19): the first cut (decay 0.55 / zoom 1.012) gave
// N≈5 surviving frames × 1.2%/frame ≈ 6% total travel — the melt died in a
// halo around its emitters. Now decay 0.92 → N≈36 frames, zoom 1.04 →
// 1.04^36 ≈ 4.1× total expansion over one trail life: the anchor-to-side
// distance is ≈3× the tunnel rim radius (anchor at ANCHOR_Y 0.44, reach
// asymmetric by design), so a rim-born trail CROSSES every edge while still
// ALIVE — the ~28-frame rim-to-side journey arrives at 0.92^28 ≈ 10%
// brightness, visibly lit, with ~8 frames of life left (an 0.90 cut reached
// the edges only AT extinction, ~5%, and read as fading just short). Inject
// drops in step — the naive bound 0.12/(1−0.92) = 1.5 OVERSTATES the gain
// (vault, starred section): the RAIL scene is sparse dots and the 4%/frame
// zoom slides history out from under its emitter, so effective overlap ≪ 1;
// the fixed point is the tunnel's own dark centre, where injection is
// near-zero by construction. Verified by looking (GL:
// docs/validation/rail-melt-edge-reach-{14s,20s}.png; C3D twin:
// rail-c3d-chamber-melt-edge-reach.png): black background through
// sustained looping, no grey wash, no corner vignette.
constexpr float FB_DECAY        = 0.92f;   // per-pass history survival
constexpr float FB_INJECT       = 0.12f;   // new-frame strength entering the loop
constexpr float FB_INJECT_SAFE  = 0.06f;   // photosensitive_safe HALVES inject
                                           // (safe static-warp bound:
                                           // 0.06/(1−0.92) = 0.75 < 1 — safe
                                           // mode cannot wash even before the
                                           // overlap relief is counted)
constexpr float FB_ZOOM         = 1.04f;   // > 1: history expands = motion streaks
constexpr float FB_ZOOM_EPS     = 0.004f;  // |zoom−1| floor (identity-warp guard)
constexpr float FB_ROT_PER_ROLL = 0.0005f; // rad/frame per rim-unit/step of roll
                                           // vel — rebudgeted over the new N
                                           // (vault third corollary): ·N·4 ≈
                                           // 4.1° total at the capped
                                           // |roll.vel| ≤ R_CAP = 4 (warp.cpp),
                                           // same authored total as at N≈5
constexpr float FB_FIX_U = ANCHOR_X / UI_W; // fixed point: the tunnel's own
constexpr float FB_FIX_V = ANCHOR_Y;        // vanishing point, in capture uv
static_assert(FB_DECAY < 1.0f, "decay >= 1 saturates the loop to white");
static_assert(FB_INJECT < FB_DECAY,
              "trail-pass gain (decay - inject) must stay positive");
static_assert(FB_INJECT_SAFE <= FB_INJECT,
              "photosensitive_safe may only LOWER inject");

// ---- MELT-O-VISION: the history redraw is a SUBDIVIDED GRID -----------------
// The chamber redraw samples the history on a 16×12 QUAD grid (17×13 = 221
// vertices) with per-vertex UV distortion — a slow radial swirl around the
// tunnel anchor plus a
// travelling sine wobble — so the history MELTS and flows instead of just
// streaking. Strictly the sample-only class: more vertices on the same draw,
// same history, no new capture; the NEW geometry each frame is drawn
// undistorted, only the history warps.
//
// EVERY per-pass rate below is budgeted over the trail life (vault:
// feedback-warp-rates-are-budgeted-over-the-trail-not-per-frame):
//   N = ln(0.05) / ln(FB_DECAY = 0.92) ≈ 35.9 frames to 5%
// so each per-pass constant is authored as (total budgeted excursion) / N —
// change FB_DECAY and these MUST be re-derived, or the compounding collapses
// the trails (the "flickering all over the screen" failure). NB FB_ZOOM is
// the ONE knob deliberately OUTSIDE the ≤10–15% total-scale rule this wave:
// FB_ZOOM^N = 1.04^35.9 ≈ 4.1× is the edge-reach contract itself (the
// design block demands total expansion > ~2×; the anchor-to-side distance
// is ≈3 rim radii, and the ×4.1 margin is what lands trails on the edge
// still visibly lit rather than at extinction — corners included).
// FB_ROT_PER_ROLL·N at the capped |roll.vel| ≤ R_CAP = 4 gives ≈ 4.1° total
// (well under the ≤15° rule — rebudgeted with the new N above).
constexpr float FB_TRAIL_N        = 35.9f;  // ln(0.05)/ln(FB_DECAY), see above
constexpr int   FB_MESH_COLS      = 16;     // grid quads across
constexpr int   FB_MESH_ROWS      = 12;     // grid quads down
// Radial swirl: differential rotation about the fixed point, strongest at the
// tunnel anchor, decaying outward with a soft 1/(1+(r/R0)²) falloff. Total
// budget 0.17 rad ≈ 10° over one trail life → ≈0.0047 rad/pass at the peak
// (total UNCHANGED by the edge-reach retune; only N moved, 5 → 35.9).
constexpr float FB_SWIRL_TOTAL    = 0.17f;               // rad over N frames
constexpr float FB_SWIRL_PER_PASS = FB_SWIRL_TOTAL / FB_TRAIL_N;
constexpr float FB_SWIRL_R0       = 0.42f;  // falloff radius, fraction of vp height
// Travelling sine wobble: zero-mean UV offsets (no net displacement — the
// fixed-point rule; a mean offset would integrate at d/(1−zoom)). Total budget
// 0.024 UV ≈ 2.4% of the screen over one trail life → ≈0.00067 UV/pass
// (total UNCHANGED by the edge-reach retune; only N moved, 5 → 35.9).
constexpr float FB_WOBBLE_TOTAL    = 0.024f;             // UV over N frames
constexpr float FB_WOBBLE_PER_PASS = FB_WOBBLE_TOTAL / FB_TRAIL_N;
constexpr float FB_WOBBLE_KX       = 2.5f * TWO_PI; // waves across u ∈ [0,1]
constexpr float FB_WOBBLE_KY       = 1.8f * TWO_PI; // waves across v ∈ [0,1]
constexpr float FB_WOBBLE_HZ       = 0.35f;  // phase advance with time …
constexpr float FB_WOBBLE_PER_ROLL = 0.05f;  // … and with roll (rad per rim unit)
static_assert(FB_MESH_COLS >= 2 && FB_MESH_ROWS >= 2,
              "the melt grid must actually subdivide");

// ---- residue sweep (8-bit quantization floor — PICA/C3D ONLY) ---------------
// On the PICA the loop state lives in an 8-bit RGBA8 chamber texture (the
// vault note's corollary says this "transfers to the PICA200 directly"), and
// blending rounds to nearest: any history value v with
// (1−FB_DECAY)·v < 0.5 LSB survives the decay UNCHANGED — at decay 0.92
// everything ≤ ~6/255 freezes forever, and after sustained looping the whole
// screen sits under a permanent dim veil (measured: the truly-black pixel
// fraction fell from 0.83 to 0.00). The old decay 0.55 froze only v ≤ 1, which
// is why this never showed before. The sweep breaks the equilibrium: every
// FB_SWEEP_PERIOD-th pass subtracts one LSB from the redrawn history (a
// reverse-subtract fullscreen quad, drawn AFTER the history redraw and BEFORE
// the new frame's light enters), so residue clears at ~1 LSB per period
// (≈0.15 s) while a bright burst trail (≥ ~100/255 at birth) still reaches the
// screen edge visibly lit: the mean bias is 1/PERIOD LSB/frame, asymptote −4
// LSB at period 3 — v₀=100 arrives at the ~28-frame edge crossing at ≈6,
// v₀=150 at ≈11, vs the un-swept 10/15. Sweeping EVERY frame instead would
// bias −12.5 and kill edge arrival for everything under v₀≈180 — do not
// "simplify" the period away. On C3D the sweep is PER EYE: each eye's chamber
// is its own independent 8-bit integrator.
// The Vulkan history is R16G16B16A16_UNORM (SFLOAT16 only on a device that
// cannot blend into UNORM16), whose 1.5e-5 LSB puts the freeze threshold at
// v < 9e-5 — a fortieth of one 8-bit step — so FB_SWEEP_* is a 3DS-ONLY
// correction and no sweep runs on PC. See t2k_pc/src/rendering/vk_chamber.cpp.
constexpr int   FB_SWEEP_PERIOD = 3;              // passes between sweeps
constexpr float FB_SWEEP_LSB    = 1.0f / 255.0f;  // exactly one 8-bit LSB
static_assert(FB_SWEEP_PERIOD >= 1, "sweep period must be at least 1");

// The melt mesh. Positions are UI space (x ∈ [0,UI_W], y ∈ [0,1]); u/v are the
// NORMALISED sample point (u ∈ [0,1] across the box, v ∈ [0,1] up it), i.e. the
// UI point (u·UI_W, v). GL feeds u/v straight to glTexCoord2f because its
// capture IS the screen; the C3D twin re-projects (u·UI_W, v) through the same
// ortho the positions take and converts the result to chamber uv, which makes
// the mapping exact whatever the target's own storage convention is.
struct FbMeshVert { float x, y, u, v; };
struct FbMesh { FbMeshVert v[FB_MESH_ROWS + 1][FB_MESH_COLS + 1]; };

// Central identity-warp guard: push zoom off 1.0 preserving its direction
// (outward when exactly 1.0). Every chamber draw routes through this — no
// module or future preset can route around it.
inline float feedbackSafeZoom(float z) {
    const float d = z - 1.0f;
    if (d > -FB_ZOOM_EPS && d < FB_ZOOM_EPS)
        return 1.0f + (d < 0.0f ? -FB_ZOOM_EPS : FB_ZOOM_EPS);
    return z;
}

// MELT-O-VISION history redraw, build half — PURE MATH, no GPU calls of any
// kind: fill the fixed FbMesh with the frame's warp geometry. Each vertex
// samples the history at fix + S(r)·R(−rot)/zoom · (p − fix) + wobble, computed
// in PIXEL space so the rotations stay aspect-true at any viewport size:
//   · the base affine (rot from roll vel, zoom through feedbackSafeZoom — the
//     single central guard, never bypassed) is exact at every vertex;
//   · S(r) is the melt SWIRL — an extra differential rotation about the SAME
//     fixed point, strongest at the tunnel anchor and decaying outward, so the
//     history slowly whirlpools around the vanishing point;
//   · the wobble is a travelling zero-mean sine in UV, phase advancing with
//     time and the player's roll, so the goo flows rather than holding still.
// zoom > 1 samples nearer the fixed point, so history expands OUTWARD (the
// trails/streaks direction — the inward sign has a diagnostic corner-vignette
// failure, see the vault note). ONE build per frame serves every pass that
// consumes it (GL's two capture redraws; C3D's two chambers — the params are
// eye-independent). distortScale halves the melt amplitudes under
// photosensitive_safe (the same knob that halves inject).
void warpFeedbackMeshBuild(FbMesh& mesh, float zoom, float rotRad,
                           float wobPhase, float distortScale,
                           int vpW, int vpH);

// The rect-parameterized twin: the same melt math over a UI SUB-RECT
// [x0,x1] x [y0,y1] (positions in UI space, sample coords rect-normalized,
// the fixed point rect-normalized too). The fullscreen entry above is this
// one over the whole UI box -- ONE math, so a fullscreen and a sliver chamber
// can never drift. Used by the pause melt (ui/pause_fx.h), which warps the
// whole UI box with a lobed swirl.
// `swirlLobes` braids the swirl AZIMUTHALLY: with K lobes the differential
// rotation runs one way in K sectors and the other way between them, so the
// history converges toward K exits around the periphery instead of spreading
// as an even radial bloom (the pause melt's "lerps to points around the
// screen"; ui/pause_fx.h). Rotation adds NO net radial displacement, so the
// loop's relief valve -- the outward zoom -- is untouched and no convergent
// integrator is reachable. `swirlScale` scales the swirl's peak.
// DEFAULTS ARE THE IDENTITY: lobes 0 makes the cosine 1 at every vertex, so
// the title, RAIL and score chambers get byte-identical geometry to before.
void warpFeedbackMeshBuildRect(FbMesh& mesh,
                               float x0, float y0, float x1, float y1,
                               float fixU, float fixV, float zoom, float rotRad,
                               float wobPhase, float distortScale,
                               int vpW, int vpH,
                               int swirlLobes = 0, float lobePhase = 0.0f,
                               float swirlScale = 1.0f);

} // namespace railgeom
} // namespace ts
