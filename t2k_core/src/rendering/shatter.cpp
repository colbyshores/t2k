// Pixel-shatter celebration text — simulation. See shatter.h for the design
// provenance (see DOCTRINE.md) and the per-message style table.
//
// Closed-form animations, no integration state: every frame is a pure
// function of (age, per-dot hash), which is what keeps the GL oracle, the C3D
// backend and the two stereo eyes bit-agreeing without any cross-talk.

#include "shatter.h"

#include <cmath>
#include <cstring>

#include "font.h"
#include "../data/yes_data.h"  // STYLE_YES letterforms: the wordmark pipeline's, outline only
#include "web_palette.h"        // STYLE_YES shades its chant from the live web
#include "../game/math_lut.h"

namespace ts {
namespace shatter {

namespace {

// Glyph metrics — the same constants as the popup stipple path so the
// letterforms are recognisably the game's own font.
constexpr float FSTR         = 0.375f;
constexpr float SKON         = 2.35f;
constexpr float CHAR_ADVANCE = SKON * (FSTR + 1.3f);   // 3.936 text units/char

// Raster lattice (the arcade reference's bitmap-scanline signature): fixed row pitch, column
// pitch widened for long strings so the bake stays inside MAX_DOTS.
constexpr float PITCH_Y = 0.175f;
constexpr float COVER_R = 0.36f;    // stroke coverage radius, glyph units

// Text-units -> R-units mapping (backend multiplies by the web's mean rim
// radius): the string spans SPAN_R of the radius each side at spread 1, and
// letters are ASPECT taller than their advance-derived width.
constexpr float SPAN_R = 0.95f;
constexpr float ASPECT = 1.55f;

// ---- STYLE_YES: the chant wears the WORDMARK's letterforms -----------------
// The same outline pipeline that draws GAME OVER
// (rendering/gameover_geometry.cpp) and DEMO (ui/demo_overlay.h):
// src/data/yes_data.h, tools/gen_wordmark.py --outline-only over the same
// font_data.cpp. The bake samples the OUTLINE contours, so the word is fat
// glowing vector lines with the play area visible straight through it, not
// the filled slab the scanline lattice makes (user, 2026-09-26: "I want that
// to be an outline like we had before. It should be similar to the GAME OVER
// screen and the DEMO text.").
//
// FOUR GLYPHS, WITH THE MARK. "YES!" is back (user, 2026-09-26: "add the '!'
// behind YES"). It was dropped on 2026-09-26 because the mark's dot read as a
// period -- and because the four-glyph bake LOOKED wrong. Both causes are now
// closed, and the second one was not the mark's fault:
//
//   The glyph is MISSING ITS BAR. The '!' is two segments, the (1,2)-(1,1)
//   stem and the (1,0)-(1,0) dot, and the stem's endpoint lands exactly on a
//   grid line at every whole-ADVANCE offset. The tracer collapsed that cell's
//   two iso crossings onto the same corner, emitted a zero-length edge, and
//   the chain walk closed a 1-point loop there -- so the DOT traced and the
//   STEM VANISHED. The chant was showing a bare period, which is exactly the
//   artefact that got the mark removed. Fixed in gen_logo.py's contours():
//   zero-length edges are dropped, not chained. Verified offset-dependent --
//   the identical field at x=0 traced both loops -- and non-destructive:
//   T2K, GAME, OVER, DEMO and A-Z all regenerate byte-identical.
//
//   THE "STRETCH" WAS SCALE, NOT SHAPE. The generator normalises total WIDTH
//   to 2.0, so adding the narrow '!' shrinks the whole block by
//   0.254468/0.308857 = 0.823. Per-GLYPH aspect is invariant under that
//   (measured: Y 0.9987 / E 1.0000 / S 1.0000 in BOTH the 3- and 4-glyph
//   bakes, and DEMO is 1.0000 too -- the same letterform, which is the whole
//   point of generating from the shipped font). YES_SIZE_FIX below cancels the
//   block-scale loss so the letters keep the size the 3-glyph bake had and
//   only the word gets wider.
//
// DENSITY. The step is sized so the bake lands near the dot count the lattice
// spent on the same word: same MAX_DOTS spend and the same MAX_WORLD_OUT
// headroom as before, but every dot now sits ON a stroke edge instead of
// inside one, so the same budget buys an outline instead of a fill.
//
// FATNESS. Dot half-extent = YES_OUTLINE_FAT * YES_THICK * dotStep * k. FAT
// is the geometric closure factor, DERIVED not eyeballed: at the shipped step
// the dot diameter has to exceed the step or the run reads as a beaded
// necklace instead of one continuous stroke. THICK is the user's knob -- half
// the first pass's thickness, and at 1.35 x step the dots still overlap, so
// it stays a line rather than a sprinkle.
//
constexpr float YES_OUTLINE_STEP = 0.0167f;        // wordmark units between dots
constexpr float YES_OUTLINE_FAT  = 1.35f;          // dot half-extent / step, for stroke closure
constexpr float YES_THICK        = 1.0f;           // half-thickness (user, 2026-09-27: comparison)
constexpr float YES_BRITE        = 1.0f;           // full brightness (user, 2026-09-26: 2x)
// V ADJUSTMENT. The module stretches v by ASPECT for every style (kY = k *
// ASPECT). The wordmark's own proportions are already correct, so the bake
// pre-divides y by ASPECT to land it undistorted -- that alone would make the
// chant's letters SQUARE, because the wordmark's Y/E/S really are 1:1.
//
// SQUARE IS NOT WHAT THE CHANT SHOULD BE. Measured against the lattice bake
// the rest of the particle text uses, a lattice glyph is 2.450 x 4.0687 world
// units -- W/H 0.6022, distinctly taller than wide. The chant's square
// letters read as squashed next to them (user, 2026-09-26: "It currently
// appears squished ... make them more proportional like the particle text at
// the bottom"). So the bake stretches y by 1/YES_LETTER_ASPECT on top of
// undoing ASPECT, and the chant's letters take the lattice's proportions.
//
// THE STRETCH IS BAKED, NOT APPLIED AT EVAL, AND THE DOT COUNT FOLLOWS IT.
// Spacing along a stroke is solved from the edge length; if the stretch were
// applied after that solve, every near-vertical run would be pulled 1.66x
// further apart than its dots were sized for and the stroke would break into
// a beaded necklace. So the edge length used for the count is the STRETCHED
// length, and the fill grid's y pitch is pre-shrunk by the same factor --
// both bakes lay their dots down in the space the sign is actually rendered
// in. Measured: stroke 624 -> 749 dots, sheet 271 -> 229 (coarser step),
// total 978 inside MAX_DOTS.
constexpr float YES_LETTER_ASPECT = 0.6022f;        // target W/H, measured off the lattice bake
constexpr float YES_V_STRETCH     = 1.0f / YES_LETTER_ASPECT;
constexpr float YES_V_ADJ         = YES_V_STRETCH / ASPECT;
// Cancels the width-normalisation loss from adding the '!' to the block:
// 0.308857 ("YES" Y_MAX) / 0.254468 ("YES!" Y_MAX) = 1.2137, so the three
// letters render at the size the 3-glyph bake had.
constexpr float YES_SIZE_FIX     = 0.308857f / 0.254468f;

// THE CENTRE. The outline bake leaves the letterforms hollow -- you see the
// web straight through them, which is the point while the word is ARRIVING.
// Once a sign has fully coalesced it is a bright ring with nothing in it, and
// the chant reads as wireframe (user, 2026-09-26: "once the YES! outline
// fully materializes lets immediately fade in a translucent center of the
// same color but less bright as the outline"). So the bake emits a second dot
// set: the INTERIOR of the same contours, on a coarser grid, which eval fades
// in the instant the coalesce closes and out again as the sign detonates.
//
// STEP is set from the measured ink area, not eyeballed: the four-glyph
// contours enclose 0.354 wordmark units^2, and the grid is laid down in the
// STRETCHED space (y pitch pre-shrunk by YES_V_STRETCH, so the effective
// pitch is STEP on both axes). 0.045 lands 229 dots inside the ink -- about
// 2.3x the overlap needed for a continuous sheet -- which keeps stroke +
// sheet at 978, inside MAX_DOTS.
//
// FAT is the sheet-closure factor (dot diameter / step). It is larger than
// the stroke's 1.35 because these are DIM additive dots: at the stroke's
// closure the falloff tails would not overlap enough to read as a surface,
// only as a dim mesh.
constexpr float YES_FILL_STEP  = 0.045f;
constexpr float YES_FILL_FAT   = 1.5f;
// "less bright as the outline": the stroke's alpha peaks at 0.85 x YES_BRITE,
// so the centre peaks at 0.35 -- about 40% of the stroke, clearly present but
// never competing with it.
constexpr float YES_FILL_BRITE = 0.35f;
// Fade-in window, measured from the end of the coalesce, not from the word.
// Short enough to read as "immediately", long enough that it does not pop.
constexpr int   YES_FILL_IN_MS = 200;

// Distance from the camera to the RIM plane (zn = 0), in tube lengths. The
// anchor this module is handed is SEAT-relative (camera_advance_norm is
// (cam_target.z - world_trans.z)/L, camera.cpp), so this constant is the only
// place the seat's own standoff enters. The seats are
// tscam::STANDOFF + VIEWS[cam_view].z (game/camera.h) = 5.25 / 7.125 / 9.00 /
// 9.00 lanes, and the default is cam_view = 1 (engine.h), i.e. 7.125/25 = 0.285.
//
// Used to convert an EYE-relative depth into the rim-relative zn this module
// emits, and to place the near-plane cull at the actual viewer rather than at
// the rim. 0.30 is the legacy value carried forward rather than retuned: it
// sits 0.015 tube lengths (0.375 world units) beyond the default seat, and the
// other two distances land at 0.21 and 0.36, so the worst case is 0.09 tube
// lengths (2.25 world units) at the nearest seat. That is a seat offset, not a
// visible break. The seat IS reachable now -- it is plain game state and
// shatter_frame.h already holds the engine -- so deriving it per frame is
// possible; it would mean threading the seat through evalWorld's signature for
// no visible gain, and is deliberately not done.
constexpr float CAM_AHEAD = 0.30f;

inline uint32_t splitmix32(uint32_t x) {
    x += 0x9E3779B9u;
    x ^= x >> 16; x *= 0x21F0AAADu;
    x ^= x >> 15; x *= 0x735A2D97u;
    x ^= x >> 15;
    return x;
}

inline float h01(uint32_t h, int shift) {
    return (float)((h >> shift) & 0xFFu) * (1.0f / 255.0f);
}

// hue (degrees, any range) -> fully saturated neon RGB.
inline void hueToRgb(float hue, float& r, float& g, float& b) {
    hue -= std::floor(hue * (1.0f / 360.0f)) * 360.0f;
    const float hp = hue * (1.0f / 60.0f);
    const int   i  = (int)hp;
    const float f  = hp - (float)i;
    const float q  = 1.0f - f;
    switch (i) {
        case 0:  r = 1; g = f; b = 0; break;
        case 1:  r = q; g = 1; b = 0; break;
        case 2:  r = 0; g = 1; b = f; break;
        case 3:  r = 0; g = q; b = 1; break;
        case 4:  r = f; g = 0; b = 1; break;
        default: r = 1; g = 0; b = q; break;
    }
}

inline uint32_t fnv1a(const char* s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

// Is text-space point (gx, gy) within COVER_R of any stroke of the string?
// NOTE: float coverage predicate defines the baked dot set; ULP shifts flip
// boundary membership and re-hash every later dot via slot.count/d.h.
/* @vfp-exempt R3 — glyph coverage predicate: float projection/clamps define which scanline dots exist; integerizing changes bake identity. measured n/a. Verified 2026-09-06. */
bool coveredAt(const char* text, int nchars, float gx, float gy) {
    const int cc = (int)(gx / CHAR_ADVANCE);
    for (int ci = cc - 1; ci <= cc + 1; ++ci) {
        if (ci < 0 || ci >= nchars) continue;
        const int fi = charToIndex(text[ci]);
        if (fi < 0 || fi >= FONT_CHAR_COUNT) continue;
        const float lx = gx - CHAR_ADVANCE * (float)ci;
        if (lx < -COVER_R || lx > 2.0f + COVER_R) continue;
        for (int sgi = 0; sgi < FONT_SEG_COUNT[fi]; ++sgi) {
            const FontSegment& sg = FONT_DATA[fi][sgi];
            if (sg.x1 == -1) break;
            const float ax = (float)sg.x1, ay = (float)sg.y1;
            const float dx = (float)sg.x2 - ax, dy = (float)sg.y2 - ay;
            const float lsq = dx * dx + dy * dy;
            float t = lsq > 0.0f ? ((lx - ax) * dx + (gy - ay) * dy) / lsq : 0.0f;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            const float ex = lx - (ax + dx * t), ey = gy - (ay + dy * t);
            if (ex * ex + ey * ey <= COVER_R * COVER_R) return true;
        }
    }
    return false;
}

// Bake STYLE_YES from the generated wordmark OUTLINE instead of the scanline
// lattice. Each contour is a closed ring; every edge gets an INTEGER count of
// dots at even arc length, so no float comparison drives how many dots exist
// (AGENTS.md T1/T2 -- the count is solved per edge, not searched for).
//
// Cold: reached only from sync() when a slot's text/style/starttime changed.
// NOTE: COLD per-event outline bake; integer edge walk, float arc fractions.
void bakeYesOutline(Slot& slot, int starttime) {
    namespace wm = ts::yeswm;

    float uMax = 1e-3f;
    /* @vfp-exempt R2 — cold wordmark outline bake: the per-edge dot count is SOLVED from the edge length (round(len/step)), which is the T1-correct form; an integer-only count would have to search for it. measured n/a. Verified 2026-09-26. */
    for (int gi = 0; gi < wm::GLYPH_COUNT && slot.count < MAX_DOTS; ++gi) {
        const wm::Glyph& G = wm::GLYPHS[gi];
        for (int ci = 0; ci < G.nContours && slot.count < MAX_DOTS; ++ci) {
            const wm::Contour& ct = G.contours[ci];
            for (int i = 0; i < ct.n && slot.count < MAX_DOTS; ++i) {
                const wm::Pt& a = ct.pts[i];
                const wm::Pt& b = ct.pts[(i + 1) % ct.n];
                const float ex = b.x - a.x, ey = b.y - a.y;
                // The count is solved from the STRETCHED length: the dots are
                // laid down in the space the sign renders in, so a vertical
                // run gets the extra dots the stretch demands instead of
                // being pulled apart by it.
                const float eys = ey * YES_V_STRETCH;
                const float len = std::sqrt(ex * ex + eys * eys);
                const int n = (int)(len * (1.0f / YES_OUTLINE_STEP) + 0.5f);
                if (n <= 0) continue;               // a sub-step edge: no dot
                const float ooN = 1.0f / (float)n;
                /* @vfp-exempt R2,R3 — cold wordmark outline bake: n is SOLVED from the edge length, not searched, and the |u| track below bounds uHalf; integerizing the arc fraction shifts every dot off its contour point. measured n/a. Verified 2026-09-26. */
                for (int s = 0; s < n && slot.count < MAX_DOTS; ++s) {
                    const float f = (float)s * ooN;
                    Dot& d = slot.dots[slot.count];
                    d.u = a.x + ex * f;
                    d.v = (a.y + ey * f) * YES_V_ADJ;
                    d.h = splitmix32((uint32_t)slot.count * 0x9E3779B9u
                                    ^ (uint32_t)starttime);
                    // Baked white: STYLE_YES recolours at eval from the live
                    // web band, exactly as the lattice bake did.
                    d.r = d.g = d.b = 1.0f;
                    if ((d.h & 1u) == 0u) ++slot.lodCount;
                    const float au = d.u < 0.0f ? -d.u : d.u;
                    if (au > uMax) uMax = au;
                    ++slot.count;
                }
            }
        }
    }
    slot.uHalf   = uMax;
    slot.dotStep = YES_OUTLINE_STEP;
}

// Is (px, py) inside the glyph's INK? Even-odd over every contour the glyph
// owns, so a counter ('A', 'O', the '!' dot's own ring) arrives as another
// contour and cancels rather than filling. COLD: bake-time only.
/* @vfp-exempt R3 — cold wordmark interior test: the float edge crossings define which sheet dots exist; integerizing the ray test changes the baked set. measured n/a. Verified 2026-09-26. */
bool insideGlyph(const ts::yeswm::Glyph& G, float px, float py) {
    int crossings = 0;
    for (int ci = 0; ci < G.nContours; ++ci) {
        const ts::yeswm::Contour& c = G.contours[ci];
        for (int i = 0; i < c.n; ++i) {
            const ts::yeswm::Pt& a = c.pts[i];
            const ts::yeswm::Pt& b = c.pts[(i + 1) % c.n];
            if ((a.y > py) != (b.y > py)) {
                const float xint = a.x + (py - a.y) * (b.x - a.x) / (b.y - a.y);
                if (px < xint) ++crossings;
            }
        }
    }
    return (crossings & 1) != 0;
}

// Cold: reached only from bakeSlot for STYLE_YES, after bakeYesOutline.
void bakeYesFill(Slot& slot, int starttime) {
    // The translucent CENTRE that fades in behind a coalesced STYLE_YES sign.
    // Appended to the same `dots` array after the stroke set and recorded in
    // slot.fillCount, so one array and one bake cap cover both.
    //
    // The grid is per-glyph (each glyph's own bbox), which keeps the point
    // count down and means a dot never has to be tested against a neighbour's
    // contours. The position jitter is BAKED, not applied at eval: an
    // additive sheet of perfectly regular dots reads as a mesh, and doing it
    // here costs the hot loop nothing.
    //
    // uHalf is deliberately NOT extended by the jitter: evalWorld derives the
    // whole sign's scale k from it, and letting the sheet nudge k would put
    // the centre out of register with the stroke it sits inside.
    namespace wm = ts::yeswm;
    const int first = slot.count;

    for (int gi = 0; gi < wm::GLYPH_COUNT && slot.count < MAX_DOTS; ++gi) {
        const wm::Glyph& G = wm::GLYPHS[gi];
        if (G.nContours <= 0) continue;

        /* @vfp-exempt R3 — cold wordmark interior bake: bbox min/max over contour floats; integerizing the bounds moves every sheet dot off the ink. measured n/a. Verified 2026-09-26. */
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        for (int ci = 0; ci < G.nContours; ++ci) {
            const wm::Contour& c = G.contours[ci];
            for (int i = 0; i < c.n; ++i) {
                const float px = c.pts[i].x, py = c.pts[i].y;
                if (px < x0) x0 = px;
                if (px > x1) x1 = px;
                if (py < y0) y0 = py;
                if (py > y1) y1 = py;
            }
        }
        // Integer iteration counts: the grid extent is SOLVED from the bbox,
        // not searched (AGENTS.md T1/T2). The y pitch is PRE-SHRUNK by the
        // letter stretch so that, once y is scaled, the effective pitch is
        // YES_FILL_STEP on both axes -- a square grid in the rendered space.
        // The inside test stays in the wordmark's own (unstretched) space,
        // because that is where the contours live.
        const float ystep = YES_FILL_STEP * YES_LETTER_ASPECT;
        /* @vfp-exempt R2 — cold wordmark interior bake: the grid extent is SOLVED from the bbox in one cast, not searched; a float-free count would have to iterate for it. measured n/a. Verified 2026-09-26. */
        const int nx = (int)((x1 - x0) * (1.0f / YES_FILL_STEP) + 0.5f);
        const int ny = (int)((y1 - y0) * (1.0f / ystep) + 0.5f);

        /* @vfp-exempt R2,R3 — cold wordmark interior bake: grid points are SOLVED from integer i/j against a solved extent, and the insideGlyph cull is the baked dot set; integerizing the lattice shifts the sheet. measured n/a. Verified 2026-09-26. */
        for (int j = 0; j <= ny && slot.count < MAX_DOTS; ++j) {
            const float py = y0 + (float)j * ystep;
            for (int i = 0; i <= nx && slot.count < MAX_DOTS; ++i) {
                const float px = x0 + (float)i * YES_FILL_STEP;
                if (!insideGlyph(G, px, py)) continue;
                Dot& d = slot.dots[slot.count];
                const uint32_t h = splitmix32((uint32_t)slot.count * 0x9E3779B9u
                                            ^ (uint32_t)starttime ^ 0x5BF03635u);
                d.u = px + (h01(h, 0) - 0.5f) * YES_FILL_STEP * 0.5f;
                d.v = (py + (h01(h, 8) - 0.5f) * ystep * 0.5f) * YES_V_ADJ;
                d.h = h;
                // Baked white, like the stroke: recoloured at eval from the
                // live web band so the centre is the SAME colour as the ring.
                d.r = d.g = d.b = 1.0f;
                ++slot.count;
            }
        }
    }
    slot.fillCount = slot.count - first;
}

void bakeSlot(Slot& slot, const char* text, int starttime, int style) {
    slot.starttime = starttime;
    slot.style     = style;
    slot.count     = 0;
    slot.lodCount  = 0;
    slot.fillCount = 0;
    slot.textHash  = fnv1a(text);
    slot.uHalf     = 1.0f;
    slot.dotStep   = 0.0f;

    const int nchars = (int)std::strlen(text);
    if (nchars == 0) return;

    // The chant is the one style that is NOT a lattice: it takes the wordmark
    // outline and stops here. Everything below stays the filled scanline bake
    // the other five styles are built from.
    if (style == STYLE_YES) {
        bakeYesOutline(slot, starttime);
        bakeYesFill(slot, starttime);
        return;
    }

    const float width     = (float)nchars * CHAR_ADVANCE;
    const float centerOff = -SKON * 0.5f * (FSTR + 1.3f) * ((float)nchars - 0.25f);
    // Column pitch widens for long strings (bake budget); rows stay PITCH_Y so
    // the scanline structure survives at any length.
    float pitchX = width * (1.0f / 128.0f);
    if (pitchX < PITCH_Y) pitchX = PITCH_Y;

    // Per-event colour identity: stable hue anchor, +-55 degree sweep across
    // the string, ~1 dot in 8 a white sparkle. Baked once, never repainted.
    // ONEUP strobes and YES shades from the web band, both at eval time, so
    // they bake white.
    const bool evalColored = (style == STYLE_ONEUP || style == STYLE_YES);
    const uint32_t eh      = splitmix32((uint32_t)starttime * 2654435761u ^ slot.textHash);
    const float    hueBase = (float)(eh % 3600u) * 0.1f;
    const float    ooHalfW = 2.0f / width;

    float uMax = 1e-3f;
    // NOTE: COLD raster bake (sync calls only on textHash/style/starttime
    // change); float scanline induction defines the coveredAt call set.
    /* @vfp-exempt R2,R3,R5 — cold per-event raster bake: float gy/gx induction and coverage culls define the baked lattice/LOD/uHalf; integer counters shift ULP rounding and change the bake. measured n/a. Verified 2026-09-06. */
    for (float gy = -0.35f; gy <= 2.35f; gy += PITCH_Y) {
        for (float gx = -0.6f; gx <= width + 0.6f; gx += pitchX) {
            if (!coveredAt(text, nchars, gx, gy)) continue;
            if (slot.count >= MAX_DOTS) { slot.uHalf = uMax; return; }
            Dot& d = slot.dots[slot.count];
            d.u = gx + centerOff;
            d.v = gy - 1.0f;
            d.h = splitmix32((uint32_t)slot.count * 0x9E3779B9u ^ (uint32_t)starttime);
            if (evalColored) {
                d.r = d.g = d.b = 1.0f;
            } else if ((d.h & 7u) == 0u) {
                d.r = d.g = d.b = 1.0f;              // white sparkle
            } else {
                hueToRgb(hueBase + d.u * ooHalfW * 55.0f, d.r, d.g, d.b);
            }
            if ((d.h & 1u) == 0u) ++slot.lodCount;   // survives the far-copy LOD
            // NOTE: u-half extent tracks the widest baked dot for the k scale.
            /* @vfp-exempt R3 — uHalf extent track: au>uMax keeps the baked half-width bound; float compare on the baked lattice. measured n/a. Verified 2026-09-06. */
            const float au = d.u < 0 ? -d.u : d.u;
            if (au > uMax) uMax = au;
            ++slot.count;
        }
    }
    slot.uHalf = uMax;
}

inline float smooth01(float x) {
    if (x < 0.0f) return 0.0f;
    if (x > 1.0f) return 1.0f;
    return x * x * (3.0f - 2.0f * x);
}

inline float clamp01(float x) {
    if (x < 0.0f) return 0.0f;
    if (x > 1.0f) return 1.0f;
    return x;
}

} // namespace

void sync(State& st, const EventView* events, int numEvents) {
    const int n = numEvents < SLOTS ? numEvents : SLOTS;
    for (int i = 0; i < n; ++i) {
        const EventView& e = events[i];
        if (!e.text || e.text[0] == '\0') continue;
        Slot& s = st.slots[i];
        if (s.starttime == e.starttime && s.style == e.style &&
            s.textHash == fnv1a(e.text)) continue;
        bakeSlot(s, e.text, e.starttime, e.style);
    }
}

int evalWorld(const State& st, int slot, int nowMs, float beat,
              bool photosensitiveSafe, int currentLevel, float anchorZn,
              const int* yesBeatMs, int yesBeatCount, WorldDot* out, int cap) {
    if (anchorZn < 0.0f) anchorZn = 0.0f;
    const Slot& s = st.slots[slot];
    if (s.count == 0 || s.starttime < 0 || s.style == 0) return 0;
    const int age = nowMs - s.starttime;
    const int dur = durationMs(s.style);
    if (age < 0 || age >= dur) return 0;
    if (beat < 0.0f) beat = 0.0f;
    if (beat > 1.0f) beat = 1.0f;

    // Text-x -> R units at spread 1. The burst styles get a 1.6x boost: they
    // DWELL at spread 1 (unlike the 1UP, whose reference ramp keeps growing),
    // and a dwell that only spans the web hole perspective-shrinks to a
    // caption — the word should dominate the web like the reference's UP does.
    float k = SPAN_R / s.uHalf;
    if (s.style != STYLE_ONEUP) k *= 1.6f;
    const float kY    = k * ASPECT;
    const float size0 = 0.115f * kY;           // FIXED dot half-extent, R units
    int n = 0;

    // Near cull. The EYE is at zn = anchorZn - CAM_AHEAD, not at the rim, so a
    // fixed `zn < 0.02` cut the through-the-camera finale 0.32 tube lengths
    // (8 world units) SHORT of the viewer for every celebration not fired
    // during an exit dive -- and the phase-C depth curves are authored to reach
    // exactly -CAM_AHEAD (STREAK deliberately past it). Keep a small floor
    // anyway: both backends size a dot by 1/w with no upper clamp, so a dot
    // sitting on the eye would smear into a fullscreen additive quad. +0.05
    // puts the floor at w = 1.0, i.e. right at the GL near plane (and inside
    // the C3D one), so nothing visible is lost.
    const float znMin = anchorZn - CAM_AHEAD + 0.05f;

    if (s.style == STYLE_ONEUP) {
        // ---- arcade reference depth-extrusion, verbatim constants (see DOCTRINE.md) ----
        const float F = (float)age * 0.06f;
        float spread = F * (1.0f / 16.0f);
        if (spread < 0.06f) spread = 0.06f;
        float zFar = (210.0f - F) * (1.0f / 110.0f);
        if (zFar > 1.35f) zFar = 1.35f;
        float zStep = F * (1.0f / 440.0f);
        if (zStep < 1e-3f) zStep = 1e-3f;
        int copies = (int)(zFar / zStep) + 1;
        // WIDEN, NEVER TRUNCATE. Two ceilings bind here, and both must widen
        // zStep rather than drop copies: the copy budget, and the OUTPUT cap.
        // The loop walks c = 0 at the FAR end, so an `n < cap` bail amputates
        // the NEAR face -- exactly the failure this rule exists to prevent
        // (DOCTRINE.md; the near face is the whole drama). Fit the copy count to
        // whatever `cap` can actually hold, then re-space the column across it.
        // Copies at zn > 0.85 shed ~half their dots to the LOD below, so budget
        // them at half; that LOD is also the only reason 371 dots x 12 copies
        // (= 4452) fits MAX_WORLD_OUT at all -- see shatter.h.
        if (copies > ONEUP_MAX_COPIES) copies = ONEUP_MAX_COPIES;
        if (s.count > 0 && copies > 1) {
            // Solve for the largest copy count that FITS, evaluating each
            // candidate at the spacing that candidate would actually produce
            // (widening the step moves copies out of the LOD band above 0.85,
            // which RAISES the dot total -- so estimating at a different
            // spacing under-counts and the cap still binds).
            const int perFull = s.count;
            const int perLod  = s.lodCount;
            // NOTE: copy-fit loop under the widen-don't-truncate law.
            /* @vfp-exempt R3 — ONEUP copy-fit: float stepC/znC select copy count under the 0.85 LOD band; fixed-point moves copies across the band and changes every zn. measured n/a. Verified 2026-09-06. */
            for (; copies > 1; --copies) {
                const float stepC = zFar / (float)(copies - 1);
                int used = 0;
                for (int c = 0; c < copies; ++c) {
                    const float znC = zFar - (float)c * stepC;
                    if (znC < 0.0f) break;
                    used += (znC > 0.85f) ? perLod : perFull;
                }
                if (used <= cap) break;
            }
        }
        if (copies > 1) zStep = zFar / (float)(copies - 1);
        // CRY low-byte cycling = magenta INTENSITY strobe, paling as it spreads.
        float phase = F * (1.0f / 16.0f);
        phase -= (float)(int)phase;
        const float inten = photosensitiveSafe ? 0.8f : 0.55f + 0.45f * phase;
        float wmix = spread * (0.6f / 9.4f);
        if (wmix > 0.6f) wmix = 0.6f;
        const float cr = inten, cg = (0.28f + wmix * 0.72f) * inten, cb = inten;
        float a = 0.35f;
        if (age < 130) a *= (float)age * (1.0f / 130.0f);
        const int tail = dur - age;
        if (tail < 200) a *= (float)tail * (1.0f / 200.0f);

        /* @vfp-exempt R3 — ONEUP per-copy emission loop (pins the zn bound and LOD branches below): zn bound gates the emitted set, LOD band halves far-copy dots. measured n/a. Verified 2026-09-06. */
        for (int c = 0; c < copies && n < cap; ++c) {
            const float zn = zFar - (float)c * zStep;
            // NOTE: zn bound + LOD band gate the emitted set and cap budget.
            if (zn < 0.0f) break;                  // the reference's loop bound
            const bool lod = (zn > 0.85f);
            // NOTE: LOD bit-test selects the far-copy dot subset.
            /* @vfp-exempt R3 — ONEUP LOD bit-test: integer hash test selects the shed subset above 0.85; branch skips writes. measured n/a. Verified 2026-09-06. */
            for (int i = 0; i < s.count && n < cap; ++i) {
                const Dot& d = s.dots[i];
                if (lod && (d.h & 1u)) continue;
                WorldDot& o = out[n++];
                o.tx = d.u * k * spread;  o.ty = d.v * kY * spread;
                o.zn = zn;                o.size = size0;
                o.r = cr; o.g = cg; o.b = cb; o.a = a;
            }
        }
        return n;
    }

    if (s.style == STYLE_YES) {
        // ---- the climb-out chant: one sign per spoken "YES!" ---------------
        // Driven by the VOICE, not a table: yesBeatMs carries the timestamps
        // at which the looping Yes sample actually says the word (game_step
        // integrates the playback phase; the glissando makes them accelerate,
        // ~0.26s / 1.31s / 2.19s / 2.92s). Each stamp spawns its own sign,
        // which coalesces out of a dot cloud, RUSHES the camera on its own
        // clock, and detonates as it passes -- so you hear "yes", a YES!
        // lands, it flies at you and bursts, and the next one is already
        // coming in faster. Signs are spiralled by index so no two stack up.
        //
        // All depths are measured from the CAMERA and added to anchorZn, which
        // tracks the diving camera -- world-parked text would whip past in a
        // blink (that bug is why only one tiny YES! was visible on hardware).
        constexpr float D_START = 0.72f;   // spawn depth ahead of the eye
        constexpr float D_BURST = 0.10f;   // detonates once this close
        constexpr int   FLIGHT  = 1150;    // ms from spoken to detonating
        constexpr float RING    = 1.05f;   // spiral radius off the tube axis
        constexpr float BASE    = 0.60f;   // sign size vs a normal message
        // Uniform size knob. Applied to the layout (kx/ky) AND the dot sizes
        // (sz/fsz) together so the whole sign scales as one: the stroke-closure
        // ratio dot/spacing = FAT*THICK/BASE is scale-invariant, so halving
        // both leaves the stroke looking identical, just half as big. Halving
        // BASE alone would shrink the footprint but keep the dots full-size --
        // a thicker stroke, not a smaller sign. (user, 2026-09-27: -50%)
        constexpr float YES_SCALE = 0.5f;
        // The coalesce window: the sign is fully materialised at this age,
        // which is also the instant the centre sheet starts fading in.
        constexpr int   YES_COALESCE_MS = 300;

        const float kx = k * BASE * YES_SIZE_FIX * YES_SCALE,
                  ky = kY * BASE * YES_SIZE_FIX * YES_SCALE;
        // The outline bake sizes its dots from their own spacing so the run
        // closes into a stroke; size0 is the LATTICE's fixed-dot law and would
        // leave the outline a sparse sprinkle. YES_THICK is the half-thickness
        // the request was for. YES_SIZE_FIX multiplies the SPACING via kx/ky,
        // so it has to multiply the dot SIZE too -- scaling the layout without
        // the dots is exactly the beaded-necklace failure this line exists to
        // prevent, only 21% worse than not applying the fix at all.
        const float sz = s.dotStep * YES_OUTLINE_FAT * YES_THICK * k * YES_SIZE_FIX * YES_SCALE;
        // The centre sheet's dot half-extent: sized from its OWN step (same law
        // as the stroke, different closure factor) so the grid overlaps into a
        // surface instead of a mesh. Loop-invariant -- hoisted next to sz.
        const float fsz = YES_FILL_STEP * (YES_FILL_FAT * 0.5f) * k * YES_SIZE_FIX * YES_SCALE;
        // The stroke set and the sheet set share one array; the sheet is the
        // tail. Hoisted out of the instance loop -- the bake does not move.
        const int strokeEnd = s.count - s.fillCount;

        // No stamps yet (music off, or the analyzer never ran) -> fall back to
        // an even cadence so the effect still plays. AudioFeatures' contract:
        // degrade, never freeze.
        const bool haveBeats = (yesBeatMs && yesBeatCount > 0);

        for (int inst = 0; inst < YES_COUNT && n < cap; ++inst) {
            int ak;
            if (haveBeats) {
                if (inst >= yesBeatCount) break;          // not said yet
                ak = nowMs - yesBeatMs[inst];
            } else {
                ak = age - (200 + inst * 900);
            }
            if (ak < 0) break;
            if (ak > FLIGHT + 260) continue;              // burst is over

            // Flight: accelerating rush from D_START to the burst distance.
            const float pf   = clamp01((float)ak * (1.0f / (float)FLIGHT));
            const float dCur = D_START - (D_START - D_BURST) * pf * pf * (3.0f - 2.0f * pf);
            const float tdI  = clamp01(((float)ak - FLIGHT) * (1.0f / 260.0f));
            const float spreadI = 1.0f + 2.4f * tdI * tdI;

            // Per-sign shade from the LIVE WEB's colour band, lifted toward
            // white so the glyph separates from the same-hue tube behind it.
            float cr, cg, cb;
            webLevelColor(currentLevel,
                          (float)inst * (1.0f / (float)(YES_COUNT - 1)),
                          cr, cg, cb, 1.0f);
            cr = cr * 0.72f + 0.28f;
            cg = cg * 0.72f + 0.28f;
            cb = cb * 0.72f + 0.28f;
            // Arrival pop on the word, then a burst flare as it detonates.
            // NOTE: exp arrival-pop decay law, 1x per sign per frame.
            /* @vfp-exempt R3,R6 — YES arrival pop std::exp(-ak/170) with fastSin flare: decay law scales every dot color; no fastExp in math_lut.h. measured n/a. Verified 2026-09-06. */
            const float pop = std::exp(-(float)ak * (1.0f / 170.0f));
            const float pm  = 1.0f + (photosensitiveSafe ? 0.3f : 0.8f) * pop
                                   + 0.6f * fastSin(tdI * 3.1415927f);

            // Spiral seat, drawn INWARD as the sign closes. Held at a constant
            // world radius it would sweep off the screen edge exactly when it
            // detonates (measured: centre at x=-191px, then -314px on a +-200px
            // screen), so the payoff happened out of frame. Converging keeps
            // the burst in view and reads as the word flying at your face.
            const float ang  = (float)inst * 2.4f;        // ~137 deg apart
            const float lat  = RING * (0.45f + 0.55f * (dCur * (1.0f / D_START)));
            const float ox   = fastCos(ang) * lat * 1.05f;
            const float oy   = fastSin(ang) * lat * 0.72f;

            // Coalesce: dots converge from a scattered, deeper cloud.
            const float pc   = clamp01((float)ak * (1.0f / (float)YES_COALESCE_MS));
            const float ec   = pc * (2.0f - pc);
            const float scat = 1.0f - ec;
            const int tailMs = dur - age;
            float tailFade = 1.0f;
            if (tailMs < 150) tailFade = (float)tailMs * (1.0f / 150.0f);
            float aI = 0.85f * YES_BRITE * (0.35f + 0.65f * ec)
                     * (1.0f - tdI * 0.85f) * tailFade;

            // The centre: keyed off the coalesce's OWN completion, so it fades
            // in the instant the ring closes and is exactly zero through the
            // whole scatter window -- which is why the sheet never needs the
            // scatter morph the stroke gets. (1 - tdI) takes it back out as
            // the sign detonates, so the burst is the ring's, not a slab's.
            const float fillIn = clamp01(((float)ak - (float)YES_COALESCE_MS)
                                       * (1.0f / (float)YES_FILL_IN_MS));
            const float aF = YES_FILL_BRITE * fillIn * (1.0f - tdI) * tailFade;

            for (int i = 0; i < strokeEnd && n < cap; ++i) {
                const Dot& d = s.dots[i];
                const float hB = h01(d.h, 8), hC = h01(d.h, 16);
                float x = ox + d.u * kx * spreadI;
                float y = oy + d.v * ky * spreadI;
                /* @vfp-exempt R3 — YES per-dot coalesce loop (pins the scat branch and zn/x culls below): scat morph, tdI debris, discard sets define placement. measured n/a. Verified 2026-09-06. */
                // NOTE: coalesce scatter + debris field + culls are the morph law.
                if (scat > 0.0f) {          // only the coalesce window scatters
                    const float a2 = hC * 6.2831853f;
                    x += fastCos(a2) * scat * 0.55f;
                    y += fastSin(a2) * scat * 0.40f;
                }
                float zn = anchorZn + dCur - CAM_AHEAD + scat * (0.35f + hB * 0.30f);
                if (tdI > 0.0f) zn += tdI * (hB - 0.5f) * 0.45f;   // debris field
                // NOTE: YES zn/x culls are the emitted discard set.
                /* @vfp-exempt R3 — YES zn/x culls: discard set for the coalescing sign; branches skip WorldDot writes. measured n/a. Verified 2026-09-06. */
                if (zn < znMin || zn > anchorZn + 1.45f) continue;
                if (x < -4.5f || x > 4.5f) continue;
                WorldDot& o = out[n++];
                o.tx = x; o.ty = y; o.zn = zn; o.size = sz;
                o.r = cr * pm; o.g = cg * pm; o.b = cb * pm;
                o.a = aI;
            }

            // The translucent centre, same colour as the ring, dimmer, faded
            // in behind it. Skipped outright while fillIn is still 0 so the
            // arrival window pays nothing for a sheet it cannot see.
            if (aF > 0.0f) {
                /* @vfp-exempt R3 — YES centre-sheet gate + zn/x culls: fillIn/tdI decide whether the sheet exists this frame, and the culls are the same discard set as the stroke's. measured n/a. Verified 2026-09-26. */
                for (int i = strokeEnd; i < s.count && n < cap; ++i) {
                    const Dot& d = s.dots[i];
                    const float x = ox + d.u * kx * spreadI;
                    const float y = oy + d.v * ky * spreadI;
                    float zn = anchorZn + dCur - CAM_AHEAD;
                    // Same debris law as the ring, so the sheet tears with it
                    // instead of holding a flat plane through the detonation.
                    if (tdI > 0.0f) zn += tdI * (h01(d.h, 8) - 0.5f) * 0.45f;
                    if (zn < znMin || zn > anchorZn + 1.45f) continue;
                    if (x < -4.5f || x > 4.5f) continue;
                    WorldDot& o = out[n++];
                    o.tx = x; o.ty = y; o.zn = zn; o.size = fsz;
                    o.r = cr * pm; o.g = cg * pm; o.b = cb * pm;
                    o.a = aF;
                }
            }
        }
        return n;
    }


    // ---- message styles: approach -> legible dwell -> shatter --------------
    // The dwell parks CLOSE to the camera plane (anchorZn + 0.15 of tube
    // depth) so the word reads big — mid-tube depths perspective-shrink to
    // nothing — and the whole path rides anchorZn, so a message fired during
    // the level-exit dive ("outta here!") stays in front of the diving camera
    // instead of whipping past. The shatter then carries it through the
    // camera plane.
    const float T  = (float)dur;
    const float ta = (float)age / T;
    const float A_END = (s.style == STYLE_SLAM) ? 0.30f : 0.38f;
    const float B_END = 0.58f;
    const bool  inA = ta < A_END, inC = ta >= B_END;
    const float pA = clamp01(ta / A_END);

    float e = 1.0f, pB = 1.0f, tc = 0.0f;
    float zBase, spreadB;
    if (inA) {
        // NOTE: 2.5-exponent arrival easing is the approach visual law.
        /* @vfp-exempt R6 — SLAM arrival 1-pow(1-pA,2.5) easing curve: no fastPow in math_lut.h; approximation shifts every dot x/y/zn. measured n/a. Verified 2026-09-06. */
        e = (s.style == STYLE_SLAM) ? pA * pA                       // heavy arrival
                                    : 1.0f - std::pow(1.0f - pA, 2.5f);
        zBase   = 1.30f - e * 1.15f;
        spreadB = 0.35f + e * 0.65f;
    } else if (!inC) {
        pB      = (ta - A_END) / (B_END - A_END);
        zBase   = 0.15f - pB * 0.03f;
        spreadB = 1.0f + 0.03f * fastSin((float)age * 0.008f) + 0.08f * beat;
    } else {
        tc      = (ta - B_END) / (1.0f - B_END);
        zBase   = 0.12f - tc * tc * 0.42f;
        spreadB = 1.0f + 2.2f * tc * tc;
    }

    // Style frame state (computed once per frame).
    float amp = 0.0f, sizeMul = 1.0f;
    int   copies = 3;
    float zStepC = 0.05f;
    switch (s.style) {
        case STYLE_WAVE:
            // Ripple ribbon: amplitude collapses to read, explodes to tear.
            amp = inA ? 0.14f * (1.0f - e) + 0.02f
                      : (!inC ? 0.02f : 0.02f + 0.40f * tc);
            break;
        case STYLE_SLAM:
            // Dense oversized slab with a spring-overshoot landing.
            copies = 5; zStepC = 0.03f; sizeMul = 1.3f;
            if (inA)       spreadB *= 1.18f;
            // NOTE: damped spring-overshoot landing law, 1x/frame in dwell.
            /* @vfp-exempt R6 — SLAM spring exp(-5*pB)*cos(14*pB): overshoot amplitude/phase law; no fastExp in math_lut.h. measured n/a. Verified 2026-09-06. */
            else if (!inC) spreadB = 1.18f + 0.22f * std::exp(-5.0f * pB) *
                                              fastCos(14.0f * pB);
            else           spreadB = 1.18f + 3.2f * tc * tc;
            break;
        case STYLE_STREAK:
            // Ejection: stays centered, then blasts past the camera HARDEST
            // (the fastest z-rush of the styles), with depth motion-trails.
            if (inC) zBase = 0.12f - tc * tc * 0.55f;
            break;
        default: break;   // STYLE_CASCADE is fully per-dot, below
    }

    float aBase = 0.55f;
    if (age < 200) aBase *= (float)age * (1.0f / 200.0f);
    const int tail = dur - age;
    if (tail < 320) aBase *= (float)tail * (1.0f / 320.0f);

    const float wavePh  = (float)age * 0.010f;
    const float size    = size0 * sizeMul;
    const float ooUHalf = 1.0f / s.uHalf;

    float copyA = aBase;
    for (int c = 0; c < copies && n < cap; ++c) {
        if (c > 0) copyA *= 0.55f;
        const bool lod = c > 0;
        for (int i = 0; i < s.count && n < cap; ++i) {
            const Dot& d = s.dots[i];
            if (lod && (d.h & 1u)) continue;
            const float hB = h01(d.h, 8);

            // Per-dot depth/spread — CASCADE staggers both by column so the
            // word slithers in left-to-right and unzips off the same way.
            float zDot = zBase, spreadD = spreadB, tcD = tc;
            if (s.style == STYLE_CASCADE) {
                const float colT = clamp01((d.u * ooUHalf + 1.0f) * 0.5f);
                if (inA) {
                    const float pd = clamp01((pA - colT * 0.30f) * (1.0f / 0.70f));
                    const float ed = pd * (2.0f - pd);
                    zDot    = 1.30f - ed * 1.15f;
                    spreadD = 0.35f + ed * 0.65f;
                } else if (inC) {
                    tcD     = clamp01(tc * 1.55f - colT * 0.55f);
                    zDot    = 0.12f - tcD * tcD * 0.42f;
                    spreadD = 1.0f + 2.2f * tcD * tcD;
                }
            }

            float x = d.u * k * spreadD;
            float y = d.v * kY * spreadD;
            float zn = anchorZn + zDot + (float)c * zStepC;   // rides the dive
            if (s.style == STYLE_WAVE) {
                const float ph = d.u * 0.85f + wavePh;
                zn += amp * fastSin(ph);
                y  += amp * 0.35f * fastCos(ph);
            }
            if (inC) {
                // NOTE: per-dot depth scatter carries the camera rush.
                /* @vfp-exempt R3 — shatter depth scatter: per-style per-dot zn stagger; rescaling moves dots across the znMin cull. measured n/a. Verified 2026-09-06. */
                // per-dot depth scatter carries the camera rush
                if (s.style == STYLE_STREAK) zn -= tc * 0.45f * hB;
                else                         zn += tcD * (hB - 0.5f) * 0.32f;
            }
            if (zn < znMin || zn > anchorZn + 1.45f) continue;
            if (x < -4.5f || x > 4.5f) continue;
            {
                WorldDot& o = out[n++];
                o.tx = x; o.ty = y; o.zn = zn; o.size = size;
                o.r = d.r; o.g = d.g; o.b = d.b;
                o.a = copyA;
            }
            // Streak motion-trails: two ghosts trailing in DEPTH behind the
            // dot's rush toward the camera (no lateral motion).
            // NOTE: ghost spacing/alpha constants are the trail visual law.
            /* @vfp-exempt R3 — STREAK ghost trails: 0.08 depth spacing with 0.45/0.20 alpha law; integerizing re-quantizes trail alpha. measured n/a. Verified 2026-09-06. */
            if (s.style == STYLE_STREAK && inC && !lod && (d.h & 2u) && n + 2 <= cap) {
                for (int t2 = 1; t2 <= 2; ++t2) {
                    WorldDot& o = out[n++];
                    o.tx = x; o.ty = y;
                    o.zn = zn + 0.08f * (float)t2; o.size = size;
                    o.r = d.r; o.g = d.g; o.b = d.b;
                    o.a = copyA * (t2 == 1 ? 0.45f : 0.20f);
                }
            }
        }
    }
    return n;
}

} // namespace shatter
} // namespace ts
