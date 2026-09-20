// gameover_geometry.cpp -- see gameover_geometry.h for the contract.
//
// The laws here are logo_geometry's, deliberately: the title screen already
// solved "a wordmark that bends and stretches over a feedback plume", and the
// recovered constants live in its header. What differs is the palette (fixed,
// from the SVG -- a game over is not a celebration and nothing lerps with the
// music) and the fact that the art is 1.66x taller, which the derived ripple
// wavelength absorbs.

#include "gameover_geometry.h"

#include <cmath>

#include "../game/engine.h"
#include "../game/math_lut.h"   // fastSin -- the same trig both backends use
#include "../ui/pause_fx.h"       // MELT_ZOOM -- the chamber's resting depth rate

namespace ts {
namespace {

constexpr float TWO_PI = 6.28318530718f;
inline float sinT(float turns) { return ts::fastSin(turns * TWO_PI); }
inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// The chamber sweep this screen supplies. Slower than the title's: the plume
// should feel like it is settling, not announcing.
constexpr float GO_FB_ROT_MAX    = 0.05f;    // radians
constexpr float GO_FB_ROT_TURNS  = 0.011f;   // per second
constexpr float GO_FB_WOB_TURNS  = 0.30f;    // per second


} // namespace

// ---------------------------------------------------------------------------
float gameoverRamp(const GameEngine& engine) {
    // THE RAMP STARTS AT THE DEATH, NOT 3 SECONDS LATER.
    //
    // nolives_animation does not begin until gameover_animation has run its
    // whole 10 -> 200 dive: 190 ticks, 3.04 s. Keying the presentation off
    // nolives alone meant the ramp was ZERO for that entire window -- so the
    // web sat at full brightness, unfaded, and (since the spiral is suppressed
    // on the final life to save the starfield) it did not fly away either. It
    // just hung there for three seconds. Every "make the fade faster" pass
    // before this one was shortening a fade that had not started yet.
    //
    // So progress is measured from the FINAL death itself, across both clocks:
    // the dive's own ticks first, then nolives. lives is already -1 by the time
    // gameover_animation is set (init_gameover decrements before), so a
    // survivable death never enters this and returns 0.
    //
    // THE HARNESS HAS TO GET PAST THIS GATE. gameover_test drives
    // nolives_animation and deliberately touches NO gameplay state, so it never
    // sets lives < 0 -- which means the gate added above silently killed the
    // look harness the moment it landed, and the instrument produced a blank
    // screen while looking like it worked. Exempting it here keeps the harness
    // presentation-only, which is its whole contract; the alternative (having
    // the frontends fake lives = -1) would make an instrument mutate play state.
    if (engine.player.lives >= 0 && !engine.gameover_test) return 0.0f;
    const float dive = engine.player.gameover_animation > 10
                     ? (float)(engine.player.gameover_animation - 10) : 0.0f;
    return clamp01((dive + (float)engine.nolives_animation) * (1.0f / 400.0f));
}

// ---------------------------------------------------------------------------
// The stage sub-ramps. Each is the whole ramp remapped onto its own slice and
// clamped, so a consumer never has to know where the boundaries are.
float gameoverWebT(float ramp) {
    return clamp01(ramp / GO_STAGE_WEB_END);
}
// The rush STARTS BEFORE THE WEB HAS FINISHED LEAVING -- the windows overlap on
// purpose, so the sequence reads as one motion rather than three steps.
float gameoverRushT(float ramp) {
    constexpr float lo = GO_STAGE_WEB_END * 0.6f;
    return clamp01((ramp - lo) / (GO_STAGE_RUSH_END - lo));
}
float gameoverTextT(float ramp) {
    return clamp01((ramp - GO_STAGE_RUSH_END) /
                   (GO_STAGE_TEXT_END - GO_STAGE_RUSH_END));
}
float gameoverMeltT(float ramp) {
    return clamp01((ramp - GO_STAGE_TEXT_END) / (1.0f - GO_STAGE_TEXT_END));
}

// ---------------------------------------------------------------------------
void gameoverStepState(GameOverFxState& st, const GameEngine& engine) {
    const int now = engine.time;

    // ENTRY, detected by the clock rather than a token -- the LogoFxState
    // pattern. Not having drawn this screen for GO_ENTRY_GAP_MS means a fresh
    // game over, which re-arms `entered` so the backends restart the melt loop
    // from black. The plume growing out of black IS the entrance; there is no
    // separate fly-in, exactly as the title screen does it.
    const int gap = now - st.lastMs;
    st.entered = !st.live || gap < 0 || gap > GO_ENTRY_GAP_MS;
    st.live = true;
    if (st.entered) {
        st.phaseX = st.phaseY = 0.0f;
        st.rippleL = st.rippleR = 0.0f;
        st.fbRotPhase = st.fbWobPhase = 0.0f;
        st.ramp = 0.0f;
        st.lastMs = now;
    }

    // dt in frames at the sim's 62.5 Hz, clamped so a hitch cannot fling every
    // oscillator forward (the pause ramp's dt-clamp rule).
    float dtMs = (float)(now - st.lastMs);
    if (dtMs < 0.0f)  dtMs = 0.0f;
    if (dtMs > 64.0f) dtMs = 64.0f;
    st.lastMs = now;
    const float dtFrames = dtMs * (62.5f / 1000.0f);
    const float dtSec    = dtMs * 0.001f;

    // Two INCOMMENSURATE breathe oscillators (logo_geometry fact 2).
    st.phaseX += dtFrames / GO_BREATHE_FRAMES_X;
    st.phaseY += dtFrames / GO_BREATHE_FRAMES_Y;
    st.phaseX -= (float)(int)st.phaseX;
    st.phaseY -= (float)(int)st.phaseY;
    st.breatheX = 1.0f + GO_BREATHE_REL * sinT(st.phaseX);
    st.breatheY = 1.0f + GO_BREATHE_REL * sinT(st.phaseY);

    // Both ripplewarp edges, at the reference's own row rates. Phases are kept
    // in TURNS and wrapped every frame -- the LUT's error grows with the
    // argument's magnitude (DOCTRINE.md), so a phase that free-runs for minutes
    // must never be handed to it raw.
    st.rippleL += dtFrames * GO_RIPPLE_RATE_L / GO_RIPPLE_ROWS;
    st.rippleR += dtFrames * GO_RIPPLE_RATE_R / GO_RIPPLE_ROWS;
    st.rippleL -= (float)(int)st.rippleL;
    st.rippleR -= (float)(int)st.rippleR;

    st.fbRotPhase += dtSec * GO_FB_ROT_TURNS;
    st.fbWobPhase += dtSec * GO_FB_WOB_TURNS;
    st.fbRotPhase -= (float)(int)st.fbRotPhase;
    st.fbWobPhase -= (float)(int)st.fbWobPhase;

    // The envelope. nolives_animation is the SAME 400-tick clock the sim freeze
    // keys off, kept precisely so this screen and the freeze cannot disagree
    // about when the game is over.
    st.ramp = gameoverRamp(engine);

    // ---- THE TRAIL STEP ----------------------------------------------------
    // Re-rolled when the current one expires, at a RANDOM interval of
    // GO_TRAIL_MS_MIN..MAX. A fixed cadence would beat against the sim tick and
    // the chamber's own trail life and read as a pulse; irregular never settles
    // into a rhythm.
    //
    // A cheap integer hash of the step index -- deterministic, no rand(), no
    // shared RNG to perturb -- gives three decorrelated draws: the compass, the
    // depth rate, and the interval until the next roll.
    if (st.entered || now >= st.trailNextMs) {
        unsigned h = (unsigned)(++st.trailSeq) * 2654435761u;
        h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
        const float a = (float)( h        & 0x3FFu) * (1.0f / 1024.0f);
        const float b = (float)((h >> 10) & 0x3FFu) * (1.0f / 1024.0f);
        const float c = (float)((h >> 20) & 0x3FFu) * (1.0f / 1024.0f);
        st.trailLobe = a;                                  // turns, full circle
        // Depth: vary MELT_ZOOM by +-GO_TRAIL_Z_VAR so each step's trails leave
        // at their own speed toward the viewer, not just in their own direction.
        st.trailZoom = MELT_ZOOM * (1.0f + GO_TRAIL_Z_VAR * (b * 2.0f - 1.0f));
        st.trailNextMs = now + (int)(GO_TRAIL_MS_MIN +
                                     c * (GO_TRAIL_MS_MAX - GO_TRAIL_MS_MIN));
    }
}

// ---------------------------------------------------------------------------
void gameoverFeedbackParams(const GameOverFxState& st, float& rotRad, float& wobPhase) {
    rotRad   = GO_FB_ROT_MAX * sinT(st.fbRotPhase);
    wobPhase = st.fbWobPhase * TWO_PI;
}

// ---------------------------------------------------------------------------
void gameoverBuild(GoTriPool& tris, GoStrokePool& strokes,
                   const GameOverFxState& st, float screenH) {
    tris.n = 0;
    strokes.n = 0;
    if (gameoverTextT(st.ramp) <= 0.0f) return;

    // Stroke widths are authored at the 240-line reference and scale by h/240,
    // the *_REF_H convention -- never author a stroke width in device pixels.
    const float wScale = (screenH > 1.0f ? screenH : 240.0f) / 240.0f;

    // THE WHOLE-IMAGE TRANSFORM plus the ripplewarp, exactly logo_geometry's
    // law. The row is read AFTER the vertical breathe so the wave is a property
    // of the DESTINATION row, which is what the reference's raster remap is;
    // reading it before packs a whole cycle into the letterforms and folds
    // strokes through each other.
    constexpr float X_SPAN_INV = 1.0f / (gameover::X_MAX - gameover::X_MIN);
    const float sx = GO_HALF_W  * st.breatheX;
    const float sy = GO_SCALE_Y * st.breatheY;

    auto xform = [&](const gameover::Pt& p, float& ox, float& oy) {
        oy = GO_CY + p.y * sy;                                  // the screen row
        const float t  = oy * GO_RIPPLE_WAVES_PER_SCR;
        const float xl = -1.0f + GO_RIPPLE_AMP * sinT(t + st.rippleL);
        const float xr =  1.0f + GO_RIPPLE_AMP * sinT(t + st.rippleR);
        const float u  = (p.x - gameover::X_MIN) * X_SPAN_INV;  // 0..1 across it
        ox = GO_CX + (xl + (xr - xl) * u) * sx;
    };

    // The envelope drives INTENSITY only. Hue is identity: the wordmark's
    // colours are the SVG's and do not move, so what fades in is brightness.
    // TEXT STAGE, not the whole ramp: the wordmark lands after the starfield
    // has warped up, and before the melt blooms behind it.
    // DEMO's own breath (demo_overlay.h: 0.72 + 0.28 sin, ~3.9 s), on top of
    // the fade-in. Intensity is the event and hue never moves, so a lit tube
    // that breathes is the whole of this wordmark's life once it has landed.
    // The phase is wrapped into one period before the LUT sees it -- the
    // table's error grows with |rad| and engine.time is milliseconds.
    // R11 (AGENTS.md): the modulo divisor is an explicit constexpr int, not an
    // inline (int)(TWO_PI / RATE). ARMv6 has NO integer divide instruction, so a
    // runtime divisor here would be a `bl __aeabi_idivmod` -- tens of cycles
    // plus a call barrier that spills live registers. A compile-time constant
    // folds to a magic-number multiply instead. It folded either way as written,
    // but nothing ENFORCED that: make PULSE_RATE runtime and the libcall appears
    // silently. The gates do not detect R11 yet, so the guard has to be the
    // declaration.
    constexpr float PULSE_RATE   = 0.0016f;
    constexpr int   PULSE_PERIOD = (int)(TWO_PI / PULSE_RATE);   // 3926 ms
    static_assert(PULSE_PERIOD > 0, "pulse period must be positive");
    const float php  = (float)(st.lastMs % PULSE_PERIOD) * PULSE_RATE;
    const float pulse = 0.72f + 0.28f * ts::fastSin(php);
    const float A = gameoverTextT(st.ramp) * pulse;

    // --- THE FACES ARE GONE -- THIS IS AN OUTLINE WORDMARK ------------------
    // User, 2026-09-08: "we should follow the design language of the DEMO
    // text." The single biggest thing that separated the two was that DEMO is
    // OUTLINE ONLY (ui/demo_overlay.h) while this still carried the SVG's
    // filled letterform bodies plus a gloss.
    //
    // Dropping them is what makes it read as lit tube rather than as painted
    // slab, and it is also what lets the melt through the letters instead of
    // masking it -- the same reason demo_overlay gives for its own outlines
    // ("a filled slab would mask the tube"). Here the thing behind is the
    // plume rather than the web, and the argument is identical.
    //
    // GoTriPool stays in the seam and simply comes back empty. Both backends
    // already draw a zero-count range, so nothing downstream changes and the
    // declared FACES-then-OUTLINES replay order still holds -- there is just
    // nothing in the first half of it now.
    (void)tris;

    // --- REPLAY ORDER, PART 2: the outlines ---------------------------------
    // A glyph here is a LIST OF CONTOURS, not one loop: A and O enclose a
    // counter and R separates into two components at this stroke weight. Each
    // contour is closed, so the last point joins back to the first, and each
    // gets its own rim -- which is what puts a proper edge around the counters.
    for (int gi = 0; gi < gameover::GLYPH_COUNT; ++gi) {
        const gameover::Glyph& G = gameover::GLYPHS[gi];
        for (int ci = 0; ci < G.nContours; ++ci) {
            const gameover::Contour& C = G.contours[ci];
            if (C.n < 2) continue;
            float px, py;
            xform(C.pts[C.n - 1], px, py);
            for (int i = 0; i < C.n; ++i) {
                float cx, cy;
                xform(C.pts[i], cx, cy);
                // Skirt, cyan mid, hot core -- widest first, so the narrow
                // bright core lands on top. demo_overlay.h's NEON stack, its
                // alphas included; three stops over one ICE row is what makes
                // a stroke read as a lit tube instead of an outline.
                strokes.emit(px, py, cx, cy, GO_OUTER_HALFPX * wScale,
                             GO_OUTER.r, GO_OUTER.g, GO_OUTER.b, GO_OUTER_A * A, 0.0f);
                strokes.emit(px, py, cx, cy, GO_MID_HALFPX * wScale,
                             GO_MID.r, GO_MID.g, GO_MID.b, GO_MID_A * A, 0.0f);
                strokes.emit(px, py, cx, cy, GO_INNER_HALFPX * wScale,
                             GO_INNER.r, GO_INNER.g, GO_INNER.b, GO_INNER_A * A, 0.0f);
                px = cx; py = cy;
            }
        }
    }
}

} // namespace ts
