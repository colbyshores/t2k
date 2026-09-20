// ============================================================================
// logo_geometry.cpp — the animated title logo's build half, shared by BOTH
// backends. See logo_geometry.h for the recovered the arcade reference laws this implements
// and for the seam contract (docs/design/title_logo.md).
//
// No GL, no citro3d, no renderer includes. Pure data + free functions over
// GameEngine, fixed pools, wall-clock-integrated accumulators.
// ============================================================================

#include "logo_geometry.h"

#include <cmath>

#include "../game/engine.h"
#include "../game/math_lut.h"   // fastSin — the same trig both backends use
#include "web_palette.h"        // webHsv2rgb

namespace ts {
namespace logogeom {

namespace {

// sin of a phase expressed in TURNS (0..1), which is how every phase in this
// module is stored — the reference's laws are all "one wave over N frames".
inline float sinT(float turns) { return ts::fastSin(turns * TWO_PI); }

// Wrap a turn-phase into [0,1) so it never loses float precision over a long
// sit on the title screen (the ripple's 34 s beat is a lot of frames).
inline float wrap1(float t) { return t - std::floor(t); }

// The 4-pass core->halo glow stack, expanded HERE in the shared builder so both
// backends replay exactly the same primitive list in exactly the same order.
inline void glowStroke(LogoStrokePool& sp,
                       float x1, float y1, float x2, float y2,
                       const EnvTint& env) {
    for (int p = 0; p < GLOW_PASSES; ++p) {
        // Pass 0 is the SVG's bright core line; the halo passes are its wide
        // purple aura. Two FIXED hues -- the music moves intensity, never hue.
        float r = p ? AURA_R : CORE_R;
        float g = p ? AURA_G : CORE_G;
        float b = p ? AURA_B : CORE_B;
        float a = OUTLINE_ALPHA * GLOW_A_MUL[p];
        env.apply(r, g, b, a);
        sp.emit(x1, y1, x2, y2, OUTLINE_HALF_W * GLOW_W_MUL[p],
                r, g, b, a, LOGO_ZEYE);
    }
}

} // namespace

// ---- the screen's persistent state ------------------------------------------
void logoStepState(LogoFxState& st, const GameEngine& engine) {
    const int now = engine.time;

    // ENTRY. Detected by the clock, not by a token (see the LogoFxState block):
    // the title screen not having been drawn for LOGO_ENTRY_GAP_MS means this is
    // a fresh entry -- true at boot and true again on every return from
    // gameplay. `entered` is what tells the backends to restart the melt loop
    // from black, which is the reference's whole intro (design doc fact 5).
    const int gap = now - st.lastMs;
    st.entered = !st.live || gap < 0 || gap > LOGO_ENTRY_GAP_MS;
    st.live = true;
    if (st.entered) {
        st.phaseX = st.phaseY = 0.0f;
        st.rippleL = st.rippleR = 0.0f;
        st.fbRotPhase = 0.0f;
        st.fbWobPhase = 0.0f;
        // The face colour starts ON its silent resting value rather than at
        // black, so a fresh entry does not fade the face up through grey.
        webHsv2rgb(FACE_HUE_BASE, FACE_SAT, FACE_VAL_BASE,
                   st.faceR, st.faceG, st.faceB);
        st.lastMs = now;
    }

    // Wall-clock dt, clamped: a long stall (a level-texture generation landing,
    // a debugger) must not teleport the oscillators.
    int dtMs = now - st.lastMs;
    if (dtMs < 0)   dtMs = 0;
    if (dtMs > 100) dtMs = 100;
    st.lastMs = now;
    const float dt = (float)dtMs * 0.001f;
    const float dFrames = dt * REF_FPS;   // the reference's own unit

    // --- THE BREATHE: two incommensurate sines, never coupled ---------------
    st.phaseX = wrap1(st.phaseX + dFrames / BREATHE_FRAMES_X);
    st.phaseY = wrap1(st.phaseY + dFrames / BREATHE_FRAMES_Y);
    st.breatheX = (BREATHE_MID + BREATHE_AMP * sinT(st.phaseX)) * BREATHE_NORM;
    st.breatheY = (BREATHE_MID + BREATHE_AMP * sinT(st.phaseY)) * BREATHE_NORM;

    // --- THE BEND: the two ripplewarp source edges, beating over ~34 s ------
    st.rippleL = wrap1(st.rippleL + dFrames * RIPPLE_RATE_L / RIPPLE_ROWS);
    st.rippleR = wrap1(st.rippleR + dFrames * RIPPLE_RATE_R / RIPPLE_ROWS);

    // --- the chamber's own two phases (the title screen has no roll) --------
    st.fbRotPhase = wrap1(st.fbRotPhase + dt / FB_ROT_PERIOD_S);
    st.fbWobPhase += dt * ts::railgeom::FB_WOBBLE_HZ * TWO_PI;
    if (st.fbWobPhase > 1.0e6f) st.fbWobPhase -= 1.0e6f;   // precision guard

    // --- THE FACE LERPS WITH THE MUSIC (the recorded divergence) ------------
    // All-zero audio is the legal calm state: the target then relaxes to the
    // SVG's own baked purple and the face sits still, which is exactly the
    // reference's behaviour when nothing is playing.
    const bool safe = engine.audio_safe_mode;
    const float pk  = engine.audio_pulse_k;
    const float rms = wclamp01(engine.audio.rms * pk);
    const float cen = wclamp01(engine.audio.spectralCentroid);
    const float hueSpan = safe ? FACE_HUE_SPAN_SAFE : FACE_HUE_SPAN;
    const float valRms  = safe ? FACE_VAL_RMS_SAFE  : FACE_VAL_RMS;
    float tr, tg, tb;
    webHsv2rgb(FACE_HUE_BASE + hueSpan * cen, FACE_SAT,
               wclamp01(FACE_VAL_BASE + valRms * rms), tr, tg, tb);
    // Frame-rate-independent approach, never a snap -- RAIL's own hue law.
    const float k = 1.0f - std::exp(-dt / FACE_TAU_S);
    st.faceR += (tr - st.faceR) * k;
    st.faceG += (tg - st.faceG) * k;
    st.faceB += (tb - st.faceB) * k;

    // --- INTENSITY IS EVENT: the beat whiten + the web's own bass duck ------
    const float beat = wclamp01(engine.audio.beat * pk);
    float duck = DUCK_BEAT * beat + DUCK_RMS * rms;
    if (duck > DUCK_MAX) duck = DUCK_MAX;
    if (safe) duck *= DUCK_DEPTH_SAFE;
    st.env.wm   = (safe ? WHITEN_BEAT_SAFE : WHITEN_BEAT) * beat;
    st.env.gain = 1.0f - duck;
}

void logoFeedbackParams(const LogoFxState& st, float& rotRad, float& wobPhase) {
    rotRad   = FB_ROT_MAX * sinT(st.fbRotPhase);
    wobPhase = st.fbWobPhase;
}

// ---- the frame's primitives -------------------------------------------------
void logoBuild(LogoTriPool& tris, LogoStrokePool& strokes,
               const LogoFxState& st) {
    // THE PER-VERTEX TRANSFORM. Both recovered laws live here and nowhere else,
    // so a face vertex and an outline vertex can never disagree about where the
    // logo is this frame:
    //   1. ripplewarp bends x by the vertex's own ROW (the genuine bend);
    //   2. the two breathe oscillators scale the result, independently on each
    //      axis (the arcade reference's whole-image 2x2, expressed on vectors).
    // Order matters: the bend's x remap is a SOURCE-space remap in the
    // reference, so it is applied in art units and the breathe scales the bent
    // art -- not the other way round, which would make the bend amplitude
    // breathe too.
    //
    // THE ROW IS THE DESTINATION ROW -- the vertex's own y in the UI box, NOT
    // its normalised height inside the wordmark. The reference's sine is a
    // function of the raster row and the art merely passes through it; making
    // it a function of the ART's own height packs a whole cycle into the
    // letterforms and folded the K through itself. Reading the row AFTER the
    // vertical breathe is also what keeps the shear gradient a property of the
    // wave and RIPPLE_AMP alone.
    //
    // AND THE WAVELENGTH IS NOT FREE: a stroke is drawn as the straight CHORD
    // between two warped endpoints, so it can only follow the bend while it
    // spans a small part of a cycle. RIPPLE_WAVES_PER_SCREEN is derived from
    // RIPPLE_WAVE_SPAN and the art's own height for exactly that reason. See
    // logo_geometry.h's BEND and CLEARANCE blocks -- both carry the derivation
    // and the numbers the host harness measured.
    constexpr float X_SPAN_INV = 1.0f / (logo::X_MAX - logo::X_MIN);
    const float sx = LOGO_HALF_W  * st.breatheX;
    const float sy = LOGO_SCALE_Y * st.breatheY;

    auto xform = [&](const logo::Pt& p, float& ox, float& oy) {
        oy = LOGO_CY + p.y * sy;                             // the screen row
        const float t  = oy * RIPPLE_WAVES_PER_SCREEN;
        const float xl = -1.0f + RIPPLE_AMP * sinT(t + st.rippleL);
        const float xr =  1.0f + RIPPLE_AMP * sinT(t + st.rippleR);
        const float u  = (p.x - logo::X_MIN) * X_SPAN_INV;   // 0..1 across it
        ox = LOGO_CX + (xl + (xr - xl) * u) * sx;
    };

    // --- REPLAY ORDER, PART 1: the faces ------------------------------------
    // The surface whose colour lerps with the music, ear-clipped by the
    // generator because the glyphs are concave.
    {
        float r = st.faceR, g = st.faceG, b = st.faceB, a = FACE_ALPHA;
        st.env.apply(r, g, b, a);
        for (int gi = 0; gi < logo::GLYPH_COUNT; ++gi) {
            const logo::Glyph& G = logo::GLYPHS[gi];
            for (int i = 0; i + 2 < G.nTris; i += 3) {
                float x0, y0, x1, y1, x2, y2;
                xform(G.face[G.tris[i + 0]], x0, y0);
                xform(G.face[G.tris[i + 1]], x1, y1);
                xform(G.face[G.tris[i + 2]], x2, y2);
                tris.emit(x0, y0, x1, y1, x2, y2, r, g, b, a, LOGO_ZEYE);
            }
        }
    }

    // --- REPLAY ORDER, PART 2: the glow outlines ----------------------------
    // Closed loops, so the last point joins back to the first.
    for (int gi = 0; gi < logo::GLYPH_COUNT; ++gi) {
        const logo::Glyph& G = logo::GLYPHS[gi];
        if (G.nOutline < 2) continue;
        float px, py;
        xform(G.outline[G.nOutline - 1], px, py);
        for (int i = 0; i < G.nOutline; ++i) {
            float cx, cy;
            xform(G.outline[i], cx, cy);
            glowStroke(strokes, px, py, cx, cy, st.env);
            px = cx; py = cy;
        }
    }
}

} // namespace logogeom
} // namespace ts
