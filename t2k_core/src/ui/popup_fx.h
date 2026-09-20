#pragma once
// ============================================================================
// popup_fx.h — the BOTTOM-SCREEN MESSAGE, as particles. Shared by both backends.
//
// ORIGINAL DESIGN, AND THAT IS THE POINT (user, 2026-09-02: "I want the
// particle text but I want an original take and design on the particle text").
// This project began as a port of someone else's game; he gave permission but
// wants no credit and no involvement, so anything that copies his identity is
// being deliberately changed. The popup was the last thing that still read as
// his. See the two designs side by side:
//
//   THE OLD ONE (his):  every glyph stroke RASTER-FILLED with a 2D lattice of
//                       dots, five time-lagged copies of the whole string
//                       stacked on one slow Lissajous drift. Solid dotted
//                       letters that wobble, and because the lagged copies
//                       trail the drift, they smear sideways -- the "sliding
//                       in from the left" read.
//
//   THIS ONE:           particles strung ALONG the stroke PATHS at even arc
//                       length. The font is line segments, so the letters are
//                       drawn as RUNS OF LIGHT POINTS following the pen, never
//                       filled. No lattice, no stacked copies, no drift.
//                       One word, made of moving points.
//
// THE MOTION is the user's own mark-up: a ring round the message with arrows
// pointing OUT of it.
//
//   CONVERGE  every particle flies in from its own bearing and radius, from
//             ALL directions at once, and lands on its point of a stroke.
//             Deliberately not sequential -- anything left-to-right is the
//             read we are getting away from.
//   HOLD      on the strokes, breathing. The word is simply legible.
//   BURST     thrown outward along those same bearings, growing and fading.
//             The arrows, literally, and it bookends the converge.
//
// AFFORDABLE ON AN OG 3DS, which is the constraint that killed the old one.
// config_apply.h records the lattice measured at 15.53 ms average / 35.80 ms
// worst against a 47.68 ms frame -- 33% of a frame already CPU-bound by 3.9x.
// But that was the CONSTRUCTION, not the particles: it re-derived every stroke
// every frame (a sqrt and two divides each), filled each with an 11x5 grid,
// and drew five overlapping layers, for 2445-6521 quads. This walks the same
// glyphs ONCE per frame and emits a BUDGETED count -- see BUDGET below -- with
// no per-segment sqrt and no layers. Roughly a tenth of the work, and the
// budget does not grow with anything the player can lengthen.
//
// RESOLUTION-INDEPENDENT BY CONSTRUCTION, AND THAT IS WHAT MAKES VR FREE
// (user: "I want this to scale proportionally so when I go to VR, theres
// nothing that needs to be changed.. it will just work"). Verified, not
// assumed:
//
//   * every position here is in NORMALISED UI SPACE (the 1.3333 x 1.0 box).
//     There is not a pixel literal in this file, and the fit factor is derived
//     from character counts and UI-space constants alone.
//   * DOT_SIZE_REF is in UI units on BOTH backends. The 3DS's renderGlowLine
//     sets g_thickBoost = 1.0 and g_capExt = halfWidth precisely so "the
//     caller asked for this width in UI units and gets it"; the PC's
//     pushUiCapsule multiplies by UI_REF_PX (240), the same 240-line reference
//     convention every other stroke width in the renderer uses.
//
// So 400x240, 1080p, the 4K arcade target and a per-eye XR view all get the
// same layout for free.
//
// THE ARC SURVIVES XR SPECIFICALLY because its depth is FAKED with a 2D scale
// factor (`persp`) rather than a real z. uiPlaneMatrix() puts every UI vertex
// on ONE seat-locked plane and DISCARDS z (m[2][2] = 0), so an arc built from
// real depth would flatten to nothing there -- silently, on the one target
// nobody can currently test. Built from scale, it reads identically on the
// plane. DO NOT "improve" the arc by giving it a true z.
//
// Seam-only: no GPU type, no engine include. Each backend just calls
// font.h renderDot for what it hands back.
// ============================================================================

#include <cmath>

#include "../rendering/font.h"
#include "../rendering/web_palette.h"
#include "../game/constants.h"
#include "../game/math_lut.h"

namespace ts {
namespace popupfx {

// ---- budget --------------------------------------------------------------
// The cap exists so cost cannot scale with message length: a long message gets
// sparser strokes rather than a bigger bill, exactly as the level bake widens
// its column pitch for long strings.
//
// IT WAS 384, AND THAT WAS THINNING ORDINARY MESSAGES. Measured natural counts
// at the old STEP_GLYPH of 0.235, before any cap:
//
//     "grab item!"             262      "zappa recharge"          402
//     "powershot!"             302      "jumpin'enabled"          410
//     "2 more 4 warp"          333      "super zapper recharge!"  618
//     "avoid spikes!"          352
//
// So anything from "zappa recharge" up was being culled by the budget, not
// drawn as designed -- which is why the word read light on the panel. The cap
// was the wrong size, not the dots.
//
// DOUBLED, and the headroom is not close. At half the step the worst message
// is 1223 dots, which is still HALF the cheapest case of the raster lattice
// this replaced (2445-6521 quads) and a fifth of its worst -- and without that
// lattice's per-segment sqrt and two divides EVERY frame, or its five
// time-lagged copies. config_apply.h has the OG measurement that killed it.
constexpr int MAX_DOTS = 1408;

// The popup's life. SHORTENED FROM 4000 (user: "It shouldnt be hanging there
// for a couple of seconds.. those are quick notifications that should dissolve
// in, show the vector then immediately dissolve out"). Nothing else depends on
// the old figure -- the backends used to expire the slot on a literal 4000 and
// now both ask slotLive(), so this constant is the single owner of the
// duration. The TRIGGER POINT is untouched; only how long the notification
// stays up.
constexpr int LIFE_MS = 2800;

// Phase boundaries as fractions of LIFE_MS. HOLD is the longest on purpose:
// the message exists to be READ, and the two moving phases are punctuation.
constexpr float T_CONVERGE = 0.28f;
constexpr float T_HOLD_END = 0.56f;

// ---- THE FRAME BUDGET, and why it is not the same number -------------------
// A dot is NOT one quad on the 3DS. It is an indexed FAN -- centre + 5 rim --
// so it costs font.h's UI_VERTS_PER_STROKE (6) 36-byte vertices, the same as a
// stroke; text_c3d.cpp spells it FAN_VERTS and renderDot writes exactly that
// many or none. The text batch is MAX_VERTS 24000, so it holds ~4000 fans, and
// the batch TRUNCATES SILENTLY past that: no assert, no log, just a message cut
// off mid-word.
//
// THIS ALREADY BIT, AT TWELVE VERTS PER DOT. renderDot used to reach the same
// diamond via renderGlowLine(x,y,x,y) -> segment()'s degenerate branch, writing
// the centre four times and each rim point twice. Raising MAX_DOTS from 384 to
// 1408 made a single message safe (1408 x 12 = 16,896) but TWO live messages
// requested 2816 dots = 33,792 verts and the second was truncated with no
// warning. Two live slots is ordinary, not exotic: show_powerup_text
// round-robins two slots and each lives the full LIFE_MS, so any two pickups
// inside four seconds does it.
//
// AT SIX IT NO LONGER OVERRUNS THAT WAY -- but the sharing STAYS. The indexed
// fan halved the dot and cut the stroke from 15, so even two uncapped slots are
// 2816 x 6 = 16,896 + ~918 of score and flash = 17,814, inside 24,000. The cap
// is therefore holding a wider margin than it was sized for, which is the right
// direction to be wrong in: the batch is still SHARED and still silent when it
// fills, so both backends must keep asking slotCap() rather than passing
// MAX_DOTS blind.
// 1600, not 2000. The 24000-vertex batch is not the popup's alone: on the 3DS
// the SCORE and the popup's own vector flash are in it too. Worst realistic
// frame, two live messages:
//
//     score        ~7 chars x 3 segs x 6 verts         =    126
//     two flashes   2 x 22 chars x 3 segs x 6 verts    =    792
//     two messages  2 x 800 dots x 6 verts             =  9,600
//                                                        ------
//                                                         10,518   (cap 24,000)
//
// A single message is 1408 x 6 + 126 + 396 = 8,970.
constexpr int DOTS_PER_FRAME = 1600;

// Is a slot of this age still showing?
inline bool slotLive(int ageMs) { return ageMs >= 0 && ageMs < LIFE_MS; }

// The cap ONE message may use, given how many are live this frame. With one
// message it gets the full density; with two they share, and BOTH thin
// equally. Sharing is the right failure: a pair of slightly sparser words is a
// far better outcome than one full word and one chopped in half.
inline int slotCap(int liveSlots) {
    if (liveSlots <= 1) return MAX_DOTS;
    const int per = DOTS_PER_FRAME / liveSlots;
    return per < MAX_DOTS ? per : MAX_DOTS;
}


// ---- layout ---------------------------------------------------------------
// Glyphs are line segments in a 2 x 2 box; characters advance by ADVANCE glyph
// units (writeAfont's own law at THICKNESS, so the particle word is laid out
// exactly like a drawn one would be).
constexpr float THICKNESS = 0.22f;
constexpr float ADVANCE   = 2.0f * (THICKNESS + 1.3f);   // 3.04 glyph units

// ---- ONE SCALE FOR THE WHOLE CLASS ----------------------------------------
// Glyph unit -> UI unit, at the AUTHORED size. These are the reference values;
// what actually gets used is scaleX()/scaleY() below.
constexpr float SCALE_X_REF = 0.0250f;
constexpr float SCALE_Y_REF = 0.0410f;

// The 1.3333-wide UI box, and the clear space kept at each end.
constexpr float BOX_W  = 1.3333f;
constexpr float MARGIN = 0.045f;

// THE WIDEST MESSAGE DECIDES THE SIZE OF EVERY MESSAGE (user request: "the
// longest text needs to resize all of the text proportionally for that class
// of text to fit in the screen... it needs to be proportional for all text
// because it needs to look symetric for that class of text").
//
// "superzapper recharge" is 20 characters and ran off both ends at the
// authored size: 20 x 3.04 x 0.025 = 1.52 UI in a 1.3333 box. The fix is NOT
// to shrink that one message -- per-message fitting would give every message a
// different size and the class would look ragged as they came and went. One
// factor is derived from the longest and applied to ALL of them, so they stay
// a set.
//
// It is a SHRINK-ONLY factor, clamped at 1: if the table's longest message
// already fits, nothing moves. And it is derived from the table at runtime
// (constants.h longestPowerupText), so a message added later re-fits the class
// automatically rather than overflowing silently.
inline float textScale() {
    static const float k = [] {
        const float widest = (float)ts::longestPowerupText() * ADVANCE * SCALE_X_REF;
        const float avail  = BOX_W - 2.0f * MARGIN;
        const float fit    = (widest > 0.0f) ? (avail / widest) : 1.0f;
        return fit < 1.0f ? fit : 1.0f;
    }();
    return k;
}
inline float scaleX() { return SCALE_X_REF * textScale(); }
inline float scaleY() { return SCALE_Y_REF * textScale(); }

// Where the word sits. IT IS A NOTIFICATION, NOT A CAPTION -- the user is
// emphatic about this: "I want this far down on the screen so its a
// notification, not something that gets in the way, otherwise it will be
// annoying to the player." Anything that has to be read over the play area
// costs the player a life eventually. Glyphs run 0..2 in y, so the word
// occupies Y_BASE .. Y_BASE + 2*SCALE_Y -- the bottom sixth of the screen.
// If this ever creeps upward, that is the bug.
constexpr float X_CENTER = 0.6666f;
constexpr float Y_BASE   = 0.090f;

// ---- particle look --------------------------------------------------------
constexpr float DOT_SIZE_REF = 0.0062f;   // UI units at rest (trimmed with the
                                        // doubled density: at 0.0072 the closer
                                        // spacing reads as a solid stroke again,
                                        // which is the fill look being avoided)
constexpr float DOT_JITTER = 0.35f;     // +-fraction of size, hashed per dot
// Arc-length spacing along a stroke, in GLYPH units. HALVED from 0.235 with
// the budget above: at 0.235 the points read as a loose dotted line at 240
// lines, and the messages that wanted more than 384 dots were being thinned on
// top of that. This is the density knob -- lower is denser.
constexpr float STEP_GLYPH = 0.1175f;

// ---- motion: A 3D ARC, not a flat dissolve --------------------------------
// User request: "have that still dissolve in and out but also slide up from
// the bottom when it dissolves in and then slide down off screen as it
// dissolves out... have the dissolve start away from the camera, move toward
// the camera and then fade downward. like an arc effect so it gives a really
// solid 3D look."
//
// So the word travels a path THROUGH DEPTH, not across the screen:
//
//   IN    deep and below the frame -> rises AND approaches -> lands on the
//         read plane at full size.
//   HOLD  on the plane, still, legible.
//   OUT   falls away downward, RECEDING as it goes, dissolving.
//
// Depth is faked the honest way, with one perspective factor k = 1/depth
// applied to BOTH the glyph offsets and the dot size. Everything scales about
// the word's own centre, so a distant word is a small tight cluster and a near
// one is full width -- which is what sells the approach as depth rather than
// as a zoom. It costs one divide per frame, not per dot.
//
// The dissolve is unchanged and runs ON TOP: particles still scatter to their
// own bearings at both ends, so the word comes apart and together rather than
// sliding as a rigid block.
constexpr float DEPTH_FAR = 2.70f;  // entry depth; 1.0 IS the read plane
constexpr float DEPTH_OUT = 1.55f;  // exit depth -- it recedes as it falls
constexpr float SLIDE_IN  = 0.150f; // UI units below the read line at entry
constexpr float SLIDE_OUT = 0.240f; // UI units below at exit (clear of frame)

constexpr float R_IN     = 0.34f;   // converge scatter (smaller now: the slide
constexpr float R_OUT    = 0.30f;   // and the depth carry most of the motion)
constexpr float SPIN_IN  = 3.4f;    // radians of swirl bled off while arriving
constexpr float SHIMMER  = 0.0016f; // hold-phase wander, UI units

// Colour: the level band's live sweep, lifted toward white so it separates
// from the tube it sits under. Intensity, not hue, is what does the separating.
constexpr float VALUE     = 1.0f;
constexpr float WHITE_MIX = 0.28f;

// ---- THE VECTOR FLASH -----------------------------------------------------
// User request: "have the text become vector text for a split second before
// shattering in to particles again... quickly fade in vector lines in that
// font right at the peak of the particles."
//
// It works because the particles are ON the stroke paths: a writeAfont stroke
// in the same font, at the same layout, traces straight through the run of
// dots that forms it. So it does not read as two overlaid things -- it reads
// as the dots IGNITING INTO LINES and then letting go again. That alignment is
// not a coincidence to be grateful for, it is the reason this design can do
// this and the raster-lattice one could not.
//
// The flash lives inside the HOLD, at the moment the cloud is fully assembled
// -- which is what "the peak of the particles" means.
//
// A QUARTER OF THE PEAK IT HAD (user: "lets cut the peak down to 1/4th of what
// it is now. It shouldnt be hanging there for a couple of seconds"). The lit
// plateau went 800 ms -> 200 ms; the rise and fall are unchanged, because they
// are what makes it read as igniting rather than switching on.
//
// Fractions of LIFE_MS (2800), so at the shipped life:
//
//     0.280 =  784 ms   the dots land, strokes begin to ignite
//     0.330 =  924 ms   fully lit          (140 ms rise)
//     0.404 = 1131 ms   begin to let go    (207 ms held lit)
//     0.504 = 1411 ms   gone               (280 ms fall)
//     0.560 = 1568 ms   the burst
//
// So the whole notification is: dissolve in, show the word, dissolve out --
// with a ~157 ms gap between the strokes going and the cloud coming apart.
// That gap is deliberate and small: without it the strokes would still be
// fading as the burst began and the two would read as one muddled event, but
// any longer and the particles visibly HANG, which is what this retune is for.
constexpr float V_IN_BEG  = T_CONVERGE;          // starts as they land
constexpr float V_IN_END  = T_CONVERGE + 0.050f; // 140 ms up
constexpr float V_OUT_BEG = T_CONVERGE + 0.124f; // 207 ms fully lit
constexpr float V_OUT_END = T_CONVERGE + 0.224f; // 280 ms down
static_assert(V_OUT_END < T_HOLD_END,
              "the vector flash must finish before the burst begins, or the "
              "strokes fade while the cloud is already coming apart");
constexpr float V_ALPHA   = 0.85f;               // peak stroke alpha

// writeAfont scales glyph units by sx*0.5 / sy*0.5, so matching the particle
// layout means handing it twice SCALE_X / SCALE_Y. Kept as an expression of
// those constants rather than as its own numbers, so the strokes cannot drift
// off the dots when the layout is retuned.
inline float vSx() { return scaleX() * 2.0f; }
inline float vSy() { return scaleY() * 2.0f; }

struct Dot {
    float x, y, size;
    float r, g, b, a;
};

inline float clamp01f(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
inline float ease(float t) { return t * t * (3.0f - 2.0f * t); }

// Cheap integer hash -> [0,1). Every per-particle choice comes from this, so
// both backends and both stereo eyes generate an identical cloud.
inline float phash(unsigned h) {
    h ^= h >> 16; h *= 0x7feb352du;
    h ^= h >> 15; h *= 0x846ca68bu;
    h ^= h >> 16;
    return (float)(h & 0xffffffu) * (1.0f / 16777216.0f);
}

// Total stroke arc length of `text` in glyph units — used once, to derive the
// spacing that keeps the dot count inside MAX_DOTS.
inline float strokeLength(const char* text, int nchars) {
    float total = 0.0f;
    for (int ci = 0; ci < nchars; ++ci) {
        const int fi = charToIndex(text[ci]);
        if (fi < 0 || fi >= FONT_CHAR_COUNT) continue;
        for (int sg = 0; sg < FONT_SEG_COUNT[fi]; ++sg) {
            const FontSegment& s = FONT_DATA[fi][sg];
            if (s.x1 == -1) break;
            const float dx = (float)(s.x2 - s.x1), dy = (float)(s.y2 - s.y1);
            // std::sqrt, not the trig LUT: this is ~35 calls per frame for a
            // whole message (once per glyph SEGMENT, never per particle), which
            // is nothing next to the per-segment sqrt AND two divides the old
            // lattice paid on every one of its thousands of dots.
            total += std::sqrt(dx * dx + dy * dy);
        }
    }
    return total;
}

// Build this frame's dots. Returns the count written; 0 once the message is
// over. `seed` (the message's start time) varies bearings per message so two
// popups in a row do not fly in along the same lines.
inline int build(Dot* out, int cap, const char* text, int nchars,
                 int ageMs, unsigned seed, int level, int timeMs) {
    if (cap <= 0 || nchars <= 0 || ageMs < 0 || ageMs >= LIFE_MS) return 0;
    const float t = (float)ageMs / (float)LIFE_MS;

    // ---- colour, once ------------------------------------------------------
    // Off the GLOBAL clock, not the message's age, so successive popups
    // continue the level's drift instead of each restarting on one hue.
    const float webT = 0.5f + 0.5f * ts::fastSin((float)timeMs * 0.001f
                                     / ts::WEB_SWEEP_PERIOD_S * 6.2831853f);
    float cr, cg, cb;
    ts::webLevelColor(level, webT, cr, cg, cb, VALUE);
    cr += (1.0f - cr) * WHITE_MIX;
    cg += (1.0f - cg) * WHITE_MIX;
    cb += (1.0f - cb) * WHITE_MIX;

    // ---- phase -------------------------------------------------------------
    // `p` is how far from home the cloud is (1 at both extremes, 0 through the
    // hold); `outward` says which side of home.
    float p = 0.0f, fade = 1.0f;
    bool outward = false;
    if (t < T_CONVERGE) {
        const float b = t / T_CONVERGE;
        p = 1.0f - ease(b);
        fade = ease(b);
    } else if (t >= T_HOLD_END) {
        const float b = (t - T_HOLD_END) / (1.0f - T_HOLD_END);
        p = ease(b);
        outward = true;
        fade = 1.0f - b * b;
    }
    const float radius  = p * (outward ? R_OUT : R_IN);
    const float shim    = (p == 0.0f) ? SHIMMER : 0.0f;

    // ---- the arc: depth and the vertical slide -----------------------------
    // `p` is already 1 at both extremes and 0 through the hold, so depth and
    // slide ride it directly -- one curve, three behaviours, and the arc
    // cannot desync from the dissolve because they share the parameter.
    float depth = 1.0f, slide = 0.0f;
    if (!outward) {
        depth = 1.0f + (DEPTH_FAR - 1.0f) * p;
        slide = -SLIDE_IN * p;                       // rises INTO place
    } else {
        depth = 1.0f + (DEPTH_OUT - 1.0f) * p;
        // Accelerating fall: gravity reads as depth, a linear drop reads as a
        // wipe. Squaring is the whole difference between "it fell away" and
        // "it slid off".
        slide = -SLIDE_OUT * p * p;
    }
    // NAMED `persp`, NOT `k`: the arc-length step loop below counts with `k`,
    // and when this was called `k` too it SHADOWED this value silently -- every
    // glyph coordinate and every dot size got multiplied by the loop index
    // instead of the perspective factor. The word spanned x -6.5..5.4 in a
    // 1.3333-wide box with dots up to 0.14 UI across. It built clean and the
    // only symptom was giant scattered blobs.
    const float persp = 1.0f / depth;   // one divide, per frame not per dot

    // ---- spacing: the budget, not the string, decides density --------------
    const float len = strokeLength(text, nchars);
    float step = STEP_GLYPH;
    if (len > 0.0f) {
        const float want = len / (float)(MAX_DOTS < cap ? MAX_DOTS : cap);
        if (want > step) step = want;      // long messages get sparser, never denser
    }

    // Centre the word: total advance width, halved, in glyph units.
    const float halfW = (float)nchars * ADVANCE * 0.5f;
    const float sx = scaleX(), sy = scaleY();

    int n = 0;
    unsigned idx = 0;
    for (int ci = 0; ci < nchars && n < cap; ++ci) {
        const int fi = charToIndex(text[ci]);
        if (fi < 0 || fi >= FONT_CHAR_COUNT) continue;
        const float ox = (float)ci * ADVANCE - halfW;
        for (int sg = 0; sg < FONT_SEG_COUNT[fi] && n < cap; ++sg) {
            const FontSegment& s = FONT_DATA[fi][sg];
            if (s.x1 == -1) break;
            const float ax = (float)s.x1 + ox, ay = (float)s.y1;
            const float dx = (float)(s.x2 - s.x1), dy = (float)(s.y2 - s.y1);
            const float l2 = dx * dx + dy * dy;
            if (l2 <= 0.0f) continue;
            const float slen = std::sqrt(l2);
            // Walk the stroke at even ARC LENGTH -- this is what makes the
            // letters runs of points along the pen path rather than a fill.
            const int steps = (int)(slen / step);
            for (int k = 0; k <= steps && n < cap; ++k) {
                const float u = (steps > 0) ? (float)k / (float)steps : 0.0f;
                // Glyph space -> UI space, centred on the word.
                float gx = ax + dx * u;
                float gy = ay + dy * u;
                // Glyph -> UI, scaled about the word's own centre by the
                // perspective factor, then slid vertically along the arc.
                float x = X_CENTER + gx * sx * persp;
                float y = Y_BASE   + gy * sy * persp + slide;

                const unsigned h = idx * 2654435761u ^ seed;
                ++idx;
                const float hA = phash(h);
                const float hR = phash(h ^ 0x9e3779b9u);
                const float hS = phash(h ^ 0x85ebca6bu);

                if (p > 0.0f) {
                    // Own bearing and radius, so the cloud opens and closes as
                    // a spray rather than a rigid ring. The swirl term bleeds
                    // off as it lands, which is what keeps the arrival from
                    // reading as a straight-line slide.
                    const float ang = hA * 6.2831853f + (outward ? 0.0f : p * SPIN_IN);
                    const float rr  = radius * (0.45f + 0.55f * hR);
                    // The scatter is in the word's own plane, so it shrinks
                    // with distance like everything else.
                    x += ts::fastCos(ang) * rr * persp;
                    // Flattened: the word is wide and short, so a circular
                    // spray would throw the top and bottom dots far clear of it
                    // while the side dots barely moved.
                    y += ts::fastSin(ang) * rr * 0.45f * persp;
                }
                if (shim > 0.0f) {
                    const float ph = (float)ageMs * 0.004f + hA * 6.2831853f;
                    x += ts::fastSin(ph) * shim;
                    y += ts::fastCos(ph * 1.3f) * shim;
                }

                Dot& d = out[n++];
                d.x = x; d.y = y;
                // Size carries the depth: a far dot is a small dot. This is
                // most of what makes the approach read as 3D.
                d.size = DOT_SIZE_REF * textScale() * persp
                       * (1.0f + (hS - 0.5f) * 2.0f * DOT_JITTER);
                d.r = cr; d.g = cg; d.b = cb;
                // Stagger the fade per particle so the word does not switch on
                // and off as one block at either end.
                d.a = clamp01f(fade * (0.55f + 0.45f * hS)) ;
            }
        }
    }
    return n;
}

// The vector flash's alpha for this age, and the transform that puts its
// strokes exactly on the particles. Returns 0 when the flash is not showing.
// `outSx`/`outSy`/`outY` are only written when it is.
inline float vectorFlash(int ageMs, float& outSx, float& outSy, float& outY) {
    if (ageMs < 0 || ageMs >= LIFE_MS) return 0.0f;
    const float t = (float)ageMs / (float)LIFE_MS;
    if (t <= V_IN_BEG || t >= V_OUT_END) return 0.0f;

    float a;
    if (t < V_IN_END)        a = ease((t - V_IN_BEG) / (V_IN_END - V_IN_BEG));
    else if (t < V_OUT_BEG)  a = 1.0f;
    else                     a = 1.0f - ease((t - V_OUT_BEG) / (V_OUT_END - V_OUT_BEG));
    a *= V_ALPHA;
    if (a <= 0.004f) return 0.0f;

    // The flash only ever happens during the HOLD, where the arc is at the read
    // plane -- depth 1, no slide. Reading them from the same constants anyway,
    // so a retune of the phase boundaries cannot leave the strokes behind.
    outSx = vSx();
    outSy = vSy();
    outY  = Y_BASE;
    return a;
}

} // namespace popupfx
} // namespace ts
