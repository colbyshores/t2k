// ============================================================================
// text_c3d.cpp — Citro3D vector-font text (Phase 2).
//
// Port of the OpenGL immediate-mode writeAfont (font.cpp) to PICA200: instead of
// glBegin/glVertex per glyph segment, it BAKES the same glowing-line triangle
// geometry (renderLineSegment's 5-triangle fan, with alpha-0 glow edges) into a
// linearAlloc vertex buffer under a CPU 2D affine (translate/rotate/scale/center
// + per-glyph advance), then draws the whole HUD in one C3D_DrawArrays with an
// ortho-tilt MVP and additive blend (SRC_ALPHA, ONE) — matching the GL oracle.
//
// Reuses the main vertex shader (pos3/color4/uv2) + the vertex-color TEV stage.
// The glyph tables live in font_data.cpp (shared with the desktop backend).
// The desktop implements the same font.h seam in t2k_pc/src/rendering/vk_font.cpp;
// the two live in separate trees, so nothing needs excluding (t2k_3ds/Makefile's
// EXCLUDE_SRC is empty by construction) — this file provides the 3DS's own
// `writeAfont` symbol so ported HUD code links unchanged.
//
// Not yet ported: the `stipple` particle-grid path (animated titles only) — it
// falls back to solid segments here (HUD/score never sets stipple).
// ============================================================================

#include "rendering/font.h"
#include "text_c3d.h"

#include <3ds.h>
#include <citro3d.h>
#include <cmath>
#include <cstring>
#include "game/math_lut.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace ts {
namespace textc3d {

namespace {

// Must match renderer_c3d.cpp's C3DVertex (same attribute permutation 0x210).
struct Vertex {
    float pos[3];
    float color[4];
    float uv[2];
};

// ============================================================================
// THE FAN INDEX BUFFER -- why every UI primitive is now exactly 6 vertices.
//
// PICA200 has no line primitive, so every stroke is CPU-expanded into triangles
// and the cost of this whole path is VERTEX WRITES: 36 B each, into a 16 KB
// D-cache with no L2 on the OG.
//
// Both primitives this batch draws are CLOSED FANS that were being written out
// as independent triangles, so their shared points were written repeatedly:
//
//     a stroke  =  1 centre + 5 rim  =  6 distinct points, written as 15
//     a dot     =  1 centre + 4 rim  =  5 distinct points, written as 12
//
// Indexed, both become ONE 6-vertex slot replayed by ONE index pattern --
// (0,1,2)(0,2,3)(0,3,4)(0,4,5)(0,5,1) -- for 60% and 50% fewer vertex writes.
// The dot pads its 5th rim slot with its 1st, so its last triangle is
// degenerate and rasterises nothing: the same trick logo tris already use to
// ride quadIbo as {a,b,c,c}.
//
// STRIDE DISCIPLINE IS THE WHOLE CONTRACT. A partial fan would misalign every
// primitive after it, so an emitter reserves all 6 slots or emits nothing --
// never a short write when the batch fills.
constexpr int FAN_VERTS   = 6;    // centre + 5 rim
constexpr int FAN_INDICES = 15;   // 5 triangles

constexpr int MAX_VERTS = 24000;   // now ~4000 fan primitives (was ~1600 strokes)
// The floor font.h promises its callers. Lowering MAX_VERTS below it would
// silently start truncating screens that were written against the promise.
static_assert(MAX_VERTS >= UI_BATCH_MIN_VERTS,
              "the UI batch must provide the capacity font.h guarantees");

// Ping-pong pair (frame pipelining — see renderer_c3d.cpp's RendererC3D::frame
// note). beginFrame(parity) latches g_parity for BOTH the writeAfont build
// (which must run BEFORE the caller's C3D_FrameBegin so it can overlap the
// prior frame's still-in-flight GPU draw of the OTHER parity's g_buf) and the
// matching flush() (issue region, reads back the SAME latched parity).
shaderProgram_s* g_program = nullptr;
int              g_uLoc_mvp = -1;
Vertex*          g_buf[2] = { nullptr, nullptr };
int              g_parity = 0;
int              g_count = 0;
int              g_texSpan = 0;   // leading verts that are TEXTURED (see markTexturedSpan)
// Verts from here on are SHIFTED by flush()'s (dx,dy) -- the popup's per-eye
// stereo offset, applied as a draw-time MVP translate instead of being baked
// into vertex bytes. That is what lets ONE emitted batch serve both eyes.
int              g_shiftSpan = -1;   // -1 = unmarked; 0 is a valid mark

// ---- Bottom-screen overlay batch (touch panel) -----------------------------
// A SECOND, independent ping-pong pair. The main batch cannot be reused for
// the bottom screen: its g_buf[p] holds the top screen's HUD until FrameEnd
// (Citro3D reads vertex buffers lazily), so rewriting it mid-frame corrupts
// the already-recorded top draws -- the documented lazy-read hazard
// (renderer_c3d.cpp renderVectorText note). While g_ovlActive, every font.h
// primitive writes here instead; flushOverlay() draws it to the bottom target.
// Lazy-allocated on first use (panel-hidden boots never pay for the pair
// sized below), sticky-fail like the particle VBO.
// Budget: worst case the panel draws ~275 character-passes -- 5 rows of 18 plus
// captions at ROW_LAYERS(1), header/eyebrow/chip/back at TEXT_LAYERS(3) -- at
// up to 75 verts/char = ~21k, plus ~25 glow lines. Truncation here is SILENT
// (pushVert just drops), so this keeps ~40% headroom over that estimate rather
// than sitting on it.
constexpr int MAX_OVL_VERTS = 30000;   // 2 x 30000 x 36 B = 2.2 MB linear, lazy
Vertex*          g_ovlBuf[2] = { nullptr, nullptr };
int              g_ovlParity = 0;
int              g_ovlCount = 0;
bool             g_ovlActive = false;
bool             g_ovlFailed = false;

// ---- Pause-overlay batch (the pause menu's rows) ---------------------------
// The options screen is ~12 rows of up to ~28 characters plus the title, one
// layer each at up to 75 verts/char: ~25k worst case is far above it, and
// truncation here would silently drop menu rows, so 8192 is sized with room.
constexpr int MAX_PAUSE_VERTS = 8192;  // 2 x 8192 x 36 B = 590 KB linear, lazy
Vertex*          g_pauseBuf[2] = { nullptr, nullptr };
// Static, shared by all three batches, built once. Indices are RELATIVE to the
// draw's base vertex, and the base is moved by rebinding the BufInfo pointer --
// PICA's DrawElements has no base-vertex parameter, which is the same reason
// draw_Stars offsets its first vertex rather than its indices.
u16*             g_fanIbo = nullptr;
// Per-vertex stereo depth (see text_c3d.h). Both default to 0, which is exactly
// the pre-existing behaviour: every vertex flat on the screen plane, no shear.
float            g_depth    = 0.0f;
float            g_eyeShear = 0.0f;
int              g_pauseParity = 0;
int              g_pauseCount = 0;
bool             g_pauseActive = false;
bool             g_pauseFailed = false;

inline Vertex* activeBuf()   { return g_pauseActive ? g_pauseBuf[g_pauseParity]
                                 : g_ovlActive ? g_ovlBuf[g_ovlParity] : g_buf[g_parity]; }
inline int&    activeCount() { return g_pauseActive ? g_pauseCount
                                 : g_ovlActive ? g_ovlCount : g_count; }
inline int     activeMax()   { return g_pauseActive ? MAX_PAUSE_VERTS
                                 : g_ovlActive ? MAX_OVL_VERTS : MAX_VERTS; }
void           (*g_tevPassthrough)(int) = nullptr;   // renderer-supplied stage reset
C3D_Mtx          g_ortho;

// Active per-string affine (glyph space -> clip 0..1.3333 x 0..1), plus color.
float g_x, g_y, g_sx, g_sy, g_cosR, g_sinR, g_centerX;
float g_r, g_g, g_b, g_a;

inline void affine(float px, float py, float& ox, float& oy) {
    // p + centerTranslate -> scale(sx*0.5, sy*0.5) -> rotate(R) -> + (x,y)
    const float cx = (px + g_centerX) * g_sx;
    const float cy = py * g_sy;
    ox = cx * g_cosR - cy * g_sinR + g_x;
    oy = cx * g_sinR + cy * g_cosR + g_y;
}

inline void pushVert(float px, float py, float a) {
    Vertex* buf = activeBuf();
    int&    n   = activeCount();
    if (!buf || n >= activeMax()) return;
    Vertex& v = buf[n++];
    float ox, oy;
    affine(px, py, ox, oy);
    v.pos[0] = ox; v.pos[1] = oy; v.pos[2] = g_depth;
    v.color[0] = g_r; v.color[1] = g_g; v.color[2] = g_b; v.color[3] = a;
    v.uv[0] = 0.0f; v.uv[1] = 0.0f;
}

// Emit one fan slot: centre + 5 rim, ALL SIX OR NONE. A short write here would
// misalign every primitive after it in the batch, so the capacity test is done
// once, up front, rather than per vertex.
// Draw `verts` vertices of `base` as indexed fans. The base VERTEX is moved by
// rebinding the buffer pointer, because PICA's DrawElements has no base-vertex
// parameter -- the same reason draw_Stars offsets its first vertex instead.
//
// NEVER ISSUES A ZERO-INDEX DRAW. That wedges the PICA command list and the
// NEXT frame's C3D_FrameBegin(SYNCDRAW) blocks forever; it cost this project a
// hardware softlock and five wrong theories
// (docs/known-issues/pica-zero-index-draw-wedges-command-list.md).
inline void drawFans(Vertex* base, int verts) {
    if (!base || verts < FAN_VERTS || !g_fanIbo) return;
    const int fans = verts / FAN_VERTS;
    if (fans <= 0) return;
    C3D_BufInfo* bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, base, sizeof(Vertex), 3, 0x210);
    C3D_DrawElements(GPU_TRIANGLES, fans * FAN_INDICES, C3D_UNSIGNED_SHORT, g_fanIbo);
}

inline bool fanRoom() {
    return activeBuf() && (activeCount() + FAN_VERTS) <= activeMax();
}
inline void fan(float cx, float cy, float ca, const float rx[5], const float ry[5],
                const float ra[5]) {
    if (!fanRoom()) return;
    pushVert(cx, cy, ca);
    for (int i = 0; i < 5; ++i) pushVert(rx[i], ry[i], ra[i]);
}

// Glyph-space port of renderLineSegment: a glowing quad (center bright a,
// edges alpha 0) as a 5-triangle fan around v0=(x1,y1), v4=(x2,y2).
// Cap extension and thickness boost are GLYPH-space tuning; renderGlowLine
// overrides both because it draws in UI space where 0.3 is a fifth of the
// screen. Defaults reproduce the text path exactly.
float g_capExt     = 0.3f;
float g_thickBoost = 1.7f;

void segment(float x1, float y1, float x2, float y2, float thickness) {
    thickness *= g_thickBoost;   // legibility boost — strokes fade to alpha 0, so the
                                 // thin default reads faint on the 240p screen
    float dx = x2 - x1, dy = y2 - y1;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len < 0.001f) {
        // Degenerate -> renderDot: a small glowing diamond. FOUR rim points, so
        // the 5th slot repeats the 1st and the fan's last triangle collapses.
        const float s = thickness * 0.5f;
        const float rx[5] = { x1 - s, x1,     x1 + s, x1,     x1 - s };
        const float ry[5] = { y1,     y1 + s, y1,     y1 - s, y1     };
        const float ra[5] = { 0.0f,   0.0f,   0.0f,   0.0f,   0.0f   };
        fan(x1, y1, g_a, rx, ry, ra);
        return;
    }
    const float ndx = dx / len, ndy = dy / len;
    const float px = -ndy * thickness, py = ndx * thickness;   // perpendicular width
    const float ex = ndx * g_capExt, ey = ndy * g_capExt;      // end-cap extension

    // Ring vertices (GL fan order): v1..v6 (v6 == v1). Center verts carry alpha a,
    // ring verts alpha 0. Fan -> triangles (v0,v1,v2)(v0,v2,v3)(v0,v3,v4)(v0,v4,v5)(v0,v5,v6).
    const float v1x = x1 - ex - px, v1y = y1 - ey - py;
    const float v2x = x1 - ex + px, v2y = y1 - ey + py;
    const float v3x = x2 + ex + px, v3y = y2 + ey + py;
    const float v5x = x2 + ex - px, v5y = y2 + ey - py;
    // v4 = (x2,y2) alpha a ; v6 = v1
    // The same ring the five tri() calls used to spell out, written once each.
    const float rx[5] = { v1x, v2x, v3x, x2,   v5x };
    const float ry[5] = { v1y, v2y, v3y, y2,   v5y };
    const float ra[5] = { 0.0f, 0.0f, 0.0f, g_a, 0.0f };
    fan(x1, y1, g_a, rx, ry, ra);
}

} // namespace

void init(shaderProgram_s* program, int uLoc_mvp) {
    g_program = program;
    g_uLoc_mvp = uLoc_mvp;
    if (!g_buf[0]) g_buf[0] = (Vertex*)linearAlloc(sizeof(Vertex) * MAX_VERTS);
    if (!g_buf[1]) g_buf[1] = (Vertex*)linearAlloc(sizeof(Vertex) * MAX_VERTS);
    // One static index buffer serves every batch, and it must be sized by the
    // LARGEST of them: max(MAX_VERTS, MAX_OVL_VERTS) = 30000 verts = 5000 fans
    // x 15 indices x 2 B = 150 KB of linear heap, bought back many times over
    // in vertex writes that no longer happen. MAX_PAUSE_VERTS rides it only
    // because it is smaller than both -- drawFans takes its index count from
    // the batch's OWN vertex count and does not clamp against this buffer, so
    // raising that batch past MAX_OVL_VERTS means widening this max too.
    if (!g_fanIbo) {
        const int maxFans = (MAX_OVL_VERTS > MAX_VERTS ? MAX_OVL_VERTS : MAX_VERTS) / FAN_VERTS;
        g_fanIbo = (u16*)linearAlloc(sizeof(u16) * maxFans * FAN_INDICES);
        if (g_fanIbo) {
            for (int f = 0; f < maxFans; ++f) {
                u16* o = g_fanIbo + f * FAN_INDICES;
                const u16 b = (u16)(f * FAN_VERTS);
                for (int t = 0; t < 5; ++t) {          // closed fan over 5 rim points
                    o[t*3+0] = b;
                    o[t*3+1] = (u16)(b + 1 + t);
                    o[t*3+2] = (u16)(b + 1 + ((t + 1) % 5));
                }
            }
        }
    }
    // Ortho for the rotated top screen (logical 4:3 HUD space, matches GL glOrtho).
    Mtx_OrthoTilt(&g_ortho, 0.0f, 1.3333f, 0.0f, 1.0f, -1.0f, 1.0f, true);
}

void beginFrame(int parity) {
    g_parity = parity & 1;
    g_count = 0; g_texSpan = 0; g_shiftSpan = -1;
    g_depth = 0.0f;   // never let one screen's depth leak into the next
}

bool beginOverlay(int parity) {
    // On sticky failure STILL enter overlay mode: activeBuf() then returns the
    // null overlay pointer and every primitive no-ops. Falling through to the
    // MAIN batch here would scribble over the buffer the GPU may still be
    // reading from the previous frame -- the exact lazy-read hazard this
    // second buffer exists to avoid.
    if (g_ovlFailed) { g_ovlParity = parity & 1; g_ovlCount = 0; g_ovlActive = true; return false; }
    if (!g_ovlBuf[0]) {
        g_ovlBuf[0] = (Vertex*)linearAlloc(sizeof(Vertex) * MAX_OVL_VERTS);
        g_ovlBuf[1] = (Vertex*)linearAlloc(sizeof(Vertex) * MAX_OVL_VERTS);
        if (!g_ovlBuf[0] || !g_ovlBuf[1]) {
            // Give back a partial pair; never retry (linear pressure on OG).
            for (int i = 0; i < 2; ++i)
                if (g_ovlBuf[i]) { linearFree(g_ovlBuf[i]); g_ovlBuf[i] = nullptr; }
            g_ovlFailed = true;
            g_ovlParity = parity & 1; g_ovlCount = 0; g_ovlActive = true;
            return false;
        }
    }
    g_ovlParity = parity & 1;
    g_ovlCount  = 0;
    g_ovlActive = true;
    return true;
}

void endOverlay() { g_ovlActive = false; }

void flushOverlay() {
    Vertex* buf0 = g_ovlBuf[g_ovlParity];
    if (!buf0 || g_ovlCount == 0 || !g_program) { g_ovlCount = 0; return; }
    C3D_BindProgram(g_program);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_uLoc_mvp, &g_ortho);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_SRC_ALPHA, GPU_ONE, GPU_SRC_ALPHA, GPU_ONE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, buf0, sizeof(Vertex), 3, 0x210);
    // Vertex-colour only (no textured span on the bottom screen, v1).
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    if (g_tevPassthrough) g_tevPassthrough(1);
    drawFans(buf0, g_ovlCount);
    g_ovlCount = 0;
}

void shutdownOverlay() {
    for (int i = 0; i < 2; ++i)
        if (g_ovlBuf[i]) { linearFree(g_ovlBuf[i]); g_ovlBuf[i] = nullptr; }
}

// Pack an rgba float quad as the GPU_ABGR8 byte order the TEV constant wants
// (renderer_c3d.cpp's to_abgr, kept local: that helper is its anonymous
// namespace's, and this file deliberately carries no renderer dependency).
inline u32 tev_abgr(float r, float g, float b, float a) {
    auto c8 = [](float v) -> u32 { return (u32)(v * 255.0f + 0.5f) & 0xFF; };
    return (c8(a) << 24) | (c8(b) << 16) | (c8(g) << 8) | c8(r);
}

// ---- the pause overlay batch ----------------------------------------------
// The parity FLIPS here rather than following the renderer's frame counter.
// flushPause deliberately never clears the batch, so the ramp-out keeps
// re-reading one fixed half for up to 240 ms while r->frame advances; a
// re-pause k frames in would then land on that same half whenever k is even
// and rewrite the buffer the in-flight draw is sourcing -- the lazy-read
// hazard these separate batches exist to avoid. Flipping is unconditional and
// needs no knowledge of the caller's clock.
bool beginPause() {
    const int parity = g_pauseParity ^ 1;
    if (g_pauseFailed) { g_pauseParity = parity & 1; g_pauseCount = 0; g_pauseActive = true; return false; }
    if (!g_pauseBuf[0]) {
        g_pauseBuf[0] = (Vertex*)linearAlloc(sizeof(Vertex) * MAX_PAUSE_VERTS);
        g_pauseBuf[1] = (Vertex*)linearAlloc(sizeof(Vertex) * MAX_PAUSE_VERTS);
        if (!g_pauseBuf[0] || !g_pauseBuf[1]) {
            for (int i = 0; i < 2; ++i)
                if (g_pauseBuf[i]) { linearFree(g_pauseBuf[i]); g_pauseBuf[i] = nullptr; }
            g_pauseFailed = true;
            g_pauseParity = parity & 1; g_pauseCount = 0; g_pauseActive = true;
            return false;
        }
    }
    g_pauseParity = parity & 1;
    g_pauseCount  = 0;
    g_pauseActive = true;
    return true;
}

void endPause() { g_pauseActive = false; }

// This does NOT clear the batch: the un-pause ramp keeps
// drawing the same staged rows at a falling alpha on the normal frame, and
// nothing re-emits them once the menu has closed. beginPause owns the reset.
void flushPause(const C3D_Mtx& ortho, float alpha) {
    Vertex* buf0 = g_pauseBuf[g_pauseParity];
    if (!buf0 || g_pauseCount == 0 || !g_program || alpha <= 0.001f) return;
    C3D_BindProgram(g_program);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_uLoc_mvp, &ortho);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_SRC_ALPHA, GPU_ONE, GPU_SRC_ALPHA, GPU_ONE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, buf0, sizeof(Vertex), 3, 0x210);
    // The ramp modulates ALPHA ONLY. The draw blends SRC_ALPHA/ONE, so scaling
    // rgb as well would fade as alpha SQUARED while the PC twin -- which scales
    // only its vertices' alpha (vk_scene.cpp) -- fades linearly: one shared
    // ramp, two curves.
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvColor(env, tev_abgr(alpha, alpha, alpha, alpha));
    C3D_TexEnvSrc(env, C3D_RGB, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_PRIMARY_COLOR, GPU_CONSTANT, GPU_CONSTANT);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
    if (g_tevPassthrough) g_tevPassthrough(1);
    drawFans(buf0, g_pauseCount);
}

void shutdownPause() {
    for (int i = 0; i < 2; ++i)
        if (g_pauseBuf[i]) { linearFree(g_pauseBuf[i]); g_pauseBuf[i] = nullptr; }
}

void markTexturedSpan() { g_texSpan = g_count; }

// Everything appended after this call is drawn through flush()'s (dx,dy)
// translate. Call it once, between the unshifted content (the score) and the
// shifted content (the popup).
void markShiftSpan() { g_shiftSpan = g_count; }

void setTevPassthrough(void (*fn)(int)) { g_tevPassthrough = fn; }

void setUiDepth(float d)  { g_depth = d; }
float uiDepthValue()      { return g_depth; }
void setEyeShear(float k) { g_eyeShear = k; }

void flush(C3D_Tex* tubeTex, bool keep, float dx, float dy) {
    Vertex* buf0 = g_buf[g_parity];
    if (!buf0 || g_count == 0 || !g_program) {
        if (!keep) { g_count = 0; g_texSpan = 0; g_shiftSpan = -1; }
        return;
    }

    C3D_BindProgram(g_program);

    // THE EYE SHEAR. x' = x + g_eyeShear * z, expressed as ortho * S so the
    // shear happens in UI space before the projection -- the same ordering the
    // shift span's translate uses and for the same reason. At shear 0 the
    // matrix is bit-identical to g_ortho, which is every existing caller.
    C3D_Mtx base = g_ortho;
    if (g_eyeShear != 0.0f) {
        C3D_Mtx sh;
        Mtx_Identity(&sh);
        sh.r[0].z = g_eyeShear;
        Mtx_Multiply(&base, &g_ortho, &sh);
    }
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_uLoc_mvp, &base);

    // Additive glow, no depth (HUD overlay) -- shared by both spans.
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD,
                   GPU_SRC_ALPHA, GPU_ONE, GPU_SRC_ALPHA, GPU_ONE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_CullFace(GPU_CULL_NONE);

    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, buf0, sizeof(Vertex), 3, 0x210);

    const int texSpan = (tubeTex && g_texSpan > 0) ? g_texSpan : 0;

    // Span 1: textured geometry (the level-select tube), texture MODULATEd by
    // vertex colour so the palette/depth shading still applies over it.
    //
    // RGB and ALPHA ARE DELIBERATELY SPLIT. Modulating C3D_Both meant the
    // fragment's alpha was texture.a * vertex.a, and the blend below is
    // (GPU_SRC_ALPHA, GPU_ONE) -- so the skin's opacity was at the mercy of
    // whatever the procedural DSL happened to leave in the texture's alpha
    // channel. It leaves ZERO for texture sets 5, 10, 12, 14 and 16 (and a
    // partial 128/138 for 8 and 19), so those levels drew a complete, correctly
    // mapped skin that contributed exactly nothing to the framebuffer -- the
    // long-standing "some webs have no texture in the level select" bug. The
    // texture sets are fine; gameplay never noticed because the grid pass runs
    // its own separate TEV chain.
    //
    // Alpha now comes from the VERTEX only, which is what the caller actually
    // means by it: level_select.cpp passes skin_, the fade-in weight.
    if (texSpan > 0) {
        C3D_TexBind(0, tubeTex);
        C3D_TexEnv* env = C3D_GetTexEnv(0);
        C3D_TexEnvInit(env);
        C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
        C3D_TexEnvSrc(env, C3D_Alpha, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
        // Neutralise stages 1-5: a five-stage grid chain may still be resident
        // and would overwrite this sample downstream, which is exactly what
        // made the textured span render flat.
        if (g_tevPassthrough) g_tevPassthrough(1);
        C3D_DrawArrays(GPU_TRIANGLES, 0, texSpan);
    }

    // Span 2: everything else -- glyphs and untextured vector art.
    // Vertex-color TEV (REPLACE from primary color, incl. alpha for the glow).
    if (g_count > texSpan) {
        C3D_TexEnv* env = C3D_GetTexEnv(0);
        C3D_TexEnvInit(env);
        C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        if (g_tevPassthrough) g_tevPassthrough(1);

        // The shift span is drawn through a TRANSLATED ortho rather than with
        // the offset baked into its vertices, so one emitted batch can serve
        // both eyes at different disparities. Before this, the popup's shift
        // WAS baked in, which forced a full rebuild per eye -- and because the
        // batch parity is per FRAME, the left eye's rebuild was then reset and
        // overwritten by the right eye's before the GPU read it at FrameEnd.
        // Half the work was discarded and the popup rendered at ZERO
        // disparity, so STEREO_TEXT_FACTOR bought nothing.
        int shiftSpan = (g_shiftSpan >= 0) ? g_shiftSpan : g_count;
        if (shiftSpan < texSpan) shiftSpan = texSpan;
        if (shiftSpan > g_count) shiftSpan = g_count;
        const bool haveShift = (shiftSpan < g_count) && (dx != 0.0f || dy != 0.0f);

        if (!haveShift) {
            // Identical to the pre-split behaviour: ONE draw for everything
            // after the textured span. Mono, or a frame with no popup.
            drawFans(buf0 + texSpan, g_count - texSpan);
        } else {
            if (shiftSpan > texSpan)
                drawFans(buf0 + texSpan, shiftSpan - texSpan);
            // ortho * translate: Mtx_Translate's bRightSide=true post-
            // multiplies, which is the order that makes this equivalent to
            // adding (dx,dy) to every vertex in the same UI units the ortho
            // is built in (0..1.3333 x 0..1).
            C3D_Mtx m = base;
            Mtx_Translate(&m, dx, dy, 0.0f, true);
            C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_uLoc_mvp, &m);
            drawFans(buf0 + shiftSpan, g_count - shiftSpan);
            C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_uLoc_mvp, &base);   // restore
        }
    }

    // keep = the caller will issue this same batch again onto another target
    // (the stereo title screen's second eye). See text_c3d.h.
    if (!keep) { g_count = 0; g_texSpan = 0; g_shiftSpan = -1; }
}

} // namespace textc3d
} // namespace ts

// ---------------------------------------------------------------------------
// writeAfont — same signature/semantics as font.cpp's GL version, baking into
// the active textc3d batch instead of issuing immediate-mode GL.
// ---------------------------------------------------------------------------
void writeAfont(const char* text, float x, float y, float sx, float sy,
                float rotation, float thickness,
                float r, float g, float b, float a,
                bool center, bool stipple, int stx, int sty, float zoom) {
    using namespace ts::textc3d;
    (void)stipple; (void)stx; (void)sty; (void)zoom;   // stipple path not ported yet
    if (!text || text[0] == '\0' || !activeBuf()) return;

    const int len = (int)std::strlen(text);

    g_x = x; g_y = y;
    g_sx = sx * 0.5f; g_sy = sy * 0.5f;
    const float rad = rotation * (float)M_PI / 180.0f;
    g_cosR = ts::fastCos(rad); g_sinR = ts::fastSin(rad);
    g_r = r; g_g = g; g_b = b; g_a = a;
    g_capExt = 0.3f; g_thickBoost = 1.7f;   // glyph-space tuning (see segment)

    const float skon = 2.0f;                       // solid-segment spacing (non-stipple)
    const float charWidth = skon * (thickness + 1.3f);
    g_centerX = center ? -skon * 0.5f * (thickness + 1.3f) * (len - 0.25f) : 0.0f;

    for (int ci = 0; ci < len; ++ci) {
        int fi = charToIndex(text[ci]);
        if (fi < 0 || fi >= FONT_CHAR_COUNT) continue;   // space/unknown
        const float hOff = charWidth * ci;
        const int segs = FONT_SEG_COUNT[fi];
        for (int s = 0; s < segs; ++s) {
            const FontSegment& sg = FONT_DATA[fi][s];
            if (sg.x1 == -1) break;
            segment((float)sg.x1 + hOff, (float)sg.y1,
                    (float)sg.x2 + hOff, (float)sg.y2, thickness);
        }
    }
}

// ---------------------------------------------------------------------------
// renderLineSegment / renderDot — the rest of the font.h primitive seam.
//
// vk_font.cpp supplies the desktop versions (its own tree, never globbed here),
// so without these any shared ui/ code that draws vector art (rather than only
// text) links on desktop and fails only at 3DS link time. They bake into the
// SAME batch as the glyphs, so a screen mixing text and lines still costs one
// draw call.
//
// The caller's coordinates are already in the 1.3333 x 1.0 UI space that the
// glyph affine would otherwise map INTO, so the affine is set to identity
// rather than bypassed -- segment() is shared with the text path and must see
// a consistent transform.
// ---------------------------------------------------------------------------
void renderLineSegment(float x1, float y1, float x2, float y2,
                       float thickness, float r, float g, float b, float a) {
    using namespace ts::textc3d;
    if (!activeBuf()) return;
    g_x = 0.0f; g_y = 0.0f;
    g_sx = 1.0f; g_sy = 1.0f;
    g_cosR = 1.0f; g_sinR = 0.0f;
    g_centerX = 0.0f;
    g_r = r; g_g = g; g_b = b; g_a = a;
    g_capExt = 0.3f; g_thickBoost = 1.7f;   // glyph-space semantics, preserved
    segment(x1, y1, x2, y2, thickness);
}

void uiDepth(float d) { ts::textc3d::setUiDepth(d); }

void renderGlowLine(float x1, float y1, float x2, float y2,
                    float halfWidth, float r, float g, float b, float a) {
    using namespace ts::textc3d;
    if (!activeBuf()) return;
    g_x = 0.0f; g_y = 0.0f;
    g_sx = 1.0f; g_sy = 1.0f;
    g_cosR = 1.0f; g_sinR = 0.0f;
    g_centerX = 0.0f;
    g_r = r; g_g = g; g_b = b; g_a = a;
    // Proportional cap, no glyph legibility boost -- the caller asked for this
    // width in UI units and gets it.
    g_capExt = halfWidth; g_thickBoost = 1.0f;
    segment(x1, y1, x2, y2, halfWidth);
    g_capExt = 0.3f; g_thickBoost = 1.7f;   // restore glyph defaults
}

void renderDot(float x, float y, float size, float r, float g, float b, float a) {
    // ---------------------------------------------------------------------
    // DIRECT PATH. This used to be renderGlowLine(x,y,x,y,...) -> segment(),
    // which reaches the same diamond by the scenic route, and the popup text
    // calls it ~1400 times a frame.
    //
    // Measured on OG profile before this change: renderVectorText was 4.28 ms,
    // 70.8% of the `rest` window and 10.8% of the WHOLE CPU budget -- and it is
    // essentially just this function in a loop.
    //
    // What the old route paid per dot, all of it avoidable:
    //   * ~13 global stores in renderGlowLine to force an IDENTITY affine,
    //     plus capExt/thickBoost saved and restored
    //   * a sqrt -- of zero, to discover the segment is degenerate
    //   * 12 x pushVert, each calling activeBuf/activeCount/activeMax (two
    //     ternaries over globals apiece) and then running affine() -- which
    //     for a dot is ALWAYS the identity, so ~16,800 no-op transforms per
    //     frame at 1400 dots.
    //
    // The output is byte-identical: the affine reduces to px*1 - py*0 + 0,
    // exact in IEEE, and the direct path first reproduced the same twelve
    // vertices, in the same order, that the four tri() calls produced -- the
    // centre four times and each rim point twice. That redundancy WAS the fan
    // topology written out longhand; the index buffer below removed it (see
    // the INDEXED note), and the five indexed triangles replay those same four
    // triangles plus one degenerate, so what rasterises is unchanged.
    //
    // ONE deliberate difference, at the overflow boundary only: pushVert drops
    // vertices INDIVIDUALLY when the batch is full, so a dot could be written
    // as a partial diamond. This drops the whole dot instead. That is the
    // better failure and it only occurs past the cap that slotCap() exists to
    // keep us under.
    // ---------------------------------------------------------------------
    // INDEXED: 12 vertices became 6. The diamond has four rim points, so the
    // fan's 5th slot repeats the first and its last triangle collapses -- see
    // the FAN_VERTS note at the top of this file.
    using namespace ts::textc3d;
    Vertex*   buf = activeBuf();
    int&      n   = activeCount();
    if (!buf || n + FAN_VERTS > activeMax()) return;   // all six or none

    // renderGlowLine set g_thickBoost = 1.0 before calling segment(), so
    // thickness == size and the half-extent is size * 0.5 -- same value the
    // old route computed.
    const float s = size * 0.5f;
    // Same ring order as segment()'s degenerate branch: L, T, R, B, then L again.
    const float vx[FAN_VERTS] = { x, x - s, x,     x + s, x,     x - s };
    const float vy[FAN_VERTS] = { y, y,     y + s, y,     y - s, y     };
    const float va[FAN_VERTS] = { a, 0.0f,  0.0f,  0.0f,  0.0f,  0.0f  };
    for (int k = 0; k < FAN_VERTS; ++k) {
        Vertex& v = buf[n++];
        v.pos[0] = vx[k]; v.pos[1] = vy[k]; v.pos[2] = uiDepthValue();
        v.color[0] = r; v.color[1] = g; v.color[2] = b; v.color[3] = va[k];
        v.uv[0] = 0.0f; v.uv[1] = 0.0f;
    }
}

// ---------------------------------------------------------------------------
// renderTexTri — one textured triangle in UI space, straight into the batch.
// Bypasses segment()'s glyph affine entirely: these coordinates and uvs are
// already final.
// ---------------------------------------------------------------------------
void renderTexTri(float x0, float y0, float u0, float v0,
                  float x1, float y1, float u1, float v1,
                  float x2, float y2, float u2, float v2,
                  float r, float g, float b, float a) {
    using namespace ts::textc3d;
    Vertex* tbuf = activeBuf();
    int&    tn   = activeCount();
    if (!tbuf || tn + 3 > activeMax()) return;
    const float px[3] = { x0, x1, x2 }, py[3] = { y0, y1, y2 };
    const float pu[3] = { u0, u1, u2 }, pv[3] = { v0, v1, v2 };
    for (int i = 0; i < 3; ++i) {
        Vertex& vx = tbuf[tn++];
        vx.pos[0] = px[i]; vx.pos[1] = py[i]; vx.pos[2] = 0.0f;
        vx.color[0] = r; vx.color[1] = g; vx.color[2] = b; vx.color[3] = a;
        vx.uv[0] = pu[i];  vx.uv[1] = pv[i];
    }
}
