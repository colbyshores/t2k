// ============================================================================
// vk_font.cpp — the font.h seam (writeAfont & friends) on the Vulkan backend.
//
// Every stroke becomes one UI-space SDF capsule in the active segment stream,
// so glyphs, UI shapes and dots ride the same pipeline as every other line on
// screen and get the same antialiasing. They do NOT get the phosphor halo:
// segPushUI ORs in SEG_NOHALO (vk_streams.cpp), so seg.frag's halo term is
// exactly 0 for every stroke here and seg.vert does not even widen the quad
// for one -- the halo is world-space only (DOCTRINE.md: "a short Gaussian halo
// on world lines only").
//
// Geometry follows the GL font.cpp law exactly: writeAfont's per-string affine
// (translate, rotate, scale sx*0.5/sy*0.5), skon character spacing, the
// centre offset, renderLineSegment's absolute 0.3 glyph-space end cap
// (expressed as the capsule's half-width extension), renderGlowLine's
// proportional cap, and renderDot's diamond as a zero-length capsule.
//
// Widths: font.cpp draws a triangle fan whose alpha falls from `a` at the
// centreline to 0 at +-thickness -- a soft stroke whose visible core is about
// half the geometric width. The capsule half-width here is that visible core
// so the strings measure the same on screen; the soft skirt comes from the
// callers' own layered passes (ui/ neonLine, neonText) and the post-process
// bloom, NOT from the SDF halo -- there is none here, see above.
// ============================================================================

#include "renderer_vk.h"

#include <cmath>
#include <cstring>

#include "rendering/font.h"

#include "game/math_lut.h"   // fastSin/fastCos — one trig path on both targets
namespace {

ts::vkr::Renderer*  g_r = nullptr;
ts::vkr::SegStream* g_segs = nullptr;
ts::vkr::TriStream* g_tris = nullptr;

// Current writeAfont affine (UI units), applied to glyph-space points.
struct Affine { float a, b, c, d, tx, ty; };   // x' = a*x + b*y + tx ; y' = c*x + d*y + ty

inline void apply(const Affine& m, float x, float y, float& ox, float& oy) {
    ox = m.a * x + m.b * y + m.tx;
    oy = m.c * x + m.d * y + m.ty;
}

// UI units -> pixels at the 240-line reference: the UI box is 1.0 tall.
constexpr float UI_REF_PX = 240.0f;

// Push one UI capsule with half-width given in UI units.
void pushUiCapsule(float x1, float y1, float x2, float y2, float halfUi,
                   float r, float g, float b, float a, uint32_t extra = 0) {
    if (!g_segs) return;
    ts::vkr::segPushUI(*g_segs, x1, y1, x2, y2, halfUi * UI_REF_PX, r, g, b, a, extra);
}

} // namespace

namespace ts {
namespace vkr {

void fontSetTarget(Renderer* r, SegStream* segs, TriStream* tris) {
    g_r = r; g_segs = segs; g_tris = tris;
}

} // namespace vkr
} // namespace ts

// ---- font.h -------------------------------------------------------------------

// DELIBERATE NO-OP, and stated rather than half-wired. Flat rendering has no
// disparity at all, so there is nothing here to apply the depth to.
//
// A first cut stored it and wrote it into renderTexTri's vertex only -- but the
// strokes go through segPushUI, which does not carry a z, so the value reached
// one of the two paths. That reads as plumbed and is not, which is worse than
// not carrying it: the next reader would trust it. Wiring it properly means
// extending SegVert and segPushUI, and that is XR's job, on the day the UI
// plane stops discarding z (DOCTRINE.md's "the pause melt's stereo depth is
// 3DS-only" gap records the same shape of work).
void uiDepth(float) {}

void renderDot(float x, float y, float size, float r, float g, float b, float a) {
    // A diamond fan with alpha 0 at the tips: visible core ~ size/2.
    pushUiCapsule(x, y, x, y, size * 0.5f, r, g, b, a);
}

void renderGlowLine(float x1, float y1, float x2, float y2,
                    float halfWidth, float r, float g, float b, float a) {
    const float dx = x2 - x1, dy = y2 - y1;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 0.0001f) { renderDot(x1, y1, halfWidth, r, g, b, a); return; }
    // Proportional cap = halfWidth along the direction: the capsule's round
    // cap already extends by its half-width, so no explicit extension.
    pushUiCapsule(x1, y1, x2, y2, halfWidth * 0.5f, r, g, b, a);
}

void renderLineSegment(float x1, float y1, float x2, float y2,
                       float thickness, float r, float g, float b, float a) {
    // Glyph-space stroke; only reachable through writeAfont's affine below,
    // which is what turns the absolute 0.3 cap into UI units. Direct callers
    // (none today) get the identity affine.
    extern void fontLineGlyph(float, float, float, float, float, float, float, float, float);
    fontLineGlyph(x1, y1, x2, y2, thickness, r, g, b, a);
}

void renderTexTri(float x0, float y0, float u0, float v0,
                  float x1, float y1, float u1, float v1,
                  float x2, float y2, float u2, float v2,
                  float r, float g, float b, float a) {
    if (!g_tris) return;
    ts::vkr::TriVert v;
    v.flags = ts::vkr::SEG_UI;
    v.col[0] = r; v.col[1] = g; v.col[2] = b; v.col[3] = a;
    v.pos[2] = 0.0f;
    v.pos[0] = x0; v.pos[1] = y0; v.uv[0] = u0; v.uv[1] = v0; g_tris->push(v);
    v.pos[0] = x1; v.pos[1] = y1; v.uv[0] = u1; v.uv[1] = v1; g_tris->push(v);
    v.pos[0] = x2; v.pos[1] = y2; v.uv[0] = u2; v.uv[1] = v2; g_tris->push(v);
}

void renderStippleSegment(float, float, float, float, float, float, float, float, float,
                          int, int, float, float, float, float, float) {
    // Disabled on every backend (DOCTRINE.md "Intentional deviations").
}

// The glyph stroke under the current affine. Static state set by writeAfont.
namespace {
Affine g_glyphAffine = {1, 0, 0, 1, 0, 0};
float  g_glyphScale = 1.0f;   // UI units per glyph unit along the stroke width
}

void fontLineGlyph(float x1, float y1, float x2, float y2,
                   float thickness, float r, float g, float b, float a) {
    const float dx = x2 - x1, dy = y2 - y1;
    const float len = std::sqrt(dx * dx + dy * dy);
    float ax, ay, bx, by;
    if (len < 0.001f) {
        apply(g_glyphAffine, x1, y1, ax, ay);
        pushUiCapsule(ax, ay, ax, ay, thickness * 0.5f * 0.5f * g_glyphScale, r, g, b, a);
        return;
    }
    // font.cpp's fan extends each stroke by 0.3 glyph units with alpha
    // falling to ZERO at the tip -- a soft taper, not a solid cap. The capsule's
    // own round cap (radius = the visible core) is the equivalent solid part;
    // extending the centreline as well drew bright crosses at every joint.
    const float ndx = dx / len, ndy = dy / len;
    (void)ndx; (void)ndy;
    const float halfCore = thickness * 0.5f;             // visible core of the fan
    apply(g_glyphAffine, x1, y1, ax, ay);
    apply(g_glyphAffine, x2, y2, bx, by);
    pushUiCapsule(ax, ay, bx, by, halfCore * g_glyphScale, r, g, b, a);
}

void writeAfont(const char* text, float x, float y, float sx, float sy, float rotation,
                float thickness, float r, float g, float b, float a,
                bool center, bool stipple, int stx, int sty, float zoom) {
    (void)stx; (void)sty; (void)zoom;
    if (!text || text[0] == '\0' || !g_segs) return;
    const int textLen = (int)std::strlen(text);

    // translate(x,y) * rotateZ(rotation) * scale(sx*0.5, sy*0.5) [* translate(centre)]
    const float rad = rotation * 3.14159265358979f / 180.0f;
    const float cr = ts::fastCos(rad), sr = ts::fastSin(rad);
    const float scx = sx * 0.5f, scy = sy * 0.5f;
    const float skon = stipple ? 2.35f : 2.0f;
    const float charWidth = skon * (thickness + 1.3f);
    const float cx = center ? -skon * 0.5f * (thickness + 1.3f) * (textLen - 0.25f) : 0.0f;

    // Compose: p' = T(x,y) R S T(cx,0) p
    Affine m;
    m.a =  cr * scx; m.b = -sr * scy;
    m.c =  sr * scx; m.d =  cr * scy;
    m.tx = x + m.a * cx;
    m.ty = y + m.c * cx;
    g_glyphAffine = m;
    // Stroke width in UI units: the fan's perpendicular is scaled by the
    // geometric mean of the two axis scales (isotropic enough for text).
    g_glyphScale = std::sqrt(std::fabs(scx * scy));

    for (int ci = 0; ci < textLen; ++ci) {
        const int fi = charToIndex(text[ci]);
        if (fi < 0 || fi >= FONT_CHAR_COUNT) continue;
        const float hOffset = charWidth * ci;
        const int segCount = FONT_SEG_COUNT[fi];
        for (int s = 0; s < segCount; ++s) {
            const FontSegment& seg = FONT_DATA[fi][s];
            if (seg.x1 == -1) break;
            fontLineGlyph((float)seg.x1 + hOffset, (float)seg.y1,
                          (float)seg.x2 + hOffset, (float)seg.y2,
                          thickness, r, g, b, a);
        }
    }
    g_glyphAffine = {1, 0, 0, 1, 0, 0};
    g_glyphScale = 1.0f;
}
