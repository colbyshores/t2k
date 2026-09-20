#pragma once

// Font data: 51 characters, each up to 5 segments (x1,y1,x2,y2).
// Sentinel: x1 == -1 means no more segments.
struct FontSegment {
    int x1, y1, x2, y2;
};

// Maximum segments per character
static constexpr int MAX_FONT_SEGMENTS = 5;

// Number of defined characters
static constexpr int FONT_CHAR_COUNT = 51;

// Font segment data (defined in font_data.cpp)
extern const FontSegment FONT_DATA[FONT_CHAR_COUNT][MAX_FONT_SEGMENTS];
extern const int FONT_SEG_COUNT[FONT_CHAR_COUNT];

// Map ASCII character to font index (0-50), or -1 for space/unknown.
int charToIndex(char ch);

// Emit one string of the procedural vector font into this frame's UI batch.
// Glyphs are laid out in the 4:3 UI box (0..1.3333 x 0..1.0) and the result is
// additively blended -- but the BACKEND's UI pass owns the projection and the
// blend state, not this call. Vulkan pushes SDF capsules into a SegStream
// (vk_font.cpp); the PICA200 bakes triangles into a linearAlloc batch drawn
// under an ortho-tilt MVP (text_c3d.cpp, blend set on the eye draw). There is
// no OpenGL implementation -- the GL backend is gone.
void writeAfont(
    const char* text,
    float x, float y,
    float sx, float sy,
    float rotation,
    float thickness,
    float r, float g, float b, float a,
    bool center = false,
    bool stipple = false,
    int stx = 0, int sty = 0,
    float zoom = 1.0f
);

// THE ADVANCE LAW, hoisted so callers can MEASURE a string without drawing it.
// Both backends' writeAfont lay glyphs out identically: each character advances
// `skon * (thickness + 1.3)` in GLYPH units, and the per-string affine scales
// glyph x by `sx * 0.5`; a centred string is shifted by half of
// `charWidth * (len - 0.25)`, which is therefore its own span. Anything that
// needs to FIT text -- the pause menu's frost box -- must use this rather than
// guess, and both writeAfont implementations must keep agreeing with it.
inline float afontWidth(int len, float sx, float thickness, bool stipple = false) {
    if (len <= 0) return 0.0f;
    const float skon = stipple ? 2.35f : 2.0f;
    return skon * (thickness + 1.3f) * ((float)len - 0.25f) * (sx * 0.5f);
}

// Helper rendering functions
void renderLineSegment(float x1, float y1, float x2, float y2,
                       float thickness, float r, float g, float b, float a);

// UI-SPACE vector art (shapes, not glyph interiors).
//
// renderLineSegment bakes in an ABSOLUTE 0.3 end-cap extension. That is a
// GLYPH-space constant: writeAfont wraps its calls in a per-string affine that
// scales by sx*0.5 (~0.015), so the cap lands at a few thousandths of a screen
// and looks right. Called directly in the 1.3333 x 1.0 UI space, that same 0.3
// adds 22% of the screen width to EACH END of every segment -- which turns any
// shape you draw with it into a bundle of overlapping lines.
//
// This is the same glowing quad, but the cap is proportional to halfWidth, so
// it behaves at any scale. Use it for anything that is not a glyph.
void renderGlowLine(float x1, float y1, float x2, float y2,
                    float halfWidth, float r, float g, float b, float a);

// One TEXTURED triangle in UI space. The vector-font batch already carries a uv
// per vertex and uses the renderer's ordinary shader, so textured geometry can
// ride the same buffer as the glyphs -- it only needs a different TEV and a
// bound texture, which ts_ui_tube_texture() arranges. Emit these BEFORE any
// writeAfont call for the frame: the batch draws its leading textured span
// first, then the glyphs.
void renderTexTri(float x0, float y0, float u0, float v0,
                  float x1, float y1, float u1, float v1,
                  float x2, float y2, float u2, float v2,
                  float r, float g, float b, float a);
void renderDot(float x, float y, float size,
               float r, float g, float b, float a);

// STEREO DEPTH FOR UI GEOMETRY. Everything drawn after this call carries `d` as
// a disparity coefficient until it is set again; 0 (the default, and what every
// existing screen leaves it at) means EXACTLY zero disparity -- on the screen
// plane, which is the comfort rule for anything the player has to read.
//
// The backend decides what to do with it: the C3D twin shears the UI ortho per
// eye (x' = x + K*d), so one emitted batch serves both eyes at PER-VERTEX
// disparity. A single shift could only give a whole span one depth, which
// cannot express a carousel where every letter sits at its own distance.
//
// The flat backend carries it into the vertex and ignores it, exactly as the
// GL twin carries WarpStroke::zeye without reading it -- so the two backends
// cannot disagree about what depth a UI element is at.
void uiDepth(float d);

// ---- THE BATCH'S GUARANTEED CAPACITY -----------------------------------------
// Every primitive above lands in ONE per-frame batch, and on the C3D twin that
// batch TRUNCATES SILENTLY when it fills: the frame simply loses whatever was
// emitted last, with no error anywhere. A leaderboard that quietly drops its
// bottom rows is the shape of bug this exists to prevent.
//
// So the seam states a floor. A caller may assume at least this many vertices
// per frame, and a screen that could exceed it should static_assert against it
// rather than find out at 240p. The backends assert that they actually provide
// it, so the floor cannot drift below what a caller was promised.
constexpr int UI_BATCH_MIN_VERTS  = 24000;
// One renderGlowLine is five triangles -- but they are a closed FAN, so the C3D
// twin writes only its six distinct points and replays them from a static index
// buffer. It was 15 vertices before that (a stroke's shared points written over
// and over), which is what made stroke-heavy screens expensive in a way glyph
// counts hide. A dot is the same six.
constexpr int UI_VERTS_PER_STROKE = 6;
void renderStippleSegment(float x1, float y1, float x2, float y2,
                          float thickness, float r, float g, float b, float a,
                          int stx, int sty, float zoom,
                          float zzx, float zzy, float sx, float sy);
