// ============================================================================
// hud_icons.cpp — the shared powerup-glyph builder (hud_icons.h).
//
// The animation law is the one the two backends carried verbatim:
//   per copy v = 1..4:
//     translate wobble  (t + BASE + v*300) at PIf*0.00045 / PIf*0.0004, +-0.0025
//     rotation wobble   (t + BASE + v*400) at PIf*0.0003, +-5 deg
//   BASE is per glyph (0 / 2000 / 4000 / 6000) so the set never locks phases.
// ============================================================================

#include "hud_icons.h"

#include <cmath>

#include "game/engine.h"
#include "game/constants.h"
#include "game/math_lut.h"
#include "data/enemy_data.h"   // PLAYER_VERTICES — the jump claw reads the claw itself

namespace ts {
namespace hudicons {

namespace {

constexpr float PIf = 3.14159265f;

// ---- the row's layout (user request: "more in the upper right corner and
// spaced evenly ... some are bunched up on each other like the tremor") -----
// Four cells of ICON_SIZE on an even pitch, right-aligned near the top-right
// of the 1.3333 x 1.0 UI box. The left HUD (score, multiplier, lives) all
// sits under x ~0.4, so the row has the whole right half to itself.
// ICON_RIGHT is the RIGHTMOST cell's centre; half a cell plus the +-0.0025
// wobble leaves ~0.03 UI of margin to the screen edge.
constexpr float ICON_SIZE  = 0.060f;
constexpr float ICON_Y     = 0.938f;
constexpr float ICON_RIGHT = 1.268f;
constexpr float ICON_PITCH = 0.086f;
// Left-to-right: jump, tremor, zapper, droid — the order they had on screen
// before, now on an even pitch. Index 0 is the LEFTMOST cell.
inline constexpr float iconX(int cell) { return ICON_RIGHT - (3 - cell) * ICON_PITCH; }

// The warp triangles share the glyph row's pitch, so each lands squarely under
// a glyph cell; their own scan line (0.835) and base size are the historical
// ones. They ACCUMULATE LEFT TO RIGHT FROM THE JUMP COLUMN (user, 2026-09-02:
// "slid over to start accumulating under the jump icon rather than all the way
// to the right"): the first token sits under cell 0 and the row grows toward
// the droid, so earning them reads as a bar filling rather than as marks
// creeping in from the edge. Three tokens arm a warp
// (WARP_ICONS_FOR_WARP) and there are four cells, so the row never runs past
// the glyphs above it.
constexpr float WARP_ICON_Y     = 0.835f;
constexpr float WARP_ICON_SCALE = 0.045f;
inline constexpr float warpIconX(int v) { return iconX(0) + (float)v * ICON_PITCH; }

// The claw row's own layout. X0 moved LEFT by one digit-width when the yellow
// `lives / 5` numeral was removed from beside it: the numeral used to occupy
// x ~0.03..0.08 and the claws began after it, so deleting it left the corner
// with a visible hole and the score sitting alone above empty space (user,
// 2026-09-02: "theres an empty space now where there was the yellow number 0").
// The claws are the readout now, so they start where the readout starts.
constexpr float LIFE_ICON_X0    = 0.075f;
constexpr float LIFE_ICON_PITCH = 0.0725f;
constexpr float LIFE_ICON_Y     = 0.845f;
constexpr float LIFE_ICON_SCALE = 0.0033f;

// A 2x3 affine, local to this TU. Both backends carry their own copy of this
// same helper for their own draws; the shared builder must not depend on
// either, so it keeps a third private one rather than picking a side.
struct Xf {
    float a = 1.0f, b = 0.0f, c = 0.0f, d = 1.0f, tx = 0.0f, ty = 0.0f;
    inline void apply(float x, float y, float& ox, float& oy) const {
        ox = a * x + b * y + tx;
        oy = c * x + d * y + ty;
    }
};
inline Xf xfTranslate(Xf m, float x, float y) { m.tx += m.a * x + m.b * y; m.ty += m.c * x + m.d * y; return m; }
inline Xf xfScale(Xf m, float sx, float sy) { m.a *= sx; m.c *= sx; m.b *= sy; m.d *= sy; return m; }
inline Xf xfRotateDeg(Xf m, float deg) {
    const float r = deg * (PIf / 180.0f), cs = ts::fastCos(r), sn = ts::fastSin(r);
    Xf o;
    o.a = m.a * cs + m.b * sn;  o.b = m.b * cs - m.a * sn;
    o.c = m.c * cs + m.d * sn;  o.d = m.d * cs - m.c * sn;
    o.tx = m.tx; o.ty = m.ty;
    return o;
}

// One glyph-local segment; `streak` selects the quarter-alpha motion law.
struct GlyphSeg { float x1, y1, x2, y2; bool streak; };

struct Glyph {
    const GlyphSeg* segs;
    int   n;
    float anchorX, anchorY;   // UI-space CENTRE of the glyph's cell
    float phase;              // animation phase base (see header)
};

// Emit one glyph's four wobbling copies into the pool. `s` is the active
// scale (1 held / 0.3 missing); the base colour ramps {1, s, s} -> rgb.
//
// EVERY GLYPH IS FITTED TO ONE CELL. The four were authored at whatever size
// and origin each drawing wanted -- the bolt stood 0.090 UI tall, the droid
// cube 0.032, and the claw was centred on x while the others grew rightward
// from it -- so on screen they read as four unrelated marks at four sizes,
// bunched at uneven gaps. The bbox is measured here and mapped into
// ICON_SIZE about the anchor: uniform scale, so nothing is distorted, and the
// anchors below can then be a plain even pitch. The measurement is ~50 min/max
// pairs once per frame for the whole HUD.
void emitGlyph(IconSegPool& out, const Glyph& g, float t, float s,
               float cr, float cg, float cb) {
    if (g.n <= 0) return;
    float x0 = g.segs[0].x1, x1 = x0, y0 = g.segs[0].y1, y1 = y0;
    for (int i = 0; i < g.n; ++i) {
        const GlyphSeg& q = g.segs[i];
        const float px[2] = {q.x1, q.x2}, py[2] = {q.y1, q.y2};
        for (int k = 0; k < 2; ++k) {
            if (px[k] < x0) x0 = px[k];   if (px[k] > x1) x1 = px[k];
            if (py[k] < y0) y0 = py[k];   if (py[k] > y1) y1 = py[k];
        }
    }
    const float w = x1 - x0, h = y1 - y0;
    const float ext = (w > h ? w : h);
    const float k = ext > 1e-6f ? (ICON_SIZE / ext) : 1.0f;
    const float mx = (x0 + x1) * 0.5f, my = (y0 + y1) * 0.5f;

    for (int v = 1; v <= 4; ++v) {
        const float dx = g.anchorX + ts::fastSin((t + g.phase + v * 300) * (PIf * 0.00045f)) * 0.0025f;
        const float dy = g.anchorY + ts::fastCos((t + g.phase + v * 300) * (PIf * 0.0004f))  * 0.0025f;
        const float ra = ts::fastSin((t + g.phase + v * 400) * (PIf * 0.0003f)) * 5.0f * (PIf / 180.0f);
        const float rc = ts::fastCos(ra), rs = ts::fastSin(ra);
        for (int i = 0; i < g.n; ++i) {
            const GlyphSeg& q = g.segs[i];
            const float a = q.streak ? v * s * 0.0625f : v * s * 0.25f;
            const float ax = (q.x1 - mx) * k, ay = (q.y1 - my) * k;
            const float bx = (q.x2 - mx) * k, by = (q.y2 - my) * k;
            out.emit(dx + ax * rc - ay * rs, dy + ax * rs + ay * rc,
                     dx + bx * rc - by * rs, dy + bx * rs + by * rc,
                     cr, cg, cb, a);
        }
    }
}

// ---- the glyphs ------------------------------------------------------------
// Coordinates are UI units, glyph-local (before the wobble + anchor).

// SUPER ZAPPER — an original bolt (the UI_rework mark-up, grey arrows):
// a closed zigzag-strike silhouette, top jag to bottom tip and back, with a
// small crossed spark at its waist. Same family, same budget as the old
// 8-point loop, our own path.
const GlyphSeg ZAPP[] = {
    {0.024f, 0.090f, 0.008f, 0.048f, false},   // upper edge, down-left
    {0.008f, 0.048f, 0.018f, 0.048f, false},   // the waist notch
    {0.018f, 0.048f, 0.006f, 0.000f, false},   // the strike, down to the tip
    {0.006f, 0.000f, 0.030f, 0.056f, false},   // the return, up-right
    {0.030f, 0.056f, 0.020f, 0.056f, false},   // the return notch
    {0.020f, 0.056f, 0.024f, 0.090f, false},   // closed at the top
    {0.015f, 0.063f, 0.021f, 0.071f, true},    // the waist spark, crossed
    {0.021f, 0.063f, 0.015f, 0.071f, true},
};

// AI DROID (the UI_rework mark-up, blue arrow): the droid IS a tumbling
// wireframe cube (line_geometry.cpp buildAiDroid), so the slot is the cube --
// front face, back face, four connectors. While the droid rides the rim the
// glyph cycles the droid's own rainbow (constants.h aiDroidColor), so the
// slot is identifiable before the player ever catches the capsule; lost or
// never granted, it dims to the siblings' red ghost.
const GlyphSeg DROID[] = {
    {0.002f, 0.008f, 0.024f, 0.008f, false},   // front face
    {0.024f, 0.008f, 0.024f, 0.030f, false},
    {0.024f, 0.030f, 0.002f, 0.030f, false},
    {0.002f, 0.030f, 0.002f, 0.008f, false},
    {0.012f, 0.018f, 0.034f, 0.018f, false},   // back face
    {0.034f, 0.018f, 0.034f, 0.040f, false},
    {0.034f, 0.040f, 0.012f, 0.040f, false},
    {0.012f, 0.040f, 0.012f, 0.018f, false},
    {0.002f, 0.008f, 0.012f, 0.018f, false},   // connectors
    {0.024f, 0.008f, 0.034f, 0.018f, false},
    {0.024f, 0.030f, 0.034f, 0.040f, false},
    {0.002f, 0.030f, 0.012f, 0.040f, false},
};

// TREMOR — the SEISMOGRAPH TRACE (user-supplied shape, 2026-09-02): a flat
// baseline, a burst of rising-then-falling spikes, a flat baseline. Traced
// vertex-for-vertex from the reference path
//   M2 12 h3 l2-3 l2 6 l2-9 l2 12 l2-7 l2 4 l2-3 h5
// with y inverted (SVG y is DOWN, UI y is UP) and the origin moved to the
// trace's start. Straight chords, MITRED not rounded -- the reference's
// stroke-linejoin is deliberately not reproduced: this renderer draws vector
// strokes, and every other glyph in the row is a bare polyline.
// Nine segments, no motion streaks: "just a line in the shape of this".
const GlyphSeg TREMOR[] = {
    {0.000f,  0.000f, 0.006f,  0.000f, false},   // the flat lead-in
    {0.006f,  0.000f, 0.010f,  0.006f, false},   // up
    {0.010f,  0.006f, 0.014f, -0.006f, false},   // down through the baseline
    {0.014f, -0.006f, 0.018f,  0.012f, false},   // the big rise
    {0.018f,  0.012f, 0.022f, -0.012f, false},   // the big fall
    {0.022f, -0.012f, 0.026f,  0.002f, false},   // the recovery
    {0.026f,  0.002f, 0.030f, -0.006f, false},
    {0.030f, -0.006f, 0.034f,  0.000f, false},   // settling to the baseline
    {0.034f,  0.000f, 0.044f,  0.000f, false},   // the flat run-out
};

// JUMP — the arcade claw leaping AWAY from the web (the UI_rework mark-up).
// The claw's look moved from Tsunami 2010's to the T2K / T2K
// wedge, and the icon follows: the outline is READ FROM PLAYER_VERTICES
// (data/enemy_data.h) — the very table the in-game claw and the ship-life
// icons draw, so the icon can never drift from the claw's shape — traced as
// the half-polygon boundary plus its mirror. The vertical sense is the
// PLAYER_VERTICES table's own (user call, looked at on screen: the first cut
// negated y and read wrong in the HUD row); the three staggered motion
// streaks trail the feet on the same side, so the wake always points back
// the way the claw came.
constexpr float JUMP_CLAW_SCALE = 0.0026f;   // 20 model units -> 0.052 UI wide
constexpr int JUMP_SEG_MAX = 16;
struct JumpGlyph { GlyphSeg segs[JUMP_SEG_MAX]; int n; };

JumpGlyph makeJumpGlyph() {
    JumpGlyph g; g.n = 0;
    // Closed outline: v0..v5 then the mirrored v4..v1, closing back on v0.
    // A negative code selects the vertex's X-mirror.
    const int outline[11] = {0, 1, 2, 3, 4, 5, -4, -3, -2, -1, 0};
    for (int i = 0; i + 1 < 11; ++i) {
        auto pt = [&](int code, float& x, float& y) {
            const int k = code < 0 ? -code : code;
            const Vertex2D& v = PLAYER_VERTICES[k];
            x = (code < 0 ? -(float)v.x : (float)v.x) * JUMP_CLAW_SCALE;
            y = (float)v.y * JUMP_CLAW_SCALE;    // see the flip note above
        };
        GlyphSeg& s = g.segs[g.n++];
        pt(outline[i], s.x1, s.y1);
        pt(outline[i + 1], s.x2, s.y2);
        s.streak = false;
    }
    // The wake: three staggered vertical streaks under the feet (post-flip:
    // negative y is screen-down, back toward the web).
    static const float sx[3]  = {-0.0156f, 0.0f,   0.0156f};
    static const float sy2[3] = {0.0200f, 0.0260f, 0.0180f};
    for (int i = 0; i < 3; ++i)
        g.segs[g.n++] = GlyphSeg{sx[i], 0.006f, sx[i], sy2[i], true};
    return g;
}
const JumpGlyph JUMP = makeJumpGlyph();

} // namespace

void buildPowerupIcons(IconSegPool& out, const GameEngine& engine) {
    const auto& pl = engine.player;
    const float t = (float)engine.time;

    // Emission order is the contract (hud_icons.h): zapp, tremor, jump, droid.
    {
        const float s = pl.zapp_stock > 0 ? 1.0f : 0.3f;
        const Glyph g{ZAPP, (int)(sizeof(ZAPP) / sizeof(ZAPP[0])), iconX(2), ICON_Y, 0.0f};
        emitGlyph(out, g, t, s, 1.0f, s, s);
    }
    {
        const float s = pl.has_tremor ? 1.0f : 0.3f;
        const Glyph g{TREMOR, (int)(sizeof(TREMOR) / sizeof(TREMOR[0])), iconX(1), ICON_Y, 2000.0f};
        emitGlyph(out, g, t, s, 1.0f, s, s);
    }
    {
        const float s = pl.has_jump ? 1.0f : 0.3f;
        const Glyph g{JUMP.segs, JUMP.n, iconX(0), ICON_Y, 4000.0f};
        emitGlyph(out, g, t, s, 1.0f, s, s);
    }
    {
        // Highlight while the droid is collected (and cycle its rainbow);
        // dim back to the red ghost the moment it is lost (engine.ai_droid
        // clears per level and per death).
        const float s = engine.ai_droid ? 1.0f : 0.3f;
        float cr = 1.0f, cg = s, cb = s;
        if (engine.ai_droid) ts::aiDroidColor(engine.time, cr, cg, cb);
        const Glyph g{DROID, (int)(sizeof(DROID) / sizeof(DROID[0])), iconX(3), ICON_Y, 6000.0f};
        emitGlyph(out, g, t, s, cr, cg, cb);
    }
}

// ---------------------------------------------------------------------------
// The warp triangles, hoisted out of the two backends (header block).
//
// The animation law is the duplicated one, byte for byte: 5 overlaid copies,
// each rotated +-5*(5-v) deg and translated +-0.05*(5-v) on its own slow
// phase, alpha ramping 0.05*v, and the three vertex colours each breathing one
// channel. ONLY the anchor x changed.
// ---------------------------------------------------------------------------
void buildWarpIcons(IconTriPool& out, const GameEngine& engine) {
    const PlayerInfo& pl = engine.player;
    if (pl.warp_icons <= 0) return;
    const float t = (float)engine.time;

    for (int v3 = 0; v3 < pl.warp_icons; ++v3) {
        Xf m0 = xfTranslate(Xf{}, warpIconX(v3), WARP_ICON_Y);
        // The grow-in pulse: the NEWEST icon (rel -> 1) swells most, so
        // earning one reads as that column popping.
        const float rel = (float)(v3 + 1) / (float)pl.warp_icons;
        const float sc = WARP_ICON_SCALE + pl.warp_animation * 0.001f * rel * rel;
        m0 = xfScale(m0, sc, sc);
        for (int vv = 1; vv <= 5; ++vv) {
            Xf m = xfRotateDeg(m0, ts::fastSin((t + vv * 961 + v3 * 3674) * (PIf * 0.0006f)) * 5.0f * (5 - vv));
            m = xfTranslate(m, ts::fastSin((t + vv * 1123 + v3 * 3674) * (PIf * 0.0002f)) * 0.05f * (5 - vv),
                               ts::fastCos((t + vv * 1034 + v3 * 3674) * (PIf * 0.00019f)) * 0.05f * (5 - vv));
            const float av = 0.05f * vv;
            const float cA[4] = {std::fabs(ts::fastCos((t + vv * 837 + v3 * 1674) * (PIf * 0.000191f))) * 0.5f, 1.0f, 1.0f, av};
            const float cB[4] = {1.0f, 1.0f, std::fabs(ts::fastCos((t + vv * 734 + v3 * 2624) * (PIf * 0.00018f))) * 0.5f, av};
            const float cC[4] = {1.0f, std::fabs(ts::fastSin((t + vv * 803 + v3 * 1872) * (PIf * 0.000185f))) * 0.5f, 1.0f, av};
            float px[3], py[3];
            m.apply(-0.5f,  0.44f, px[0], py[0]);
            m.apply( 0.5f,  0.44f, px[1], py[1]);
            m.apply( 0.0f, -0.44f, px[2], py[2]);
            out.emit(px, py, cA, cB, cC);
        }
    }
}

// ---------------------------------------------------------------------------
// The claw row, hoisted out of the two backends (header block).
// ---------------------------------------------------------------------------
void buildLifeIcons(IconTriPool& out, const GameEngine& engine) {
    const PlayerInfo& pl = engine.player;
    // `lives` is attempts IN RESERVE -- death decrements it and game over is
    // lives < 0 -- so the number of attempts left, this one included, is
    // lives + 1. That is what the row draws.
    int n = pl.lives + 1;
    if (n > LIFE_ICON_MAX) n = LIFE_ICON_MAX;

    for (int v3 = 0; v3 < n; ++v3) {
        // UNIFORM scale (was 0.00475 x 0.006875, a 1.45:1 stretch tuned to the
        // old wedge). The claw's 2.5:1 arcade proportions are the whole point,
        // so the icon must not distort them -- 0.0033 fills the 0.0725 slot
        // (20 model units -> 0.066 wide, 8 -> 0.0264 tall).
        const Xf m0 = xfScale(xfTranslate(Xf{}, LIFE_ICON_X0 + v3 * LIFE_ICON_PITCH, LIFE_ICON_Y),
                              LIFE_ICON_SCALE, LIFE_ICON_SCALE);
        const Xf forms[2] = { m0, xfScale(m0, -1.0f, 1.0f) };   // the mirrored half
        for (int vv = 0; vv <= 1; ++vv) {
            const Xf& m = forms[vv];
            for (const Face& face : PLAYER_FACES) {
                const int fidx[3] = { face.v0, face.v1, face.v2 };
                float px[3], py[3], cols[3][4];
                for (int i = 0; i < 3; ++i) {
                    const Color4& c = PLAYER_COLORS[fidx[i]];
                    const Vertex2D& vtx = PLAYER_VERTICES[fidx[i]];
                    m.apply((float)vtx.x, (float)vtx.y, px[i], py[i]);
                    cols[i][0] = c.r / 255.0f; cols[i][1] = c.g / 255.0f;
                    cols[i][2] = c.b / 255.0f; cols[i][3] = (c.a / 255.0f) * 0.75f;
                }
                out.emit(px, py, cols[0], cols[1], cols[2]);
            }
        }
    }
}

} // namespace hudicons
} // namespace ts
