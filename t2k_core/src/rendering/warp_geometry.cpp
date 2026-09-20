// ============================================================================
// warp_geometry.cpp — the GATES bonus round's build half, shared by
// BOTH backends. Lifted VERBATIM out of renderer.cpp's warpgl block (the same
// move rail_geometry.{h,cpp} made for RAIL), which is why the comments read as
// they do: they are the frozen GL look's own reasoning, and the C3D twin
// inherits it by consuming this file rather than re-deriving anything.
//
// No GL, no citro3d, no renderer includes. Pure data + free functions over
// GameEngine/WarpState, fixed pools, deterministic hashes.
// ============================================================================

#include "warp_geometry.h"

#include <cmath>
#include <cstring>

#include "../game/engine.h"
#include "../game/warp.h"
#include "../game/math_lut.h"   // fastSin/fastCos — the same trig both backends use
#include "../game/phase.h"   // exact integer phase: zero drift at cabinet uptimes
#include "web_palette.h"        // currentWebColorBatch / webHsv2rgb

namespace ts {
namespace warpgeom {
// Gate-ring pulse rate, Q0.48 turns/ms (game/phase.h). File scope: compile-time
// constant, and the VFP gate reads a constexpr `double` inside a hot function
// as an R4 promotion even though it is host-evaluated.
constexpr ts::phase::Rate K_GATE_PULSE = ts::phase::fromRadPerMs(0.012);
}

namespace warpgeom {

namespace {

// The 4-pass core->halo glow stack, UI space — the same soft stack the web's
// vector glow uses (renderLevelPass0 passW/passA; C3D drawGlowWire), expressed
// as widening strokes instead of a line-width change. EXPANDED HERE, in the
// shared builder, so both backends replay exactly the same primitive list in
// exactly the same order (the GL oracle's own emission order, unchanged).
const float GLOW_W_MUL[4] = { 1.0f, 2.6f, 5.4f, 10.5f };
const float GLOW_A_MUL[4] = { 0.85f, 0.34f, 0.16f, 0.075f };

inline void glowStroke(WarpStrokePool& sp,
                       float x1, float y1, float x2, float y2, float w,
                       float r, float g, float b, float a, int passes,
                       float zeye) {
    if (passes > 4) passes = 4;
    for (int p = 0; p < passes; ++p)
        sp.emit(x1, y1, x2, y2, w * GLOW_W_MUL[p],
                r, g, b, a * GLOW_A_MUL[p], zeye);
}

// Closed glow-wire ring (segs=8 + rot pi/8 = the reference's own octagonal
// gate shape, in this game's glow stack).
inline void ringStroke(WarpStrokePool& sp, float cx, float cy, float rUi,
                       int segs, float rot, float halfW,
                       float r, float g, float b, float a, int passes,
                       float zeye) {
    float lx = 0.0f, ly = 0.0f;
    for (int k = 0; k <= segs; ++k) {
        const float ang = rot + (float)k / (float)segs * TWO_PI;
        const float x = cx + fastCos(ang) * rUi;
        const float y = cy + fastSin(ang) * rUi;
        if (k) glowStroke(sp, lx, ly, x, y, halfW, r, g, b, a, passes, zeye);
        lx = x; ly = y;
    }
}

}  // namespace

// ---- the round's persistent state -------------------------------------------
void warpStepState(WarpFxState& st, const GameEngine& engine,
                   const WarpState& W) {
    // Round entry: the audio accumulators restart (hue melt starts ON the
    // level's hue; the snake's breath resets to mid-range).
    if (st.roundStamp != W.warp_level_time) {
        st.roundStamp   = W.warp_level_time;
        st.starDrift    = 0.0f;
        st.riverHue     = 0.0f;
        st.riverMeltV   = 0.0f;
        st.riverAmp     = RIVER_AMP_MID;
        st.riverAmpTgt  = RIVER_AMP_MID;
        // riverAmpEpoch is DELIBERATELY NOT reset. Resetting it would force an
        // immediate re-choose on the very next step, overwriting the A_MID the
        // two lines above just established — i.e. the round-entry reset would
        // not survive its own frame. Leaving the epoch alone means a new round
        // starts at mid-range and stays there until the wall-clock retarget
        // bucket rolls, which is the shipped behaviour and what the reset is
        // for. (Its 0xFFFFFFFF initial value still makes the first round of a
        // session pick a target on its first frame.)
        st.lastMs       = engine.time;
    }

    // Integrated in wall-clock ms so rates are frame-rate independent (the
    // railgeom flow discipline). Rates and the recorded hue-is-identity
    // exception live in the RIVER_HUE_DRIFT_* / STARRUSH_DRIFT_RMS blocks;
    // all-zero audio degrades to the calm MIN-rate state, never a freeze.
    int adt = engine.time - st.lastMs;
    if (adt < 0) adt = 0;
    if (adt > 100) adt = 100;      // pause/hitch guard
    st.lastMs = engine.time;
    const float rmsK  = wclamp01(engine.audio.rms);
    const float centK = wclamp01(engine.audio.spectralCentroid);
    st.starDrift += STARRUSH_DRIFT_RMS * rmsK * (float)adt;
    st.riverHue += (RIVER_HUE_DRIFT_MIN + RIVER_HUE_DRIFT_RMS * rmsK
                    + RIVER_HUE_DRIFT_CENT * centK)
                 * 0.001f * (float)adt;
    st.riverHue -= std::floor(st.riverHue);   // hue is cyclic
    st.riverMeltV += (RIVER_MELT_V_MIN + RIVER_MELT_V_RMS * rmsK)
                   * 0.001f * (float)adt;
    // meltV stays unfolded on purpose: it feeds two differently-scaled uv
    // terms (v and u·RIVER_MELT_DIAG), so no single fold period keeps
    // both continuous. It grows ~0.05/s — float-exact for hours.

    // The river snake's BREATHING amplitude (fourth river verdict — law
    // + clamp derivation in the RIVER_AMP_* block): re-choose the target
    // once per RETARGET_S wall-clock bucket from the deterministic
    // hash01 stream (uniform in [A_MIN, A_MAX]), then lerp the displayed
    // A toward it with the frame-rate-independent 1 − exp(−dt/τ) — the
    // same discipline as the RAIL sector-hue lerp. Hard-clamped both
    // ends; reset to A_MID with the round (block above).
    const uint32_t ampEpoch =
        (uint32_t)(engine.time / (int)(RIVER_AMP_RETARGET_S * 1000.0f));
    if (ampEpoch != st.riverAmpEpoch) {
        st.riverAmpEpoch = ampEpoch;
        st.riverAmpTgt = RIVER_AMP_MIN
            + (RIVER_AMP_MAX - RIVER_AMP_MIN)
                * hash01(ampEpoch * 2654435761u + 97u);
    }
    // NOTE: 1-exp(-dt/tau) breathing-amplitude lerp law, 1x/frame.
    /* @vfp-exempt R6 — river breathing amplitude lerp 1-exp(-adt/tau): frame-rate-independent retarget law; no fastExp in math_lut.h. measured n/a. Verified 2026-09-06. */
    const float ampK = 1.0f - std::exp(-(float)adt * 0.001f / RIVER_AMP_TAU_S);
    st.riverAmp += (st.riverAmpTgt - st.riverAmp) * ampK;
    if (st.riverAmp < RIVER_AMP_MIN) st.riverAmp = RIVER_AMP_MIN;
    if (st.riverAmp > RIVER_AMP_MAX) st.riverAmp = RIVER_AMP_MAX;
}

// ---- the star backdrop -------------------------------------------------------
void warpBuildStarRush(WarpStrokePool& strokes, WarpDotPool& dots,
                       const GameEngine& engine, const WarpState& W,
                       const EnvTint& env, int count,
                       float baseAlpha, float strobeGain, float rushMul,
                       float audioDrift, float hr, float hg, float hb) {
    float beat = wclamp01(engine.audio.beat * engine.audio_pulse_k);
    // Agreed strobe law, identical on both backends: depth 0.75 normally
    // hard-capped to 0.18 under audio_safe_mode.
    if (engine.audio_safe_mode && strobeGain > 0.18f) strobeGain = 0.18f;

    const int catchAge = W.sim_time - W.catch_ms;
    if (catchAge >= 0 && catchAge < 300)
        beat = wclamp01(beat + (1.0f - (float)catchAge / 300.0f) * 0.8f);

    // NOTE: star-field star-count truncation + catch-kick envelope below.
    /* @vfp-exempt R2,R3 — star-rush count/envelope: (int) truncation sets the emitted star budget, catch-kick scales beat; hash salts are integer lattice identity. measured n/a. Verified 2026-09-06. */
    int total = count + (int)(env.wm * (float)(STAR_MAX - count));
    if (total > STAR_MAX) total = STAR_MAX;

    const float speedK = (W.base_speed > 0.01f)
                       ? (W.speed - W.base_speed) / W.base_speed : 0.0f;
    const float timeS = (float)engine.time * 0.001f;

    /* @vfp-exempt R2,R3 — star-rush per-star loop (pins cull, twinkle, streak/dot split below): hash salts are integer identity; cull/fade define the emitted primitive set. measured n/a. Verified 2026-09-06. */
    for (int i = 0; i < total; ++i) {
        // NOTE: placement salts below are integer lattice identity.
        /* @vfp-exempt R2,R3 — star placement salts (pins the hashes below): integer lattice identity; parallax wrap is the depth law. measured n/a. Verified 2026-09-06. */
        const float ang = hash01((uint32_t)i * 3u + 1u) * TWO_PI;
        const float r0  = hash01((uint32_t)i * 3u + 2u);
        const float dep = 0.35f + 0.65f * hash01((uint32_t)i * 3u + 3u);
        // Sim rush + the rms-integrated audio drift share the depth divisor,
        // so the parallax layering survives the audio term.
        float rr = r0 + (W.dist * 0.00045f * rushMul + audioDrift) / dep;
        rr -= std::floor(rr);
        const float rad = rr * rr * 1.05f;
        const float ca = fastCos(ang), sa = fastSin(ang);
        const float x = ANCHOR_X + ca * rad * 1.15f;
        const float y = ANCHOR_Y + sa * rad;
        /* @vfp-exempt R3 — star viewport cull (pins the if below): discard set for the rush field. measured n/a. Verified 2026-09-06. */
        /* @vfp-exempt R2,R3 — twinkle block (pins the salt hashes and tw fade below; nested segments split attribution). measured n/a. Verified 2026-09-06. */
        if (x < -0.05f || x > UI_W + 0.05f || y < -0.05f || y > 1.05f) continue;

        // Slow per-star twinkle (brightness only; distinct hash stream from
        // the placement seeds above so rates never correlate with angles).
        /* @vfp-exempt R2 — twinkle hash salts are integer lattice identity; tw fade is the brightness law. measured n/a. Verified 2026-09-06. */
        /* @vfp-exempt R2 — twinkle second salt stream (pins the hash below): integer lattice identity, de-correlated rates. measured n/a. Verified 2026-09-06. */
        const float twHz = STARRUSH_TW_HZ_LO
                         + STARRUSH_TW_HZ_SPAN * hash01((uint32_t)i * 7u + 4u);
        const float tw = 1.0f - STARRUSH_TW_DEPTH
                       * (0.5f + 0.5f * fastSin(timeS * twHz * TWO_PI
                                   + hash01((uint32_t)i * 7u + 5u) * TWO_PI));

        float a = baseAlpha * (0.25f + 0.75f * rr) * tw
                * (1.0f + strobeGain * beat);
        float r = hr, g = hg, b = hb;
        whiten(0.35f + 0.45f * rr, r, g, b);   // near stars burn whiter
        env.apply(r, g, b, a);

        const float size = (0.0035f + 0.0040f * rr) * (1.0f + 5.0f * env.wm);
        const float len = rad * (0.10f * speedK + 0.30f * env.wm);
        // The backdrop's own eye-relative depth, layered by the star's parallax
        // divisor so the field still has structure in stereo (C3D only; GL
        // never reads it).
        const float zeye = STARRUSH_ZEYE * dep;
        /* @vfp-exempt R3 — streak-vs-dot split on len: radial streaks for boosts/white-out, dots otherwise; defines the emitted primitive set. measured n/a. Verified 2026-09-06. */
        /* @vfp-exempt R3 — streak split branch (pins the if below; nested segments split attribution). measured n/a. Verified 2026-09-06. */
        /* @vfp-exempt R2,R3 — streak branch island (pins the if below; split_loops emits a bare-if segment here). measured n/a. Verified 2026-09-06. */
        if (len > 0.006f) {
            // radial streak: boosts and the white-out stretch the field
            strokes.emit(x - ca * len, y - sa * len, x, y, size,
                         r, g, b, a, zeye);
        } else {
            dots.emit(x, y, size, r, g, b, a, zeye);
        }
    }
}

// ---- the river ---------------------------------------------------------------
void riverBuildStrip(RiverMesh& mesh, const GameEngine& engine,
                     const WarpState& W, const EnvTint& env,
                     float huePhase, float meltV, float amp, float glowT) {
    // The surface's private view: same lateral frame as the gate world, but
    // the vertical offset amplified RIVER_H_PARALLAX× so the surface reads
    // close. sign(sy) is preserved — above/below flips exactly at the sim's
    // y = 0.
    View v;
    v.sx = W.axis_x.value;
    v.sy = W.axis_y.value * RIVER_H_PARALLAX;

    // ---- the tint: level hue + music hue-melt, bass-ducked -----------------
    // Hue base is the level band's own sweep (hue is identity...) plus the
    // integrated melt phase (...except HERE, by recorded design exception —
    // see the RIVER_HUE_DRIFT_* block). webHsv2rgb wraps hue internally.
    const WebHueBatch& batch = currentWebColorBatch(engine.current_level);
    const float hue = batch.h0 + (batch.h1 - batch.h0) * glowT + huePhase;
    const float sat = batch.s0 + (batch.s1 - batch.s0) * glowT;
    // The web's own duck law, constant for constant (audioWebBrightness):
    // beat is the dominant transient term, rms the small continuous one.
    float depth = engine.audio_pulse_k;
    if (engine.audio_safe_mode && depth > RIVER_DUCK_DEPTH_SAFE)
        depth = RIVER_DUCK_DEPTH_SAFE;
    float duck = (engine.audio.beat * RIVER_DUCK_BEAT
                  + engine.audio.rms * RIVER_DUCK_RMS) * depth;
    if (duck > RIVER_DUCK_MAX) duck = RIVER_DUCK_MAX;
    const float bright = RIVER_BRIGHT_BASE * (1.0f - duck);
    float tr, tg, tb;
    webHsv2rgb(hue, sat, bright, tr, tg, tb);

    // Fold each layer's scroll into one repeat period (1.0, REPEAT wrap) ONCE
    // per frame; rows then add z·k unfolded, so v stays continuous between
    // adjacent rows and float precision never decays however long the round
    // runs. texB adds the integrated melt drift — the second, slower scroll.
    const float vBaseA = W.dist * RIVER_V_PER_Z
                       - std::floor(W.dist * RIVER_V_PER_Z);
    const float vbRaw  = W.dist * RIVER_VB_PER_Z + meltV;
    const float vBaseB = vbRaw - std::floor(vbRaw);
    const float ubDrift = meltV * RIVER_MELT_DIAG;
    const float uBaseB  = ubDrift - std::floor(ubDrift);

    const float timeS = (float)engine.time * 0.001f;

    for (int i = 0; i <= RIVER_ROWS; ++i) {
        // Depth rows on a squared ramp: dense near the eye, sparse at the far
        // cutoff — the same density bias the mode-7 scanlines this replaces
        // had for free.
        const float t = (float)i / (float)RIVER_ROWS;
        const float z = RIVER_Z_NEAR + (RIVER_Z_FAR - RIVER_Z_NEAR) * t * t;

        // THE SNAKE WAVE (fourth river verdict — law, clamps and derivation
        // in the RIVER_AMP_* block): ONE travelling sine displaces BOTH
        // edges by the SAME offset about their resting ±HALF_W, amplitude
        // amp = the breathing A(t) warpStepState integrates. Constant width,
        // silhouettes in exact anti-sync read, structurally unable to differ
        // in intensity — no pinned edge, no straight cut on either side.
        const float theta  = RIVER_RIPPLE_KZ * z + RIVER_RIPPLE_WT * timeS;
        const float offset = RIVER_HALF_W * amp * fastSin(theta);
        const float xL     = offset - RIVER_HALF_W;
        const float xR     = offset + RIVER_HALF_W;

        const float vvA = vBaseA + z * RIVER_V_PER_Z;
        const float vvB = vBaseB + z * RIVER_VB_PER_Z;

        // Depth fog toward the far cutoff (the reference's depth cue), fed to
        // the shader's INTERPOLATE-to-black stage through the vertex ALPHA
        // slot: fog = 1 exactly at RIVER_Z_FAR, so the far edge dissolves
        // into the starfield instead of ending on a line.
        const float fog = t * t;
        float r = tr, g = tg, b = tb;
        whiten(env.wm * (1.0f - fog), r, g, b);   // win white-out on the tint

        // Project at the REMAPPED view depth (v4 framing under the WYSIWYG
        // View — the riverViewZ remap block); z itself stays the sim depth for
        // the wave/texture/fog math above.
        const float zView = riverViewZ(z);
        const float zeye  = zView + EYE_Z;   // the row's own scale divisor

        for (int j = 0; j <= RIVER_COLS; ++j) {
            const float u = (float)j / (float)RIVER_COLS;
            const float wx = xL + (xR - xL) * u;
            RiverVert& o = mesh.v[i][j];
            v.project(wx, 0.0f, zView, o.x, o.y);
            o.r = r; o.g = g; o.b = b;
            o.fog = fog;
            // Constant width ⇒ the texture RIDES the snake laterally rather
            // than compressing with it (u fixed per column, xR−xL constant)
            // — correct for a snaking band; matches the later port meander read.
            // The arcade reference texel squeeze left with the v3 scale law.
            o.uA = u * RIVER_U_TILE;
            o.vA = vvA;
            o.uB = uBaseB + u * RIVER_UB_TILE;
            o.vB = vvB;
            o.zeye = zeye;
        }
    }
}

void riverBuildBorders(WarpStrokePool& strokes, const RiverMesh& mesh,
                       const GameEngine& engine, float gain) {
    // GLOWY VECTOR EDGE BORDERS (third proof verdict) — emitted AFTER the strip
    // (drawn on top of it), before the gates. The polyline IS the strip's own
    // outer columns: mesh.v[i][0] and mesh.v[i][RIVER_COLS] hold the exact
    // projected edge positions the surface just drew (xL/xR through the same
    // View), so the border can never desync from the silhouette — shared math,
    // nothing re-derived. One glowStroke per row pair per side; the vertex tint
    // already carries the music hue-melt, the bass duck and the win whiten, and
    // the row fog fades the border into the horizon with the surface. Beat
    // pulse capped under audio_safe_mode (strobeGain pattern); gain folds in
    // the fail fade / win bloom like everything else here.
    const float beatK = wclamp01(engine.audio.beat * engine.audio_pulse_k);
    const float edgeBeat = engine.audio_safe_mode ? RIVER_EDGE_BEAT_SAFE
                                                  : RIVER_EDGE_BEAT;
    const float edgePulse = 1.0f + edgeBeat * beatK;
    /* @vfp-exempt R3 — river border loop (pins horizon cull, sqrt falloff, alpha clamps below): fogK cull is the discard set, sqrt is the readability falloff law. measured n/a. Verified 2026-09-06. */
    for (int side = 0; side < 2; ++side) {
        const int j = side ? RIVER_COLS : 0;
        for (int i = 0; i < RIVER_ROWS; ++i) {
            const RiverVert& p0 = mesh.v[i][j];
            const RiverVert& p1 = mesh.v[i + 1][j];
            const float fogK = 1.0f - 0.5f * (p0.fog + p1.fog);
            if (fogK <= 0.02f) continue;   // dissolved into the horizon
            // sqrt falloff: mid-depth banks (the ones a high-altitude view
            // actually sees) keep a readable glow while the horizon still
            // dissolves to nothing with the surface's own fog.
            const float fogA = std::sqrt(fogK);
            float ea = RIVER_EDGE_ALPHA * fogA * edgePulse * gain;
            if (ea > 1.0f) ea = 1.0f;
            /* @vfp-exempt R3 — border alpha clamps: horizon fade law; zero-alpha skip avoids invisible strokes. measured n/a. Verified 2026-09-06. */
            if (ea <= 0.0f) continue;
            // Width tapers with the same fog term — a cheap perspective cue
            // (true scaleAt(z) spans 170:1 near-to-far, far too extreme for
            // a stroke width).
            const float ew = RIVER_EDGE_W * (0.35f + 0.65f * fogK);
            glowStroke(strokes, p0.x, p0.y, p1.x, p1.y, ew,
                       p0.r, p0.g, p0.b, ea, 4, p0.zeye);
        }
    }
}

// ---- the gates ---------------------------------------------------------------
void warpBuildGates(WarpStrokePool& strokes,
                    const GameEngine& engine, const WarpState& W,
                    const EnvTint& env,
                    float hr, float hg, float hb) {
    View v;
    v.sx = W.axis_x.value;
    v.sy = W.axis_y.value;

    const float wstep = std::min(0.85f, 0.17f * (float)W.section);
    float gr = hr, gg = hg, gb = hb;
    whiten(wstep, gr, gg, gb);

    // The clock, once. Loop-invariant, and an int-to-unsigned widening rather
    // than a conversion of any kind -- but it belongs out here regardless.
    const uint32_t tms = (uint32_t)engine.time;

    /* @vfp-exempt R3 — gate loop (pins culls, far-side dim, halfW clamps, victory/boost mods, chevron ternary below): culls define the discard set, fades define the ring-equals-box law. measured n/a. Verified 2026-09-06. */
    for (int i = W.gate_count - 1; i >= 0; --i) {
        const WarpGate& gate = W.gates[i];
        if (!gate.alive) continue;
        const float z = gate.z;
        if (z <= 0.0f || z > WARP_GATE_VIEW_Z) continue;

        const float s = v.scaleAt(z);
        const float zeye = z + EYE_Z;
        // Gate-layer lateral compression (GATE_LATERAL_K block): centres and
        // radius compress TOGETHER, so "reticle inside ring" still reads
        // exactly |dx| < GATE_R per axis — WYSIWYG preserved.
        const float cx = ANCHOR_X + GATE_LATERAL_K * (gate.x - v.sx) * s;
        const float cy = ANCHOR_Y + GATE_LATERAL_K * (gate.y - v.sy) * s;
        const float rUi = GATE_LATERAL_K * GATE_R * s;
        const float aIn = wclamp01((WARP_GATE_VIEW_Z - z) / 36.0f);
        float a = (0.22f + 0.68f * (1.0f - z / WARP_GATE_VIEW_Z)) * aIn;

        // RIVER far-side rule: the reference hides a gate on the other side
        // of the y = 0 surface from the ship. Simplest faithful form: dim it
        // to RIVER_FAR_SIDE_DIM when the y signs differ, reading the SIM's own
        // axis_y so visibility flips exactly with the river's above/below
        // crossing. In-plane gates and a plane-skimming ship see everything.
        {
            const float sy = W.axis_y.value;
            /* @vfp-exempt R3 — river far-side dim: hides gates across the y=0 surface per the reference; sign test reads the sim axis. measured n/a. Verified 2026-09-06. */
            /* @vfp-exempt R3 — far-side sign branch (pins the if below; nested segments split attribution). measured n/a. Verified 2026-09-06. */
            if ((gate.y >  RIVER_SIDE_EPS && sy < -RIVER_SIDE_EPS) ||
                (gate.y < -RIVER_SIDE_EPS && sy >  RIVER_SIDE_EPS))
                a *= RIVER_FAR_SIDE_DIM;
        }
        float halfW = rUi * 0.10f;
        /* @vfp-exempt R3 — gate stroke-width clamps: perspective floor/ceiling for ring readability. measured n/a. Verified 2026-09-06. */
        if (halfW < 0.0018f) halfW = 0.0018f;
        if (halfW > 0.0110f) halfW = 0.0110f;

        float r = gr, g = gg, b = gb;
        // THE ONLY PHASE IN THE TREE THAT HARD-FAILS, NOT MERELY STEPS.
        // At 0.012 rad/ms this is the fastest surviving float argument, and it
        // reaches fastSin's documented domain bound (|rad| < ~1.318e7, see
        // math_lut.h) after 1.098e9 ms = 12.7 DAYS -- *before* the 24.9-day
        // signed-millisecond rollover. Past that, fastSin's truncating range
        // reduction saturates and the pulse PINS at a constant instead of
        // oscillating. On an arcade cabinet that is reachable in under a
        // fortnight.
        //
        // Exact integer turns (game/phase.h), and the per-gate radian stagger
        // becomes phase::radIndex -- an integer multiply that wraps modularly,
        // so there is no float anywhere left in the argument.
        const ts::phase::Turns gatePhase =
            ts::phase::at(tms, K_GATE_PULSE)
          + ts::phase::radIndex(i);
        const float pulse = 0.5f + 0.5f * ts::phase::sinTurns(gatePhase);

        if (gate.type == WGATE_VICTORY) {
            // White-hot and pulsing — intensity is the event, never a new hue.
            whiten(0.55f + 0.40f * pulse, r, g, b);
            a = std::min(1.0f, a * 1.3f);
        } else if (gate.type == WGATE_BOOST) {
            whiten(0.30f, r, g, b);
            a *= 0.9f + 0.25f * pulse;
        }
        env.apply(r, g, b, a);

        {
            constexpr float OCT_ROT = 0.3926991f;   // pi/8: flat-bottomed octagon
            ringStroke(strokes, cx, cy, rUi, 8, OCT_ROT, halfW,
                       r, g, b, a, 4, zeye);
            if (gate.type == WGATE_BOOST)
                ringStroke(strokes, cx, cy, rUi * 0.55f, 8, OCT_ROT, halfW * 0.8f,
                           r, g, b, a * 0.8f, 2, zeye);
            if (gate.type == WGATE_VICTORY) {
                ringStroke(strokes, cx, cy, rUi * 0.74f, 8, OCT_ROT, halfW * 0.9f,
                           r, g, b, a * 0.9f, 2, zeye);
                ringStroke(strokes, cx, cy, rUi * 0.50f, 8, OCT_ROT, halfW * 0.8f,
                           r, g, b, a * 0.8f, 2, zeye);
            }
        }

        // Impulse chevron: points the kick's direction.
        if (gate.type == WGATE_UP || gate.type == WGATE_DOWN) {
            /* @vfp-exempt R3 — chevron orientation ternary: UP=+1/DOWN=-1 kick direction; integer enum condition. measured n/a. Verified 2026-09-06. */
            /* @vfp-exempt R3 — chevron dir ternary (pins the select below; nested segments split attribution). measured n/a. Verified 2026-09-06. */
            const float dir = (gate.type == WGATE_UP) ? 1.0f : -1.0f;
            float cr2 = r, cg2 = g, cb2 = b;
            whiten(0.25f, cr2, cg2, cb2);
            const float ca2 = std::min(1.0f, a * 1.2f);
            glowStroke(strokes, cx - rUi * 0.5f, cy - dir * rUi * 0.20f,
                       cx,                       cy + dir * rUi * 0.35f,
                       halfW * 0.8f, cr2, cg2, cb2, ca2, 2, zeye);
            glowStroke(strokes, cx,              cy + dir * rUi * 0.35f,
                       cx + rUi * 0.5f,          cy - dir * rUi * 0.20f,
                       halfW * 0.8f, cr2, cg2, cb2, ca2, 2, zeye);
        }
    }
}

// ---- the catch shockwave -----------------------------------------------------
void warpBuildShock(WarpStrokePool& strokes, const WarpState& W,
                    const EnvTint& env, float hr, float hg, float hb) {
    const int age = W.sim_time - W.catch_ms;
    if (age < 0 || age >= SHOCK_LIFE_MS) return;
    const float t = (float)age / (float)SHOCK_LIFE_MS;

    // The rim size the player was just shown at the judgment plane —
    // compressed exactly like the ring it echoes (GATE_LATERAL_K block).
    constexpr float SHOCK_RIM_UI =
        GATE_LATERAL_K * GATE_R * (FOCAL / (ts::WARP_CATCH_Z + EYE_Z));
    const float rUi = SHOCK_RIM_UI * (SHOCK_OPEN_FRAC + SHOCK_SWEEP_FRAC * t);
    float a = (1.0f - t);
    a = a * a * 0.85f;
    float r = hr, g = hg, b = hb;
    whiten(0.70f, r, g, b);
    env.apply(r, g, b, a);
    // Anchored at the reticle — in DEPTH as well as in position, so the two
    // fuse as one object: the player flew through the hoop, the wrap is around
    // THEM. That also keeps this burst inside the RET_ZEYE comfort limit; it
    // is a big, bright, dead-centre ring, i.e. exactly the shape that hurts
    // most at crossed disparity.
    ringStroke(strokes, ANCHOR_X, ANCHOR_Y, rUi, 12, 0.0f,
               0.0040f + 0.0040f * (1.0f - t), r, g, b, a, 2, RET_ZEYE);
}

// ---- the '<  >' reticle ------------------------------------------------------
void warpBuildReticle(WarpStrokePool& strokes, const GameEngine& engine,
                      const EnvTint& env, float hr, float hg, float hb) {
    const float beatK = wclamp01(engine.audio.beat * engine.audio_pulse_k);
    const float depth = engine.audio_safe_mode ? RET_BEAT_DEPTH_SAFE
                                               : RET_BEAT_DEPTH;
    const float s = 1.0f + depth * beatK;

    float r = hr, g = hg, b = hb;
    whiten(RET_WHITEN, r, g, b);
    float a = RET_ALPHA;
    env.apply(r, g, b, a);

    const float cx = ANCHOR_X, cy = ANCHOR_Y;
    const float tip = RET_TIP_X * s, arm = RET_ARM_X * s, h = RET_H * s;
    // STEREO COMFORT (hardware play-test): the avatar parks just BEHIND the
    // zero-parallax plane, never at the judgment plane it used to share with
    // the hoop — see the RET_ZEYE block, including why the WYSIWYG containment
    // law is untouched by it.
    const float zeye = RET_ZEYE;
    // Left chevron '<' — tip outermost, arms opening toward the catch point.
    glowStroke(strokes, cx - tip, cy, cx - tip + arm, cy + h,
               RET_HALF_W, r, g, b, a, 3, zeye);
    glowStroke(strokes, cx - tip, cy, cx - tip + arm, cy - h,
               RET_HALF_W, r, g, b, a, 3, zeye);
    // Right chevron '>' — the X-mirror.
    glowStroke(strokes, cx + tip, cy, cx + tip - arm, cy + h,
               RET_HALF_W, r, g, b, a, 3, zeye);
    glowStroke(strokes, cx + tip, cy, cx + tip - arm, cy - h,
               RET_HALF_W, r, g, b, a, 3, zeye);
}

// ---- the popup detonation ----------------------------------------------------
void warpBuildTextBurst(WarpTextPool& dots, WarpFxState& st,
                        const GameEngine& engine, const EnvTint& env) {
    using ts::shatter::EventView;
    using ts::shatter::WorldDot;

    // Mirror every warp-owned event (all of them, seventh verdict item 3).
    EventView ev[ts::shatter::SLOTS];
    bool live = false;
    for (int i = 0; i < ts::shatter::SLOTS; ++i) {
        const auto& e = engine.shatter_events[i];
        if (warpOwnsEvent(e.text) && e.kind) {
            ev[i] = { e.text, e.starttime, e.kind };
            const int age = engine.time - e.starttime;
            if (age >= 0 && age < ts::shatter::durationMs(e.kind)) live = true;
        } else {
            ev[i] = { "", -1, 0 };
        }
    }
    if (!live) return;

    ts::shatter::sync(st.txtState, ev, ts::shatter::SLOTS);

    static WorldDot wd[TXT_DOT_CAP];   // fixed scratch, drop-not-grow
    const float beat = wclamp01(engine.audio.beat * engine.audio_pulse_k);
    // NOTE: per-event deterministic placement + roll identity hash.
    /* @vfp-exempt R2,R3 — text-burst event loop (pins seed hash, glyph compensation, roll projection, culls below): seed is per-event integer identity; compensation/culls define the legibility law. measured n/a. Verified 2026-09-06. */
    for (int slot = 0; slot < ts::shatter::SLOTS; ++slot) {
        if (!ev[slot].text[0]) continue;

        // Per-event deterministic placement + roll (seventh verdict items
        // 1-2): quadrant and angle from the event's own identity hash.
        const uint32_t seed =
            (uint32_t)ev[slot].starttime * 2654435761u
            ^ (uint32_t)slot * 0x9E3779B9u
            ^ (uint32_t)(unsigned char)ev[slot].text[0] * 101u;
        const uint32_t quad = (uint32_t)(hash01(seed) * 4.0f) & 3u;
        const float ex = ANCHOR_X + ((quad & 1u) ? -TXT_OFF_XY : TXT_OFF_XY);
        const float ey = ANCHOR_Y + ((quad & 2u) ? -TXT_OFF_XY : TXT_OFF_XY);
        const float ang = (hash01(seed ^ 0x51ED270Bu) * 2.0f - 1.0f)
                        * TXT_TILT_MAX_DEG * 0.017453293f;
        const float ca = ts::fastCos(ang), sa = ts::fastSin(ang);
        // Glyph-readability compensation (TXT_LEN_* block above).
        /* @vfp-exempt R3 — glyph length compensation clamps: sqrt(strlen) scale keeps long strings legible; floor/ceiling bind the range. measured n/a. Verified 2026-09-06. */
        float lenK = std::sqrt((float)std::strlen(ev[slot].text)
                               * (1.0f / TXT_LEN_REF));
        if (lenK < 1.0f) lenK = 1.0f;
        if (lenK > TXT_LEN_MAX) lenK = TXT_LEN_MAX;
        const float rUi = TXT_R_UI * lenK;

        // anchorZn 0: the warp has no tube dive — the burst parks on the
        // module's resting rim frame. yesBeat nullptr/0: the documented
        // fallback; the warp events never carry STYLE_YES.
        const int n = ts::shatter::evalWorld(st.txtState, slot, engine.time,
                                             beat, engine.audio_safe_mode,
                                             engine.current_level,
                                             /*anchorZn=*/0.0f,
                                             /*yesBeatMs=*/nullptr,
                                             /*yesBeatCount=*/0,
                                             wd, TXT_DOT_CAP);
        /* @vfp-exempt R3 — text-burst dot loop (pins div guard, roll projection, viewport/size/alpha culls below): discard set for the tilted popup. measured n/a. Verified 2026-09-06. */
        for (int i = 0; i < n; ++i) {
            const WorldDot& d = wd[i];
            const float div = d.zn + TXT_EYE_N;
            if (div <= 0.01f) continue;  // belt+braces under the module floor
            const float s = TXT_FOCAL / div;
            // Projected offset from the event's own anchor, then the roll —
            // rotating AFTER projection keeps the shatter's radial blast
            // geometry intact under the tilt.
            //
            // ORIENTATION CONVENTION (mirror audit, eighth verdict item 2):
            // the module's WorldDot frame is +tx = reading direction,
            // +ty = UP — d.u/d.v are the game font's own segment coordinates
            // (FONT_DATA is authored y-up: writeAfont draws segments raw
            // into the same y-up 0..1.3333 x 0..1 UI box this warp uses,
            // and evalWorld scales u,v by strictly positive k/kY). The
            // pinhole scale s is positive (div > 0.01 guard) and the roll
            // below is a proper CCW rotation (det = +1). Every stage has
            // positive determinant, so this path CANNOT mirror glyphs in
            // either axis; the historical "mirrored" reports were the old
            // [15°,165°] tilt law rolling text near-upside-down.
            const float ox = d.tx * rUi * s;
            const float oy = d.ty * rUi * s;
            const float x = ex + ox * ca - oy * sa;
            const float y = ey + ox * sa + oy * ca;
            /* @vfp-exempt R3 — text-burst discard set: viewport cull, dot-size cap, alpha cull gate popup emission. measured n/a. Verified 2026-09-06. */
            /* @vfp-exempt R3 — popup viewport cull (pins the if below; nested segments split attribution). measured n/a. Verified 2026-09-06. */
            if (x < -0.06f || x > UI_W + 0.06f || y < -0.06f || y > 1.06f)
                continue;
            float size = d.size * rUi * s;
            if (size > TXT_DOT_MAX) size = TXT_DOT_MAX;
            float r = d.r, g = d.g, b = d.b, a = d.a;
            env.apply(r, g, b, a);
            /* @vfp-exempt R3 — text-burst alpha cull: skips invisible popup dots. measured n/a. Verified 2026-09-06. */
            if (a <= 0.004f) continue;
            dots.emit(x, y, size, r, g, b, a, TXT_ZEYE);
        }
    }
}

} // namespace warpgeom
} // namespace ts
