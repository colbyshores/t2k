#pragma once
// ============================================================================
// pause_fx.h — the pause presentation, shared by both targets (the UI_rework
// mark-up, dark red arrow).
//
// WHAT IT IS: pausing during gameplay no longer cuts to a dark screen with
// text. The frame keeps rendering, frozen, and:
//   * the world AROUND the web (the starfield) lerps into a stereoscopic
//     melt-o-vision feedback loop (the title/RAIL chamber, MELT_TEX to
//     conserve cycles) whose trails stream OUTWARD toward a handful of points
//     around the screen, while the WEB stays crisp, in focus;
//   * a frost box blurs in over the web carrying the pause/options menu,
//     every transition smoothstep-ramped so nothing jars;
//   * on a BONUS ROUND there is no melt: the frozen round just gets the
//     frost box + menu, ramped the same way.
// Un-pausing runs the same ramp backwards on the normal frame.
//
// This header owns the parts both front-ends and both renderers must agree
// on: the ramp, the box geometry, the melt's parameter law, and the composite
// MASK that keeps the web clear. It is header-only (ui/screen_fade.h's
// pattern) so nothing else needs wiring.
//
// THE CLOCK IS THE WALL CLOCK. Both front-ends set engine.time from the wall
// clock every frame, pause included, so the breaths below keep running while
// the player sits in the menu -- deliberate, and the 3DS twin reproduces it
// for free because main_3ds.cpp does the same thing.
// ============================================================================

#include <cmath>

#include "game/math_lut.h"
#include "game/phase.h"
#include "rendering/rail_geometry.h"
#include "rendering/warp_geometry.h"

namespace ts {

// ---- the ramp ---------------------------------------------------------------
// 0 = live game, 1 = fully paused. Stepped ONCE per frame by the front-end
// (both mains) with the wall-clock dt; the render seam reads the value from
// the engine (pause_fx). ~320 ms in, ~240 ms out, smoothstep-shaped: the melt
// lerps in, the box lerps in, and leaving is the mirror -- never a cut.
struct PauseFx { float t = 0.0f; };

// A single frame's dt is CLAMPED before it reaches the ramp. Every other timed
// effect in this tree derives from engine.time; this is the one wall-clock
// integrator, and one hitch (a window drag, the texture warm) would otherwise
// complete the whole ramp in one frame -- exactly the jar the smoothstep
// exists to prevent.
constexpr int PAUSE_DT_MAX_MS = 50;

inline void pauseFxStep(PauseFx& fx, bool pauseOpen, int dtMs) {
    if (dtMs < 0) dtMs = 0;
    if (dtMs > PAUSE_DT_MAX_MS) dtMs = PAUSE_DT_MAX_MS;
    const float rate = pauseOpen ? (1.0f / 320.0f) : (1.0f / 240.0f);
    fx.t += (pauseOpen ? 1.0f : -1.0f) * rate * (float)dtMs;
    if (fx.t < 0.0f) fx.t = 0.0f;
    if (fx.t > 1.0f) fx.t = 1.0f;
}

// The eased value: smoothstep, so the ends have zero velocity (no jar).
inline float pauseFxEase(float t) { return t * t * (3.0f - 2.0f * t); }

// ---- the frost box (the 1.3333 x 1.0 UI box) --------------------------------
// Centred over the web, sized to hold the open screen's widest AND tallest
// list with the title above the rows (both halves are measured -- see
// pauseBoxHalfWidth / pauseBoxHalfHeight). The menu's overlay rows lay out
// from boxRowY(hh).
// The box sits ON TOP of the web BY DESIGN (the mark-up's own words: "blurred
// box lerps in on top of web with pause/options menu") -- what must stay
// clear of the web is the MELT, which is what the mask below is for.
// THE GLASS IS DARK ENOUGH TO READ TEXT THROUGH (user call, looked at: the
// first cut sat a little over half and the rows fought the web behind them).
// It is still glass -- 18% of the web comes through, and the haze keeps the
// melt faintly visible at the edges where it is behind the pane -- but the
// menu is now the brightest thing inside its own box, which is the point.
constexpr float BOX_CX = 0.6666f, BOX_CY = 0.52f;
// The box's half-width is MEASURED, not fixed: an options row is
// "<label>  <value>", and a scanned soundtrack name can be 31 characters, so
// any constant wide enough for the worst case is far too wide for "paused /
// resume / options / quit to menu". BOX_HW_MIN is the floor (the pause
// screen's own comfortable size) and BOX_HW_MAX keeps the glass off the screen
// edges; between them the box grows to whatever the open screen needs.
constexpr float BOX_HW_MIN = 0.40f;
constexpr float BOX_HW_MAX = 0.63f;   // UI_W/2 - 0.037 of margin each side
constexpr float BOX_PAD_X  = 0.045f;  // clear space between text and glass

// THE HEIGHT IS MEASURED THE SAME WAY THE WIDTH IS. It used to be one constant
// (0.335) sized for the pause screen's three rows, which is why the Controls
// screen -- eleven rows -- drove "Back" straight through the bottom of the
// glass (report, 2026-09-28). The vertical layout is expressed top-down from
// the top edge so the half-height can be SOLVED for the row count rather than
// searched:
//   top edge -> title baseline        BOX_TITLE_PAD
//   title    -> first row baseline   BOX_ROW0_GAP
//   each further row                 BOX_ROW_DY
//   last row   -> bottom edge        BOX_BOTTOM_PAD
// so hh = (TITLE_PAD + ROW0_GAP + (rows-1)*ROW_DY + BOTTOM_PAD) / 2, clamped
// to the range below. BOX_HH_MAX is set by the TOP edge, not the bottom:
// BOX_CY is 0.52, so 0.45 leaves 0.03 of screen margin above and 0.07 below,
// and it covers a 14-row list before the clamp starts to bite.
constexpr float BOX_HH_MIN     = 0.335f;  // the pause screen's own size
constexpr float BOX_HH_MAX     = 0.45f;   // keeps the glass off the screen edges
constexpr float BOX_TITLE_PAD  = 0.075f;
constexpr float BOX_ROW0_GAP   = 0.085f;
constexpr float BOX_ROW_DY     = 0.052f;
constexpr float BOX_BOTTOM_PAD = 0.045f;

// Fit the box to the number of rows it must hold.
inline float pauseBoxHalfHeight(int rows) {
    if (rows < 1) rows = 1;
    const float need = (BOX_TITLE_PAD + BOX_ROW0_GAP
                       + (float)(rows - 1) * BOX_ROW_DY + BOX_BOTTOM_PAD) * 0.5f;
    float hh = need > BOX_HH_MIN ? need : BOX_HH_MIN;
    if (hh > BOX_HH_MAX) hh = BOX_HH_MAX;
    return hh;
}

// Fit the box to the widest row it must hold. `widest` is the widest row's UI
// width (font.h afontWidth); the result is clamped to the range above. The
// caller stores it on the engine and both backends read it there, so the two
// targets cannot size the glass differently.
inline float pauseBoxHalfWidth(float widest) {
    float hw = widest * 0.5f + BOX_PAD_X;
    if (hw < BOX_HW_MIN) hw = BOX_HW_MIN;
    if (hw > BOX_HW_MAX) hw = BOX_HW_MAX;
    return hw;
}

// What a backend should use this frame: the measured width, or the floor when
// nothing has measured one (a ramp-out after the engine was reset, a renderer
// driven with no menu).
inline float pauseBoxHW(float measured) {
    return measured > BOX_HW_MIN ? (measured < BOX_HW_MAX ? measured : BOX_HW_MAX)
                                 : BOX_HW_MIN;
}

inline float pauseBoxHH(float measured) {
    return measured > BOX_HH_MIN ? (measured < BOX_HH_MAX ? measured : BOX_HH_MAX)
                                 : BOX_HH_MIN;
}

// Where the title and the first row sit for a box of half-height `hh`. At
// hh == BOX_HH_MIN these come back at 0.78 and 0.695 -- the values this box
// has always had, to within 6e-8 UI units (~0.01 px at 240p) of float
// reassociation -- so the pause and options screens are unchanged and only a
// taller box moves them.
inline float boxTitleY(float hh) { return BOX_CY + hh - BOX_TITLE_PAD; }
inline float boxRowY(float hh)  { return boxTitleY(hh) - BOX_ROW0_GAP; }

// The glass, in DISPLAY-REFERRED panel units (DOCTRINE.md: the authored value
// IS the panel value on both targets -- a gain "tuned for linear light" is a
// parity bug). Straight-alpha dark pane, then the melt chamber added back
// through it as the blur, then a hairline border.
constexpr float BOX_ALPHA      = 0.82f;   // the dark pane's alpha at full pause
constexpr float BOX_RGB[3]     = {0.010f, 0.014f, 0.046f};
constexpr float BOX_HAZE_GAIN  = 0.30f;   // the chamber, sampled through the glass
constexpr float BOX_EDGE_ALPHA = 0.75f;
// The hairline border: half-thickness in UI units and the four edge colours
// (top, bottom, left, right), brighter along the top to seat the glass in the
// scene's light.
//
// ONLY THE PC READS THESE (t2k_pc/src/rendering/vk_scene.cpp:323-330). The 3DS
// still carries its own literals in t2k_3ds/src/rendering/c3d/09_warp.inc --
// EW = 0.004f and the four colours inline in the frost-box builder -- so
// BOX_EDGE_ALPHA is the only edge value actually shared today. The colours
// agree by hand; the HALF-THICKNESS does not: 0.0022 UI units here against the
// 3DS's 0.004, ~1.8x. Routing the 3DS through this table is therefore a
// deliberate LOOK CHANGE to the reference target, not a cleanup -- do it in its
// own commit with a looked-at Mandarine frame, or move this number to 0.004.
constexpr float BOX_EDGE_HALF  = 0.0022f;
constexpr float BOX_EDGE_RGB[4][3] = {
    {0.45f, 0.75f, 1.00f},   // top
    {0.25f, 0.45f, 0.75f},   // bottom
    {0.30f, 0.55f, 0.85f},   // left
    {0.30f, 0.55f, 0.85f},   // right
};
constexpr float BOX_EDGE_MUL[4] = { 1.0f, 0.8f, 0.8f, 0.8f };

// ---- the melt's chamber ------------------------------------------------------
// The fullscreen chambers run 128^2 (3DS) / 512^2 (PC). The pause bed is the
// score bed's precedent, which set the "the PC runs 2x on each axis" rule: a
// QUARTER of a fullscreen PC chamber, and the 3DS's own 128 -- the number the
// mark-up asked for -- unchanged. At 1080p 128 would be 8.4 screen px per
// texel and the bed reads blocky; 256 is 4.2.
constexpr int MELT_TEX_PC  = 256;
constexpr int MELT_TEX_3DS = 128;

// A draw gap this long re-primes the loop from black: a pause that follows a
// level transition starts clean instead of inheriting a stale smear. Its own
// constant rather than borrowing the score bed's, so the two can be retuned
// apart; 200 ms is under the 240 ms ramp-out, so a pause re-opened after the
// ramp has fully closed always restarts, and a flicker-pause never does.
constexpr int ENTRY_GAP_MS = 200;

// ---- the melt's gain law -----------------------------------------------------
// The chamber's own FB_* constants were tuned against SPARSE injectors (the
// logo strokes, the RAIL dots): the gameplay starfield is ~600 stars, each
// smeared into a long trail by the outward zoom, so the loop needs a colder
// injector and a shorter memory or the whole field integrates to a white wall
// (looked at: the first run of this feature).
//   trail life N     = ln(0.05)/ln(DECAY)      = 28.4 passes = 0.47 s at 60 Hz
//   per-pixel ceiling = INJECT/(1-DECAY)        = 0.30 panel units
//   light held        = INJECT/(1-DECAY*zoom^2) = 0.99 field-equivalents at t=1
// so the field turns INTO trails without gaining or losing light, and the
// composite below restores what STAR_KEEP took out of the direct draw.
// The injected star's core half-width IN CHAMBER PIXELS. Authored here rather
// than derived from the screen's LINE_HALFPX_SCALE, because the injector draws
// into a MELT_TEX-wide target, not the screen: referred to screen pixels the
// same star is 8x fatter at 1080p than at the chamber's own scale and 17x at
// 4K, so both the melt's look and the LOOP'S GAIN would change with output
// resolution. One chamber pixel is also the right size on its own terms -- at
// MELT_TEX_PC over 1080 lines a texel is ~4 screen px, which is a star.
constexpr float MELT_STAR_HALFPX = 1.15f;

// THE INJECT IS PER STORAGE CLASS, and this is the one place the two targets
// legitimately carry different numbers -- the parameter is the CHAMBER'S
// PRECISION, which DOCTRINE.md already records as a 3DS-forced divergence
// (16-bit history on PC, RGBA8 on PICA).
//
// WHY: the starfield is a DIM injector -- a resting star is ~0.2 panel units,
// where the title's logo strokes and RAIL's dots are near 1.0, which is why
// the shared FB_INJECT works for them. On an 8-bit store an injected peak of
// 0.09 x 0.2 = 0.018 lands as 4/255, and the 0.90 decay then rounds it to
// nothing in a handful of passes: the loop ran correctly, primed, with ~540
// star quads per eye going in, and the chamber was BLACK (looked at on
// Mandarine; at inject 1.0 the same build showed full trails). The float
// chamber has no such floor. So the 8-bit path is authored to put the peak
// well clear of the LSB, and the trail survives its full ~28 passes on both.
constexpr float PAUSE_MELT_INJECT       = 0.09f;   // float history (PC)
constexpr float PAUSE_MELT_INJECT_SAFE  = 0.045f;
constexpr float PAUSE_MELT_INJECT_8BIT  = 0.75f;   // RGBA8 history (PICA200)
constexpr float PAUSE_MELT_INJECT_8BIT_SAFE = 0.375f;
constexpr float PAUSE_MELT_DECAY       = 0.90f;   // N = 28.4 passes (5% threshold)
// The composite's gain at full pause (it lerps in with the ramp). Solved for
// LIGHT CONSERVATION against STAR_KEEP: composite = (1 - STAR_KEEP)/0.99, so
// the paused surroundings hold the light the live field had -- the pause is a
// transformation, not a fade.
constexpr float PAUSE_MELT_COMPOSITE   = 0.70f;
// What the DIRECT star draw keeps at full pause. Not 0: a crisp head at the
// tip of every smear is the only thing that makes the bed read as MOTION
// rather than as a blur.
constexpr float PAUSE_STAR_KEEP        = 0.30f;

// THE INJECT MUST NOT SEE STAR_KEEP. The direct draw dims and the chamber
// takes its place; if the chamber's injector were dimmed too, the melt's own
// light source would fade exactly as the composite meant to display it ramps
// up, and the bed would peak mid-ramp and then recede. Backends inject from
// the UNDIMMED star tint -- this helper names the rule so neither can forget.
inline float pauseStarDim(float easedT) { return 1.0f - (1.0f - PAUSE_STAR_KEEP) * easedT; }

// ---- the melt's stereo depth -------------------------------------------------
// The mark-up asks for a STEREOSCOPIC melt. Same convention the retired score
// bed used and the title logo still uses: the zero-parallax plane is
// warpgeom::WARP_FOCUS_ZEYE, and a larger z sits further BEHIND the glass
// (parallax factor 1 - FOCUS/z). The bed is the far wall the box hangs in
// front of; the box and its text sit exactly ON the glass, because text is
// for reading.
constexpr float MELT_ZEYE = warpgeom::WARP_FOCUS_ZEYE * 3.0f;   // 0.667 parallax
constexpr float BOX_ZEYE  = warpgeom::WARP_FOCUS_ZEYE;          // on the glass
static_assert(MELT_ZEYE > BOX_ZEYE, "the melt bed must sit behind the menu glass");

// ---- the melt's parameter law ----------------------------------------------
// Outward expansion from the screen's centre (the web), braided toward
// MELT_LOBES slowly-drifting compass sectors so the trails LEAVE toward a
// handful of points around the periphery instead of spreading as a plain
// radial bloom. `t` is the eased ramp.
//
// THE OUTWARD RATE RAMPS WITH t, and that is the load-bearing part: the zoom
// carries ~82% of the melt's per-pass displacement, so ramping only the swirl
// and the brightness (the first cut) left the streak SPEED at full strength on
// the frame the pause opened -- already-melted-and-fading-in, never melting.
// feedbackSafeZoom floors |zoom-1| at FB_ZOOM_EPS, so small t parks the loop
// on the identity guard by construction: no melt yet, then it grows.
constexpr int   MELT_LOBES    = 5;         // exits around the screen
constexpr float MELT_LOBE_HZ  = 0.02f;     // turns/s the exits drift
constexpr float MELT_SWIRL_K  = 3.2f;      // x FB_SWIRL_PER_PASS: braid strength
constexpr float MELT_ZOOM     = 0.038f;    // per-pass outward rate at t = 1
constexpr float MELT_ZOOM_BR  = 0.008f;    // its breath, +-
constexpr float MELT_ZOOM_MS  = 9000.0f;   // the breath's period
constexpr float MELT_ROT      = 0.004f;    // rad/pass sway at t = 1
constexpr float MELT_ROT_MS   = 14000.0f;
constexpr float MELT_WOB_HZ   = railgeom::FB_WOBBLE_HZ;   // turns/s, the house unit

// The melt radiates from the middle of the screen -- the web -- rather than
// the bonus round's tunnel anchor (FB_FIX_V 0.44), so the stationary point
// tracks the thing the mark-up says to keep in focus.
constexpr float MELT_FIX_U = 0.5f, MELT_FIX_V = 0.5f;

// The mesh's REFERENCE VIEWPORT. Only the RATIO reaches the melt math (the
// swirl's radial falloff is scale-invariant), and it must be the same ratio on
// both targets or the swirl is a different shape -- so it is the 3DS panel's,
// shared, rather than each backend's own framebuffer. The PC passing its window
// dims also made the melt change shape with the window's aspect.
constexpr int MELT_VP_W = 400, MELT_VP_H = 240;

// THE WRAP IS EXACT AND IS DONE IN INTEGERS (game/phase.h).
//
// This helper used to read:
//     const float k = (float)timeMs / periodMs;
//     return (k - (float)(int)k) * 6.2831853f;
// which wrapped, but wrapped a value that had ALREADY lost precision: a
// wall-clock millisecond count stops round-tripping through `float` at 2^24 ms
// = 4.7 hours, and past that consecutive milliseconds collapse onto the same
// float. On a handheld nobody notices. On the ARCADE CABINET, which is left
// running for weeks, the melt's wobble and swirl quantise into visible steps —
// measured: over 1000 consecutive milliseconds the old path produced 58
// distinct phase values at 1.6 days of uptime and NINE at 14 days, where a
// smooth ramp is 1000. It reads as broken frame pacing.
//
// `phase::at` is modular integer arithmetic, so the wrap costs nothing, the
// precision is one part in 2^32 of a turn forever, and it survives the 24.9-day
// signed-millisecond rollover that would otherwise send `timeMs` negative.
//
// The period is a compile-time constant at every call site below, so `perMs`
// folds away and this is a multiply and a shift.
inline float pauseWrapSin(int timeMs, ts::phase::Rate rate) {
    return ts::phase::sinTurns(ts::phase::at((uint32_t)timeMs, rate));
}

inline void pauseMeltParams(int timeMs, float t, float& zoom, float& rot,
                            float& wobPhase, float& distort, float& lobePhase) {
    const float zFull = MELT_ZOOM + MELT_ZOOM_BR * pauseWrapSin(timeMs, ts::phase::fromPeriodMs(MELT_ZOOM_MS));
    zoom      = 1.0f + zFull * t;
    rot       = MELT_ROT * t * pauseWrapSin(timeMs, ts::phase::fromPeriodMs(MELT_ROT_MS));
    // MELT_WOB_HZ is TURNS PER SECOND, the unit every other chamber uses
    // (FB_WOBBLE_HZ). Authored per-millisecond once and handed to this
    // millisecond-period helper, it gave a 47-minute period -- the travelling
    // wobble was a frozen static distortion on both targets.
    // These two are handed onward as RADIANS (the mesh builder takes an angle),
    // so they convert back out of turns here. The conversion is exact: the
    // turns value is already inside one period, so multiplying by 2*PI cannot
    // lose the magnitude the old path lost.
    wobPhase  = ts::phase::radOf(ts::phase::at((uint32_t)timeMs, ts::phase::fromHz(MELT_WOB_HZ)));
    distort   = t;
    lobePhase = ts::phase::radOf(ts::phase::at((uint32_t)timeMs, ts::phase::fromHz(MELT_LOBE_HZ)));
}

// ---- the composite MASK — "keeping the web in focus" ------------------------
// The chamber is composited as ADDITIVE light, and a feedback loop is
// brightest AT its fixed point (the per-pass slide goes to zero there, so the
// steady state runs to INJECT/(1-DECAY)). The fixed point is the middle of the
// screen, which is where the web is: composited as a plain full-box quad the
// bed would be 6-10x brighter over the web than at the edges -- the exact
// inverse of what was asked for.
//
// So the composite is a RING, not a quad: gain 0 inside KEEP_R, full by
// OPEN_R, and full out to the corners. The gain already rides in vertex
// colour and both backends' fragment path multiplies by it, so the mask is
// FREE -- no shader, no pipeline, and the PICA's TEV does the same modulate.
// Radii are in UI units (the box is 1.0 tall), measured from the middle.
constexpr float MELT_KEEP_R = 0.22f;   // the clear pool the web sits in
constexpr float MELT_OPEN_R = 0.52f;   // fully open by here (a long, soft ramp)
constexpr int   MELT_MASK_SEG = 24;    // ring resolution

// The mask as a FUNCTION of a UI point, so anything that shows the chamber
// obeys the same keep-out. The frost box's haze needs this as much as the
// composite does: the box sits over the pool, and a quad that samples the
// chamber there reads the loop's saturated fixed point straight through the
// hole the mask just cut -- which is exactly how the first 3DS cut turned the
// glass into a flat grey slab while the screen around it stayed correct.
inline float pauseMaskGain(float x, float y) {
    const float dx = x - railgeom::UI_W * MELT_FIX_U, dy = y - MELT_FIX_V;
    const float d = std::sqrt(dx * dx + dy * dy);
    if (d <= MELT_KEEP_R) return 0.0f;
    if (d >= MELT_OPEN_R) return 1.0f;
    const float u = (d - MELT_KEEP_R) / (MELT_OPEN_R - MELT_KEEP_R);
    return u * u * (3.0f - 2.0f * u);   // smoothstep, like every other ramp here
}

// The glass's haze grid: the box as GRID x GRID quads so the mask above can
// ride its vertices. 4 is enough -- the chamber it samples is a blur.
constexpr int MELT_HAZE_GRID  = 4;
constexpr int MELT_HAZE_VERTS = MELT_HAZE_GRID * MELT_HAZE_GRID * 6;
// Vertices: (SEG) quads across the ramp + (SEG) quads from OPEN_R out to a
// radius that covers the corners, 6 verts each.
constexpr int   MELT_MASK_VERTS = MELT_MASK_SEG * 12;

struct MeltMaskVert { float x, y, u, v, gain; };

// Build the masked composite ring in UI space. `gain` is the composite's gain
// at full open; `uv` is the chamber's own identity mapping (y flipped), which
// is what both backends' full-box composite already uses. ONE builder, so the
// two targets cannot disagree about where the pool is.
inline int pauseMeltMaskBuild(MeltMaskVert* out, float gain) {
    const float cx = railgeom::UI_W * MELT_FIX_U, cy = MELT_FIX_V;
    // Far enough to cover a corner of the 1.3333 x 1.0 box from the middle.
    // The screen's half-diagonal from the fixed point is
    // hypot(UI_W/2, 0.5) = 0.833; a hair past it covers the corners and no
    // more. PICA200 has NO GUARD BAND and clips against all six planes, so
    // every triangle beyond this is setup and clip cost for nothing.
    const float farR = 0.88f;
    int n = 0;
    auto put = [&](float ang, float rad, float g) {
        const float x = cx + rad * ts::fastCos(ang);
        const float y = cy + rad * ts::fastSin(ang);
        MeltMaskVert& o = out[n++];
        o.x = x; o.y = y; o.gain = g;
        // The chamber's identity mapping over the whole UI box, y flipped.
        o.u = x / railgeom::UI_W;
        o.v = 1.0f - y;
    };
    const float dA = 6.2831853f / (float)MELT_MASK_SEG;
    for (int band = 0; band < 2; ++band) {
        const float r0 = band == 0 ? MELT_KEEP_R : MELT_OPEN_R;
        const float r1 = band == 0 ? MELT_OPEN_R : farR;
        const float g0 = band == 0 ? 0.0f : gain;
        const float g1 = gain;
        for (int i = 0; i < MELT_MASK_SEG; ++i) {
            const float a0 = (float)i * dA, a1 = (float)(i + 1) * dA;
            put(a0, r0, g0); put(a1, r0, g0); put(a1, r1, g1);
            put(a0, r0, g0); put(a1, r1, g1); put(a0, r1, g1);
        }
        (void)g0; (void)g1;
    }
    return n;
}

// Build the frost box's haze as a masked grid: the same chamber light the
// composite shows, through the same keep-out, so the glass is frosted where
// the melt is behind it and clear where the web is. Positions in UI space;
// `gain` is BOX_HAZE_GAIN x the ramp. The box's own measured half-extents go
// in with it, so the haze can never be sized against a different box than the
// pane and the border were drawn with.
inline int pauseHazeGridBuild(MeltMaskVert* out, float gain, float boxHW, float boxHH) {
    const float x0 = BOX_CX - boxHW, y0 = BOX_CY - boxHH;
    const float dx = (2.0f * boxHW) / (float)MELT_HAZE_GRID;
    const float dy = (2.0f * boxHH) / (float)MELT_HAZE_GRID;
    int n = 0;
    auto put = [&](float x, float y) {
        MeltMaskVert& o = out[n++];
        o.x = x; o.y = y; o.gain = gain * pauseMaskGain(x, y);
        o.u = x / railgeom::UI_W;
        o.v = 1.0f - y;
    };
    for (int i = 0; i < MELT_HAZE_GRID; ++i)
        for (int j = 0; j < MELT_HAZE_GRID; ++j) {
            const float ax = x0 + (float)j * dx, bx = ax + dx;
            const float ay = y0 + (float)i * dy, by = ay + dy;
            put(ax, ay); put(bx, ay); put(bx, by);
            put(ax, ay); put(bx, by); put(ax, by);
        }
    return n;
}

} // namespace ts
