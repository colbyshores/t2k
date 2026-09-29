// ============================================================================
// vk_scene.cpp — the gameplay frame (ts_render_frame) on the Vulkan backend.
//
// Pass order is the 3DS backend's (renderer_c3d.cpp drawScene):
//   stars -> web -> entities -> line entities -> pickups -> web glow wire ->
//   death fade/spirals -> HUD icons -> HUD text -> powerup popup -> shatter.
//
// Every builder call is the SHARED one (grid_geometry, entity_geometry,
// line_geometry, starfield, shatter); this file only turns their output into
// stream vertices. The three renderer-local laws both existing backends carry
// (web colour cycle, glow wire, starfield emission) are ported verbatim from
// the 3DS build with their constants, and marked as such.
// ============================================================================

#include "renderer_vk.h"
#include "ui/demo_overlay.h"   // the attract-mode DEMO wordmark

#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <thread>
#include <cstdlib>

#include <glm/gtc/matrix_transform.hpp>
#include <volk.h>

#include "game/engine.h"
#include "game/camera.h"
#include "game/constants.h"
#include "game/math_lut.h"
#include "rendering/grid_geometry.h"
#include "rendering/entity_geometry.h"
#include "rendering/line_geometry.h"
#include "rendering/rail_geometry.h"
#include "rendering/shatter_frame.h"
#include "rendering/web_palette.h"
#include "rendering/textures.h"
#include "rendering/font.h"
#include "rendering/hud_icons.h"
#include "ui/pause_fx.h"
#include "ui/popup_fx.h"
#include "data/enemy_data.h"

// The GAME OVER melt's viewport and gains. The viewport pair is the UI box's own
// 400x240 reference (vk_title.cpp uses the identical numbers for the title
// chamber) and it is what keeps the swirl aspect-true.
constexpr int   UI_VP_W = 400;
constexpr int   UI_VP_H = 240;

// The melt's two gains are SHARED (gameover_geometry.h GO_PLUME_GAIN /
// GO_INJECT_SCALE) -- they decide whether the plume reads, and a per-backend
// copy is how the two targets diverged here to begin with.


namespace ts {
namespace vkr {

namespace {

constexpr float PIf = 3.14159265f;
constexpr float FOG_START = 1.0f, FOG_END = 45.0f;
// The 3DS's blanket half-width scale on every line_geometry segment
// (renderer_c3d.cpp LINE_HALFPX_SCALE): builders author in "GL-1920 px"
// units, and this is what puts them at the 240-line reference the rest of
// the widths are authored in.
constexpr float LINE_HALFPX_SCALE = 0.35f;
// WEB_SWEEP_PERIOD_S: rendering/web_palette.h (shared).

// ---- level texture means (star tint identity) ------------------------------
// The starfield tints each pool by the mean colour of its level's plane B
// (hue is identity). The reference planes are the CPU DSL's; they are
// evaluated ONCE per boot on a worker so no frame ever waits, exactly as the
// 3DS computes them at upload. Until a set's mean is in, its stars are
// neutral -- the documented calm fallback, not an error.
struct TexMeanJob {
    std::thread worker;
    std::atomic<int> done{0};
    std::atomic<bool> stop{false};   // teardown: finish the current set, skip the rest
    float mean[GRID_NUM_TEX][3];
    std::atomic<bool> valid[GRID_NUM_TEX];
    bool started = false;
};
TexMeanJob g_texMean;

void computeTexMean(const uint8_t* rgba, int w, int h, float out[3]) {
    uint32_t sr = 0, sg = 0, sb = 0, n = 0;
    const size_t px = (size_t)w * h;
    for (size_t i = 0; i < px; i += 4) {
        const uint8_t* s = rgba + i * 4;
        sr += s[0]; sg += s[1]; sb += s[2]; ++n;
    }
    if (!n) { out[0] = out[1] = out[2] = 1.0f; return; }
    float r0 = (float)sr / (float)n, g0 = (float)sg / (float)n, b0 = (float)sb / (float)n;
    float mx = r0 > g0 ? (r0 > b0 ? r0 : b0) : (g0 > b0 ? g0 : b0);
    if (mx < 1.0f) { out[0] = out[1] = out[2] = 1.0f; return; }
    out[0] = 0.45f + 0.55f * (r0 / mx);
    out[1] = 0.45f + 0.55f * (g0 / mx);
    out[2] = 0.45f + 0.55f * (b0 / mx);
}

void texMeanStart() {
    if (g_texMean.started) return;
    g_texMean.started = true;
    for (int i = 0; i < GRID_NUM_TEX; ++i) g_texMean.valid[i] = false;
    g_texMean.worker = std::thread([]() {
        for (int set = 0; set < GRID_NUM_TEX; ++set) {
            if (g_texMean.stop) break;
            auto planes = generateLevelTextures(set, 0);
            const int W = 256, H = 256;   // generateLevelTextures planes are 256x256 RGBA8
            computeTexMean(planes.second.data(), W, H, g_texMean.mean[set]);
            g_texMean.valid[set] = true;
        }
        g_texMean.done = 1;
    });
}

// ---- helpers ------------------------------------------------------------------

inline float fogOf(const glm::mat4& view, float x, float y, float z) {
    const float vz = view[0][2] * x + view[1][2] * y + view[2][2] * z + view[3][2];
    float ff = (-vz - FOG_START) / (FOG_END - FOG_START);
    return ff < 0.0f ? 0.0f : (ff > 1.0f ? 1.0f : ff);
}

// One quad (two triangles) into a tri stream.
inline void pushQuad(TriStream& s, const float (*p)[3], const float* col, const float (*uv)[2], uint32_t flags) {
    static const int idx[6] = {0, 1, 2, 0, 2, 3};
    for (int k = 0; k < 6; ++k) {
        TriVert v;
        const int i = idx[k];
        v.pos[0] = p[i][0]; v.pos[1] = p[i][1]; v.pos[2] = p[i][2];
        v.col[0] = col[0]; v.col[1] = col[1]; v.col[2] = col[2]; v.col[3] = col[3];
        v.uv[0] = uv ? uv[i][0] : 0.0f; v.uv[1] = uv ? uv[i][1] : 0.0f;
        v.flags = flags;
        s.push(v);
    }
}

struct Xform2D {
    float a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;
    void apply(float x, float y, float& ox, float& oy) const { ox = a * x + b * y + tx; oy = c * x + d * y + ty; }
};
inline Xform2D xfTranslate(Xform2D m, float x, float y) { m.tx += m.a * x + m.b * y; m.ty += m.c * x + m.d * y; return m; }
inline Xform2D xfScale(Xform2D m, float sx, float sy) { m.a *= sx; m.c *= sx; m.b *= sy; m.d *= sy; return m; }
inline Xform2D xfRotateDeg(Xform2D m, float deg) {
    const float r = deg * PIf / 180.0f, cs = std::cos(r), sn = std::sin(r);
    Xform2D o = m;
    o.a = m.a * cs + m.b * sn; o.b = -m.a * sn + m.b * cs;
    o.c = m.c * cs + m.d * sn; o.d = -m.c * sn + m.d * cs;
    return o;
}

} // namespace

// Joined at renderer teardown: a joinable std::thread destroyed at process
// exit calls std::terminate ("terminate called without an active exception"
// on every quit), and the worker must not outlive the generator's statics.
void texMeanJoin() {
    // Stop first: a set the game no longer needs is not worth generating on
    // the way out (all 20 took ~75 s on a loaded machine, and the headless
    // harnesses quit after a few seconds of frames).
    g_texMean.stop = true;
    if (g_texMean.worker.joinable()) g_texMean.worker.join();
}

// ---- texgen -------------------------------------------------------------------

bool texgenEnsure(Renderer& r, TexgenSlot& slot) {
    if (slot.a.img) return true;
    vkres::ImageDesc d;
    d.width = d.height = TEXGEN_RES;
    d.format = r.texgenFormat;
    d.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
              VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!vkres::createImage(r.ctx, slot.a, d)) return false;
    if (!vkres::createImage(r.ctx, slot.b, d)) return false;
    VkCommandBuffer cmd = vkctx::beginOneShot(r.ctx);
    vkres::transition(cmd, slot.a, VK_IMAGE_LAYOUT_GENERAL);
    vkres::transition(cmd, slot.b, VK_IMAGE_LAYOUT_GENERAL);
    vkctx::endOneShot(r.ctx, cmd);
    slot.set2 = allocTexSet(r, r.pipes.setTex2, slot.a.view, r.sampLinearRepeat, slot.b.view, r.sampLinearRepeat);
    slot.setA = allocTexSet(r, r.pipes.setTex1, slot.a.view, r.sampLinearRepeat);
    slot.texSet = -1;
    return true;
}

// Render both planes of `texSet` into the slot (or the pickup sprite into
// r.bonusTex when bonusMode). Must run OUTSIDE the scene pass.
void texgenRun(Renderer& r, TexgenSlot& slot, int texSet, float timeS, float rms, bool bonusMode) {
    if (!r.frameOpen || r.sceneOpen) return;
    VkCommandBuffer cmd = r.cmd;
    vkres::Image& ia = bonusMode ? r.bonusTex : slot.a;
    vkres::Image& ib = bonusMode ? r.bonusTex : slot.b;   // bonus: both outputs to the same image is illegal
    VkRenderingAttachmentInfo col[2]{};
    for (int i = 0; i < 2; ++i) {
        col[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        col[i].imageView = i == 0 ? ia.view : ib.view;
        col[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        col[i].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        col[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {ia.width, ia.height}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = bonusMode ? 1 : 2;
    ri.pColorAttachments = col;
    if (bonusMode) {
        // The texgen pipeline has two colour outputs; a one-attachment pass
        // needs its own pipeline variant. The bonus sprite is generated once
        // through the two-output path into a scratch pair instead.
        TexgenSlot& scratch = r.texgen[1];
        col[0].imageView = scratch.a.view;
        col[1].imageView = scratch.b.view;
        ri.colorAttachmentCount = 2;
        ri.renderArea = {{0, 0}, {scratch.a.width, scratch.a.height}};
    }
    vkCmdBeginRendering(cmd, &ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipes.texgen);
    vkpipe::setDynamic(cmd, {ri.renderArea.extent.width, ri.renderArea.extent.height}, false, false);
    const float push[8] = {(float)texSet, timeS, 0.35f, (float)TEXGEN_RES,
                           bonusMode ? 1.0f : 0.0f, rms, 0.0f, 0.0f};
    vkCmdPushConstants(cmd, r.pipes.layoutTexgen, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    if (bonusMode) {
        // Blit the scratch A plane into the 256^2 bonus texture.
        vkres::barrierColorToSample(cmd);
        TexgenSlot& scratch = r.texgen[1];
        vkres::transition(cmd, scratch.a, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vkres::transition(cmd, r.bonusTex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {(int32_t)scratch.a.width, (int32_t)scratch.a.height, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[1] = {(int32_t)r.bonusTex.width, (int32_t)r.bonusTex.height, 1};
        vkCmdBlitImage(cmd, scratch.a.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r.bonusTex.img,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        vkres::transition(cmd, scratch.a, VK_IMAGE_LAYOUT_GENERAL);
        vkres::transition(cmd, r.bonusTex, VK_IMAGE_LAYOUT_GENERAL);
        scratch.texSet = -1;
        return;
    }
    vkres::barrierColorToSample(cmd);
    slot.texSet = texSet;
    slot.frameGenerated = r.frameCounter;
}

void levelTexMeanEnsure(Renderer& r, int texSet) {
    (void)r; (void)texSet;
    texMeanStart();
}

// ---- the frame ------------------------------------------------------------------

// The pause frost box (ui/pause_fx.h), drawn into the OPEN scene pass at the
// eased ramp t: a dark translucent pane, the melt chamber added back through
// it as the blur, and a hairline border. The menu text lands on top
// afterwards. drawWarp's pause path calls the same helper (with no chamber --
// a bonus round gets no melt), so the two states' boxes cannot drift.
//
// THE BLUR IS THE CHAMBER, NOT A BLOOM MIP. An earlier cut sampled a deep
// bloom mip: it stretched the WHOLE screen into the box (the uv table was the
// full-box composite's), it fed back into itself frame over frame because the
// box draws into the scene the bloom is built from, it read undefined memory
// on the frame after a resize, and the 3DS has no bloom pyramid to copy it
// with. The chamber is already a blurred, moving picture of the surroundings,
// both targets have one, and sampling it at the BOX's own uv sub-rect is what
// makes the glass look like glass.
void drawPauseBox(Renderer& r, float t, Chamber* haze, float boxHW, float boxHH) {
    if (t <= 0.001f || !r.sceneOpen) return;
    const float x0 = BOX_CX - boxHW, x1 = BOX_CX + boxHW;
    const float y0 = BOX_CY - boxHH, y1 = BOX_CY + boxHH;
    static const int idx[6] = {0, 1, 2, 0, 2, 3};
    const float p[4][3] = {{x0, y0, 0.0f}, {x1, y0, 0.0f}, {x1, y1, 0.0f}, {x0, y1, 0.0f}};
    // 1. The pane: straight-alpha dark glass, see-through so the web reads.
    {
        TriStream q;
        if (triStreamBegin(r, q, 6)) {
            for (int k = 0; k < 6; ++k) {
                TriVert v;
                const int i = idx[k];
                v.pos[0] = p[i][0]; v.pos[1] = p[i][1]; v.pos[2] = p[i][2];
                v.col[0] = BOX_RGB[0]; v.col[1] = BOX_RGB[1]; v.col[2] = BOX_RGB[2];
                v.col[3] = BOX_ALPHA * t;
                v.uv[0] = v.uv[1] = 0.0f;
                v.flags = SEG_UI;
                q.push(v);
            }
            triStreamDraw(r, r.pipes.triAlpha, r.pipes.layoutTri, q, 0, q.count, false, false, 0.0f);
        }
    }
    // 2. The haze: the melt chamber sampled at the BOX's own sub-rect, THROUGH
    //    the same keep-out the composite uses -- the glass is frosted where the
    //    melt is behind it and clear where the web is. Sampling it unmasked
    //    reads the loop's saturated fixed point straight through the hole the
    //    mask just cut, and the glass becomes a flat slab.
    if (haze && haze->primed && haze->hist[0].img) {
        static MeltMaskVert hz[MELT_HAZE_VERTS];   // fixed storage, .bss
        const int n = pauseHazeGridBuild(hz, BOX_HAZE_GAIN * t, boxHW, boxHH);
        TriStream q;
        if (triStreamBegin(r, q, n)) {
            for (int k = 0; k < n; ++k) {
                TriVert v;
                v.pos[0] = hz[k].x; v.pos[1] = hz[k].y; v.pos[2] = 0.0f;
                v.col[0] = v.col[1] = v.col[2] = hz[k].gain; v.col[3] = 1.0f;
                v.uv[0] = hz[k].u; v.uv[1] = hz[k].v;
                v.flags = SEG_UI;
                q.push(v);
            }
            triStreamDraw(r, r.pipes.triTexAdd, r.pipes.layoutTriTex, q, 0, q.count,
                          false, false, 0.0f, haze->histSet[haze->flip ^ 1]);
        }
    }
    // 3. The hairline border: one cool line around the glass, brighter at the
    //    top edge to seat it in the scene's light.
    {
        SegStream sg;
        if (segStreamBegin(r, sg, 8)) {
            // The PC's edge table (pause_fx.h). Half-width in reference pixels.
            // NB the 3DS does NOT read it -- see the note there; only
            // BOX_EDGE_ALPHA is shared, and the half-thickness differs ~1.8x.
            const float a = BOX_EDGE_ALPHA * t;
            const float hp = BOX_EDGE_HALF * 240.0f;
            const float ex[4][4] = {{x0, y1, x1, y1}, {x0, y0, x1, y0},
                                    {x0, y0, x0, y1}, {x1, y0, x1, y1}};
            for (int e = 0; e < 4; ++e)
                segPushUI(sg, ex[e][0], ex[e][1], ex[e][2], ex[e][3], hp,
                          BOX_EDGE_RGB[e][0], BOX_EDGE_RGB[e][1], BOX_EDGE_RGB[e][2],
                          a * BOX_EDGE_MUL[e]);
            segStreamDraw(r, sg, 0, sg.count, false);
        }
    }
}

// The pause overlay text (the shared Menu's rows), CPU-staged by
// ts_render_pause_menu_begin and flushed here, on top of the frost box.
// Shared with drawWarp's pause path (renderer_vk.h).
//
// THE RAMP IS APPLIED HERE, NOT AT EMISSION. The staged pool survives the
// pause closing (nothing clears it until the next ts_render_pause_menu_begin),
// so the normal frame can keep drawing it at a falling alpha and the menu
// FADES OUT with the box instead of popping off in one frame. That is also
// why Menu::renderOverlay emits at full alpha: one multiply, one place.
void flushPauseText(Renderer& r, float alpha) {
    if (alpha <= 0.001f) return;
    static const bool dbg = std::getenv("T2K_DEBUG") != nullptr;
    if (dbg && (r.frameCounter % 60) == 0)
        std::fprintf(stderr, "[vk-pause] flushPauseText segs=%d tris=%d a=%.2f\n",
                     r.pauseSegs.count, r.pauseTris.count, alpha);
    if (r.pauseTris.count > 0 && r.pauseTriBase) {
        TriStream q;
        if (triStreamBegin(r, q, r.pauseTris.count)) {
            std::memcpy(q.base, r.pauseTriBase, (size_t)r.pauseTris.count * sizeof(TriVert));
            q.count = r.pauseTris.count;   // memcpy fills the ring; the count is ours to set
            for (int i = 0; i < q.count; ++i) q.base[i].col[3] *= alpha;
            triStreamDraw(r, r.pipes.triAdd, r.pipes.layoutTri, q, 0, q.count, false, false, 1.0f);
        }
    }
    if (r.pauseSegs.count > 0 && r.pauseSegBase) {
        SegStream s;
        if (segStreamBegin(r, s, r.pauseSegs.count)) {
            std::memcpy(s.base, r.pauseSegBase, (size_t)r.pauseSegs.count * sizeof(SegInst));
            s.count = r.pauseSegs.count;
            for (int i = 0; i < s.count; ++i) { s.base[i].colA[3] *= alpha; s.base[i].colB[3] *= alpha; }
            segStreamDraw(r, s, 0, s.count, false);
        }
    }
}

void drawGameplay(Renderer& r, GameEngine& engine) {
    texMeanStart();
    const int texSet = ((engine.current_level % GRID_NUM_TEX) + GRID_NUM_TEX) % GRID_NUM_TEX;
    const float timeS = (float)engine.time * 0.001f;
    // The pause presentation ramp (ui/pause_fx.h): 0 in normal play, so every
    // pause term below vanishes to exactly the frame that always drew.
    const float pauseT = pauseFxEase(engine.pause_fx);
    // Decided HERE, before the starfield is emitted, because the star draw's
    // own dim is gated on it (see starDim below). Same kill-switch as every
    // chamber (engine.warp_feedback); MELT_TEX_PC is the score bed's "PC runs
    // 2x on each axis" precedent applied to the mark-up's 128.
    const bool pauseMelt = pauseT > 0.001f && engine.warp_feedback &&
                           chamberEnsureSized(r, r.chamberPause, MELT_TEX_PC, MELT_TEX_PC);

    // ---- 1. shared grid geometry (the CPU wave IS the oracle path) ---------
    gridgeom::transformLevel(engine, false);
    gridgeom::lightenLevel(engine);
    gridgeom::textureLevel(engine, r.tex1, r.tex2);

    // ---- 2. the level skin, both planes, live --------------------------------
    texgenRun(r, r.texgen[0], texSet, timeS, engine.audio.rms, false);
    static bool bonusDone = false;
    if (!bonusDone) { texgenRun(r, r.texgen[1], 0, 0.0f, 0.0f, true); bonusDone = true; }

    // ---- 3. streams for this frame ---------------------------------------------
    SegStream lines;  segStreamBegin(r, lines, SEG_MAX);
    SegStream glow;   segStreamBegin(r, glow, 4096);
    SegStream ui;     segStreamBegin(r, ui, 32768);
    // THE CHAMBER'S INJECTOR, and it is its own stream because what feeds the
    // loop is not what gets drawn: the wordmark is drawn with straight alpha
    // (its dark body is a mask) but must enter the loop ADDITIVELY, and the
    // echoes are drawn once but injected alongside it. Never submitted.
    SegStream injSegs; segStreamBegin(r, injSegs, 8192);
    TriStream ents;   triStreamBegin(r, ents, 65536);
    TriStream bonus;  triStreamBegin(r, bonus, 16384);
    TriStream uiTris; triStreamBegin(r, uiTris, 16384);
    TriStream shat;   triStreamBegin(r, shat, shatter::SLOTS * shatter::MAX_WORLD_OUT * 6);

    const glm::mat4 view = r.ubo.view[0];
    const float bg0 = engine.bg_color[0], bg1 = engine.bg_color[1], bg2 = engine.bg_color[2];

    // ---- 4. stars (starfield.h; emission law = renderer_c3d build_Stars) ------
    struct StarDraw { int first, count; float push[16]; float tint[3]; };
    StarDraw starDraws[2]; int nStarDraws = 0;
    VkDeviceSize starOff = 0;
    StarInst* starBase = static_cast<StarInst*>(vkres::ringAlloc(*r.ring, sizeof(StarInst) * STAR_MAX, 64, starOff));
    int starCount = 0;
    {
        float R = 0.001f;
        for (const auto& gp : engine.grid_level_pos) {
            const float d = std::sqrt(gp.x * gp.x + gp.y * gp.y);
            if (d > R) R = d;
        }
        if (R < 0.5f) R = 0.5f;
        const float shell = R * 1.9f;
        float dt = (float)(engine.time - r.starLastTime) * 0.001f;
        r.starLastTime = engine.time;
        if (dt < 0.0f || dt > 0.05f) dt = 0.05f;
        const AudioFeatures& a = engine.audio;
        float depth = engine.audio_pulse_k;
        if (engine.audio_safe_mode && depth > 0.4f) depth = 0.4f;

        // Ring hash (FNV over the lane vectors), as the 3DS's gridRingHash.
        uint32_t h = 2166136261u;
        auto mix = [&](uint32_t v) { h ^= v; h *= 16777619u; };
        mix((uint32_t)engine.lane_count); mix(engine.grid_level_go_round ? 1u : 0u);
        for (int i = 0; i < engine.lane_count && i < (int)engine.grid.size(); ++i) {
            uint32_t bx, by; std::memcpy(&bx, &engine.grid[i].dx, 4); std::memcpy(&by, &engine.grid[i].dy, 4);
            mix(bx); mix(by);
        }
        if (h != r.starShapeHash) {
            r.starShapeHash = h;
            const int nRim = (int)engine.grid_level_pos.size();
            if (nRim >= 3 && nRim <= GRID_MAX_ELEMENTS) {
                float rim[GRID_MAX_ELEMENTS * 2];
                for (int i = 0; i < nRim; ++i) { rim[i * 2] = engine.grid_level_pos[i].x; rim[i * 2 + 1] = engine.grid_level_pos[i].y; }
                starfield::buildShapeLut(r.starShapeB, rim, nRim, engine.grid_level_go_round);
            } else {
                r.starShapeB.valid = false;
            }
            starfield::update(r.starFieldB, engine.current_level, 0.0f, 0.0f, 0.0f, &r.starShapeB);
            r.starBlendTotalFrames = tstrans::STAR_BLEND_FRAMES;
            r.starBlendFramesLeft = tstrans::STAR_BLEND_FRAMES;
        }
        const float env = camera_star_env(engine);
        const float rush = camera_star_rush(engine);
        const float flashBoost = camera_flash(engine) * 2.5f;
        if (env > 0.02f) {
            if (!r.starAudioHeld) { r.starAudioHeld = true; r.starHeldRms = a.rms; r.starHeldBeat = a.beat; r.starHeldTreble = a.treble; }
        } else r.starAudioHeld = false;
        float rmsFx = a.rms, beatFx = a.beat, trebleFx = a.treble;
        if (r.starAudioHeld) {
            const float live = 1.0f - env;
            rmsFx = r.starHeldRms * env + a.rms * live;
            beatFx = r.starHeldBeat * env + a.beat * live;
            trebleFx = r.starHeldTreble * env + a.treble * live;
        }
        bool blending = false; float blendT = 0.0f;
        if (r.starBlendFramesLeft > 0) {
            blending = true;
            blendT = 1.0f - (float)r.starBlendFramesLeft / (float)r.starBlendTotalFrames;
            const starfield::ShapeLut* shpA = (engine.player.out_animation > 0) ? nullptr : &r.starShape;
            starfield::update(r.starField, r.starField.builtLevel, dt, rmsFx * depth, beatFx * depth, shpA, rush);
            starfield::update(r.starFieldB, r.starFieldB.builtLevel, dt, rmsFx * depth, beatFx * depth, &r.starShapeB, rush);
            if (--r.starBlendFramesLeft == 0) {
                r.starField = r.starFieldB;
                r.starShape = r.starShapeB;
                starfield::invalidate(r.starFieldB);
            }
        } else {
            const starfield::ShapeLut* shp = (engine.player.out_animation > 0) ? nullptr : &r.starShape;
            starfield::update(r.starField, engine.current_level, dt, rmsFx * depth, beatFx * depth, shp, rush);
        }
        const float whiteMix = env * tstrans::STAR_WHITE_MAX;
        float streakPx = (rush - 1.0f) * 3.4f; if (streakPx > 42.0f) streakPx = 42.0f;
        const float zOff = camera_star_anchor_z(engine);
        // THE SAME ANIMATED BASE THE TUBE IS LIT WITH -- one shared law
        // (web_palette.h webBaseAnim), the parity twin of renderer_c3d.cpp's
        // call. Exact integer phase: this is the build an arcade cabinet runs,
        // so it is the one that sits at the uptimes where the float form
        // stepped and pulled the star tint away from the tube it matches.
        float animR, animG, animB;
        ts::webBaseAnim((uint32_t)engine.time, animR, animG, animB);
        auto levelTint = [&](int lvl, float out[3]) {
            out[0] = animR; out[1] = animG; out[2] = animB;
            const int ts = ((lvl % GRID_NUM_TEX) + GRID_NUM_TEX) % GRID_NUM_TEX;
            if (lvl >= 0 && g_texMean.valid[ts]) { out[0] *= g_texMean.mean[ts][0]; out[1] *= g_texMean.mean[ts][1]; out[2] *= g_texMean.mean[ts][2]; }
        };
        const float pulseA = r.starField.pulse, pulseB = r.starFieldB.pulse;
        const float nearSpan = tstrans::STAR_NEAR_FADE_REST + (rush - 1.0f) * tstrans::STAR_NEAR_FADE_RUSH;
        const float pxScale = (float)r.height / 240.0f;
        // The pause crossfade: as the surroundings melt into the chamber, the
        // direct star draw dims toward PAUSE_STAR_KEEP -- not to nothing, so a
        // crisp head still sits at the tip of every smear, which is what makes
        // the bed read as MOTION rather than as a blur. GATED on the chamber
        // actually running: with warp_feedback off (a real, persisted config
        // key) dimming the field would take light away and put nothing in its
        // place -- the dead-knob trap this codebase has paid for before.
        const float starDim = pauseMelt ? pauseStarDim(pauseT) : 1.0f;
        auto emitField = [&](const starfield::Field& f, float lo, float hi, float pulse) {
            if (!starBase) return;
            StarDraw& sd = starDraws[nStarDraws];
            sd.first = starCount;
            for (int i = 0; i < f.activeCount && starCount < STAR_MAX; ++i) {
                const starfield::Star& s = f.stars[i];
                StarInst& o = starBase[starCount++];
                o.x = s.x; o.y = s.y; o.z = s.z; o.paletteT = s.paletteT;
                o.seed = s.seed; o.pad0 = o.pad1 = o.pad2 = 0.0f;
            }
            sd.count = starCount - sd.first;
            float tint[3]; levelTint(f.builtLevel, tint);
            float* p = sd.push;
            p[0] = shell * (1.0f + pulse * 0.22f); p[1] = zOff; p[2] = rush; p[3] = streakPx;
            p[4] = whiteMix; p[5] = flashBoost; p[6] = trebleFx * depth; p[7] = pulse * depth;
            p[8] = lo; p[9] = hi; p[10] = nearSpan; p[11] = LINE_HALFPX_SCALE * pxScale;
            p[12] = tint[0] * starDim; p[13] = tint[1] * starDim; p[14] = tint[2] * starDim; p[15] = pulse;
            // The chamber's injector reads the UNDIMMED tint (pause_fx.h's
            // pauseStarDim rule): the direct draw fades and the loop takes its
            // place, so dimming the loop's own source too would make the bed
            // peak mid-ramp and then recede.
            sd.tint[0] = tint[0]; sd.tint[1] = tint[1]; sd.tint[2] = tint[2];
            if (sd.count > 0) ++nStarDraws;
        };
        constexpr float OVER_ONE = 1.0001f;
        if (blending) {
            emitField(r.starField, blendT, OVER_ONE, pulseA);
            emitField(r.starFieldB, 0.0f, blendT, pulseB);
        } else {
            emitField(r.starField, 0.0f, OVER_ONE, pulseA);
        }
    }

    // ---- 5. the web: one interleaved stream from the shared builders ------------
    VkDeviceSize gridOff = 0, gridIdxOff = 0;
    int gridVerts = 0, gridIdx = 0;
    {
        const int cols = engine.lane_count * GRID_LOD_X;
        const int nv = (cols + 1) * GRID_STRIDE;
        GridVert* gv = static_cast<GridVert*>(vkres::ringAlloc(*r.ring, sizeof(GridVert) * nv, 64, gridOff));
        const int ni = (int)engine.face_indices.size() * 6;
        uint32_t* gi = static_cast<uint32_t*>(vkres::ringAlloc(*r.ring, sizeof(uint32_t) * (ni > 0 ? ni : 1), 64, gridIdxOff));
        if (gv && gi && (int)engine.vertex_pos.size() > cols) {
            const float wb = webAudioBrightness(engine.web_brightness, engine.audio.beat, engine.audio.rms,
                                                engine.audio_pulse_k, engine.audio_safe_mode)
                           * engine.web_band_additive;   // per-band dim: sky/jade wash out additive enemies
            // -- the arcade web colour cycle (renderer_c3d.cpp, verbatim law) --
            const bool cyc = engine.web_color_cycle;
            const WebHueBatch& hb = currentWebColorBatch(engine.current_level);
            constexpr float WAVE_PERIOD_S = 512.0f / 60.0f;
            const float wavePhase = (float)engine.time * 0.001f / WAVE_PERIOD_S * 6.2831853f;
            // Closed (go_round) webs need a full 2pi span so the wave is
            // loop-periodic across the join; open webs keep the pi half-cycle
            // end-to-end travel. See renderer_c3d wave path for the derivation.
            const float waveLaneStep = (engine.grid_level_go_round ? 2.0f * PIf : PIf)
                                   / (float)engine.lane_count;
            constexpr float VAL_NEAR = 1.00f, VAL_FAR = 0.45f;
            constexpr float LIGHT_FLOOR = 0.55f, LIGHT_PERIOD_S = 23.0f;
            const float lightAngle = (float)engine.time * 0.001f * (6.2831853f / LIGHT_PERIOD_S);
            const float lightX = ts::fastCos(lightAngle), lightY = ts::fastSin(lightAngle);
            float laneR[GRID_MAX_ELEMENTS], laneG[GRID_MAX_ELEMENTS], laneB[GRID_MAX_ELEMENTS], laneLight[GRID_MAX_ELEMENTS];
            float depthVal[GRID_STRIDE];
            const float cycKc = engine.web_cycle_k > 1.0f ? 1.0f : engine.web_cycle_k;
            if (cyc) {
                const int nl = std::min(engine.lane_count, (int)GRID_MAX_ELEMENTS);
                for (int lane = 0; lane < nl; ++lane) {
                    const float hueT = 0.5f + 0.5f * ts::fastSin(wavePhase - (float)lane * waveLaneStep);
                    const float hue = hb.h0 + (hb.h1 - hb.h0) * hueT;
                    const float sat = hb.s0 + (hb.s1 - hb.s0) * hueT;
                    webHsv2rgb(hue, sat, 1.0f, laneR[lane], laneG[lane], laneB[lane]);
                    const auto& nrm = engine.grid_level_normal[lane];
                    const float ndotl = nrm.x * lightX + nrm.y * lightY;
                    laneLight[lane] = LIGHT_FLOOR + (1.0f - LIGHT_FLOOR) * (ndotl > 0.0f ? ndotl : 0.0f);
                }
                for (int vz = 0; vz < GRID_STRIDE; ++vz)
                    depthVal[vz] = VAL_NEAR + (VAL_FAR - VAL_NEAR) * ((float)vz / (float)GRID_LOD_Z);
            }
            for (int vg = 0; vg <= cols; ++vg) {
                // Terminal column: open web = true free end (last lane); closed
                // (go_round) web = duplicate of column 0, so lane 0's colour,
                // else two coincident columns get different hues and seam the join.
                const int rawLane = vg / GRID_LOD_X;
                const int lane = rawLane < engine.lane_count
                              ? rawLane
                              : (engine.grid_level_go_round ? 0 : engine.lane_count - 1);
                const float lr = cyc ? laneR[lane] : 0.0f, lg = cyc ? laneG[lane] : 0.0f;
                const float lb = cyc ? laneB[lane] : 0.0f, ll = cyc ? laneLight[lane] : 0.0f;
                for (int vz = 0; vz < GRID_STRIDE; ++vz) {
                    const int i = vg * GRID_STRIDE + vz;
                    const auto& vp = engine.vertex_pos[vg][vz];
                    const auto& vc = engine.vertex_col[vg][vz];
                    GridVert& o = gv[i];
                    o.base[0] = vp.x; o.base[1] = vp.y; o.base[2] = vp.z;
                    o.normal[0] = 0.0f; o.normal[1] = 0.0f;   // wave already applied by the shared builder
                    o.wave[0] = 0.0f; o.wave[1] = 0.0f;
                    float cr = vc.r, cg = vc.g, cb = vc.b;
                    if (cyc) {
                        const float val = depthVal[vz] * ll;
                        cr += (lr * val - cr) * cycKc; cg += (lg * val - cg) * cycKc; cb += (lb * val - cb) * cycKc;
                    }
                    o.col[0] = cr * wb; o.col[1] = cg * wb; o.col[2] = cb * wb;
                    o.uv0[0] = r.tex1[i * 2]; o.uv0[1] = r.tex1[i * 2 + 1];
                    o.uv1[0] = r.tex2[i * 2]; o.uv1[1] = r.tex2[i * 2 + 1];
                }
            }
            gridVerts = nv;
            const int drawCount = std::min(gridgeom::gridDrawCount(), ni);
            int k = 0;
            for (const auto& f : engine.face_indices) {
                for (int j = 0; j < 6 && k < drawCount; ++j) gi[k++] = (uint32_t)f[j];
                if (k >= drawCount) break;
            }
            gridIdx = k;
        }
    }

    // ---- 6. entities (shared entity_geometry; fog folded per object) -------------
    struct EntSlice { int first, count; bool depthWrite, depthTest; };
    static EntSlice slices[4096];
    int nSlices = 0;
    {
        static entitygeom::EntityDraw draws[4096];
        int nd = entitygeom::buildPlayer(engine, draws, 4096);
        nd += entitygeom::buildEnemies(engine, draws + nd, 4096 - nd);
        for (int i = 0; i < nd && nSlices < 4096; ++i) {
            const entitygeom::EntityDraw& d = draws[i];
            const int need = d.faceCount * 3;
            if (need == 0 || ents.count + need > ents.cap) continue;
            const glm::vec4& wp = d.model[3];
            const float fog = fogOf(view, wp.x, wp.y, wp.z);
            const float omf = 1.0f - fog;
            const int first = ents.count;
            for (int f = 0; f < d.faceCount; ++f) {
                const Face& face = d.faces[f];
                const int vids[3] = {face.v0, face.v1, face.v2};
                for (int j = 0; j < 3; ++j) {
                    const int vi = vids[j];
                    const Vertex2D& vp2 = d.verts[vi];
                    const glm::vec4 w = d.model * glm::vec4((float)vp2.x, (float)vp2.y, 0.0f, 1.0f);
                    float er = 1, eg = 1, eb = 1, ea = 1;
                    if (vi < d.colorCount) {
                        const Color4& c = d.colors[vi];
                        float bb = c.b / 255.0f + d.blueAdd; if (bb > 1.0f) bb = 1.0f;
                        er = c.r / 255.0f; eg = c.g / 255.0f; eb = bb; ea = c.a / 255.0f;
                    }
                    ea *= d.alphaScale;
                    TriVert v;
                    v.pos[0] = w.x; v.pos[1] = w.y; v.pos[2] = w.z;
                    v.col[0] = bg0 * fog + er * omf; v.col[1] = bg1 * fog + eg * omf; v.col[2] = bg2 * fog + eb * omf; v.col[3] = ea;
                    v.uv[0] = v.uv[1] = 0.0f; v.flags = 0;
                    ents.push(v);
                }
            }
            slices[nSlices++] = {first, need, d.depthWrite, d.depthTest};
        }
    }

    // ---- 7. line entities (shared line_geometry::buildAll) ----------------------
    {
        static linegeom::Seg segs[16384];
        const float fov = engine.fov_deg > 1.0f ? engine.fov_deg : tscam::FOV_Y_DEG;
        const float lodPxPerUnit = ((float)r.height * 0.5f) / std::tan(fov * 0.5f * PIf / 180.0f);
        const int n = linegeom::buildAll(engine, segs, 16384, 1.0f / LINE_HALFPX_SCALE, lodPxPerUnit);
        for (int i = 0; i < n; ++i) {
            const linegeom::Seg& s = segs[i];
            SegInst o;
            o.a[0] = s.a[0]; o.a[1] = s.a[1]; o.a[2] = s.a[2]; o.halfPx = s.halfPx * LINE_HALFPX_SCALE;
            o.b[0] = s.b[0]; o.b[1] = s.b[1]; o.b[2] = s.b[2];
            o.flags = SEG_FOG | (s.fullIntensity ? SEG_FULLINTENSITY : 0u);
            for (int k = 0; k < 4; ++k) { o.colA[k] = s.col[k]; o.colB[k] = s.col[k]; }
            if (s.glowPass >= 0) {
                // The arcade enemy border: the shared three-pass stack drawn as
                // ONE analytic profile (the web glow wire's treatment) from
                // pass 0; the flat halo passes are folded into the table
                // (u.glowA2) and skipped here. The builder's alpha carries its
                // pass weight, so divide it back out -- the shader applies it.
                if (s.glowPass != 0) continue;
                o.halfPx = 1.0f;
                o.flags = SEG_FOG | SEG_FULLINTENSITY | SEG_GLOW | SEG_GLOW_ENEMY | SEG_NOHALO;
                const float a = s.col[3] / ARCADE_ENEMY_GLOW_ALPHA[0];
                o.colA[3] = a; o.colB[3] = a;
            }
            lines.push(o);
        }
    }

    // ---- 8. pickups (renderer_c3d build_Bonus, verbatim) ------------------------
    if (!engine.bonuses.empty() && !engine.grid_level_pos.empty() &&
        (int)engine.grid_level_normal.size() >= (int)engine.grid_level_pos.size()) {
        const int N = engine.lane_count;
        const float time = (float)engine.time;
        const float qx[4] = {-0.175f, -0.175f, 0.175f, 0.175f};
        const float qy[4] = {-0.175f, 0.175f, 0.175f, -0.175f};
        const float uv[4][2] = {{0, 0}, {0, 1}, {1, 1}, {1, 0}};
        for (const auto& b : engine.bonuses) {
            const int el = b.grid_element_pos;
            if (el < 0 || el >= N) continue;
            int el2;
            if (b.d < 0.0f) { el2 = el - 1; if (el2 < 0) el2 = engine.grid_level_go_round ? N - 1 : 0; }
            else            { el2 = el + 1; if (el2 >= N) el2 = engine.grid_level_go_round ? 0 : N - 1; }
            const auto& p1 = engine.grid_level_pos[el];   const auto& p2 = engine.grid_level_pos[el2];
            const auto& n1 = engine.grid_level_normal[el]; const auto& n2 = engine.grid_level_normal[el2];
            const float ad = std::fabs(b.d);
            const float bx = (p1.x + n1.x * 0.2f) * (1.0f - ad) + (p2.x + n2.x * 0.2f) * ad;
            const float by = (p1.y + n1.y * 0.2f) * (1.0f - ad) + (p2.y + n2.y * 0.2f) * ad;
            const float t = time + b.max_animation * 64.0f;
            const float tx = bx + b.dx * ts::fastSin((t - b.z * 43) * PIf * 0.00051f);
            const float ty = by + b.dy * ts::fastCos((t - b.z * 41) * PIf * 0.00049f);
            glm::mat4 accum(1.0f);
            accum = glm::translate(accum, glm::vec3(tx, ty, -b.z));
            for (int v2 = 0; v2 < 3; ++v2) {
                const float ang = ts::fastSin((t - b.z * 33) * PIf * 0.00031f) * 122.0f * (v2 + 1);
                accum = glm::rotate(accum, ang * PIf / 180.0f, glm::vec3(0, 0, 1));
                const float sc = 0.85f + ts::fastSin((t - b.z * 37) * PIf * 0.0009f) * 0.15f - v2 * 0.3f;
                const float sc2 = 0.85f + ts::fastCos((t - b.z * 31) * PIf * 0.00096f) * 0.15f - v2 * 0.3f;
                accum = glm::scale(accum, glm::vec3(sc, sc2, 1.0f));
                const float col[4] = {
                    (ts::fastCos((t - b.z * 27) * PIf * 0.00035f) * 0.2f + 0.4f) * (v2 + 1),
                    (ts::fastSin((t - b.z * 24) * PIf * 0.00038f) * 0.2f + 0.4f) * (v2 + 1),
                    (ts::fastSin((t - b.z * 21) * PIf * 0.00042f) * 0.2f + 0.4f) * (v2 + 1), 1.0f};
                float wc[4][3];
                for (int c = 0; c < 4; ++c) {
                    const glm::vec4 w = accum * glm::vec4(qx[c], qy[c], 0.0f, 1.0f);
                    wc[c][0] = w.x; wc[c][1] = w.y; wc[c][2] = w.z;
                }
                pushQuad(bonus, wc, col, uv, 0);
            }
        }
    }

    // ---- 9. the web glow wire (renderer_c3d drawGlowWire, as segments) ----------
    if (engine.web_glow != 0) {
        const float tt = (float)engine.time;
        const int ne = (int)engine.grid_level_pos.size();
        if (ne > 1) {
            const float L = GRID_ELEMENT_LENGTH;
            const bool go_round = engine.grid_level_go_round;
            const int ringLim = ne;
            const int spokeLim = go_round ? ne : ne + 1;
            const WebHueBatch& gb = currentWebColorBatch(engine.current_level);
            const float glowT = 0.5f + 0.5f * ts::fastSin(tt * 0.001f / WEB_SWEEP_PERIOD_S * 6.2831853f);
            float cr, cg, cb;
            webHsv2rgb(gb.h0 + (gb.h1 - gb.h0) * glowT, gb.s0 + (gb.s1 - gb.s0) * glowT, 1.0f, cr, cg, cb);
            const float baseR = cr, baseG = cg, baseB = cb;
            constexpr float LANE_HL_R = 1.0f, LANE_HL_G = 0.90f, LANE_HL_B = 0.05f;
            float bx[GRID_MAX_ELEMENTS + 1], by[GRID_MAX_ELEMENTS + 1];
            gridgeom::borderRing(engine, ne, go_round, bx, by);
            const float wTremor = engine.tremor_strength;
            const float wSc = (engine.screen_flash > 0) ? ts::fastSin((float)engine.screen_flash * 0.05f) * 0.5f : 0.0f;
            const float wDyn = tt * 0.001f - wTremor * 50.0f - wSc;
            // The pickup burst's tube ripple (grid_geometry.h): the border rides
            // it like the surface does, the same shared term the CPU wave adds.
            const gridgeom::WebRipple wRip = gridgeom::webRippleFor(engine);
            auto wv = [&](int v, float z, float& ox, float& oy) {
                ox = bx[v]; oy = by[v];
                float vz = (-z) / L * (float)GRID_LOD_Z; if (vz < 0.0f) vz = 0.0f;
                if (vz < 1e-4f) return;
                const int vn = (v < ne) ? v : (go_round ? 0 : ne - 1);
                const float ph = (vz * 0.1f + (float)(vn * GRID_LOD_X) * 0.066f + wDyn) * PIf;
                const float sq = std::sqrt(vz);
                float disp = ts::fastSin(ph) * wTremor * sq;
                // Ripple column: the surface's own for this vertex (the open
                // web's free end is the terminal column ne * GRID_LOD_X).
                if (wRip.on) {
                    const int rc = (v < ne) ? v * GRID_LOD_X : (go_round ? 0 : ne * GRID_LOD_X);
                    disp += gridgeom::webRippleDisp(wRip, (float)rc, vz, sq);
                }
                ox += engine.grid_level_normal[vn].x * disp;
                oy += engine.grid_level_normal[vn].y * disp;
            };
            auto falloff = [&](float z) { float t = (-z) / L; if (t < 0) t = 0; if (t > 1) t = 1; return 1.0f - 0.8f * t; };
            int hlA = engine.player.grid_element_pos; if (hlA < 0) hlA = 0; if (hlA > ne - 1) hlA = ne - 1;
            int hlB = hlA + 1; if (go_round && hlB >= ne) hlB -= ne;
            // ONE analytic instance per lane edge (SEG_GLOW): the shader sums
            // the shared four-pass shape as Gaussians (u.glowW / u.glowA), so
            // the 3DS's nested quads become one smooth, round-ended profile.
            // halfPx is the width SCALE (1.0 = the reference table as-is).
            {
                const float hpx = 1.0f;
                const float pa = engine.web_glow_k;
                auto seg = [&](float ax, float ay, float az, float bx2, float by2, float bz, bool hl) {
                    SegInst o;
                    o.a[0] = ax; o.a[1] = ay; o.a[2] = az; o.halfPx = hpx;
                    o.b[0] = bx2; o.b[1] = by2; o.b[2] = bz;
                    o.flags = SEG_FOG | SEG_FULLINTENSITY | SEG_GLOW | SEG_NOHALO;
                    const float fa = falloff(az) * pa, fb = falloff(bz) * pa;
                    const float tr = hl ? LANE_HL_R : baseR, tg = hl ? LANE_HL_G : baseG, tb = hl ? LANE_HL_B : baseB;
                    o.colA[0] = tr * fa; o.colA[1] = tg * fa; o.colA[2] = tb * fa; o.colA[3] = 1.0f;
                    o.colB[0] = tr * fb; o.colB[1] = tg * fb; o.colB[2] = tb * fb; o.colB[3] = 1.0f;
                    glow.push(o);
                };
                for (int e = 0; e < 2; ++e) {
                    const float z = -e * L - 0.001f;
                    for (int v2 = 0; v2 < ringLim; ++v2) {
                        float a0x, a0y, b0x, b0y;
                        wv(v2, z, a0x, a0y); wv(v2 + 1, z, b0x, b0y);
                        seg(a0x, a0y, z, b0x, b0y, z, false);
                    }
                }
                for (int v2 = 0; v2 < spokeLim; ++v2) {
                    const bool hl = (v2 == hlA || v2 == hlB);
                    float pz = -L, ppx, ppy; wv(v2, pz, ppx, ppy);
                    for (int s = 1; s <= 3; ++s) {
                        const float z2 = -L + (L - 0.001f) * ((float)s / 3.0f);
                        float nx, ny; wv(v2, z2, nx, ny);
                        seg(ppx, ppy, pz, nx, ny, z2, hl);
                        ppx = nx; ppy = ny; pz = z2;
                    }
                }
            }
        }
    }

    // ---- 10. GAME OVER wordmark (gameover_geometry, shared) --------------------
    // Replaces the death fade + three coloured spirals this block used to carry
    // (user, 2026-09-03: "we will be ripping those colored lines -> fade out").
    // The MATH is in t2k_core so both backends draw the same picture; this is
    // submission only, and the replay order -- FACES then OUTLINES -- is the
    // builder's declared contract, not a local choice.
    // The caller's existing tri range, reused: it is already drawn with the
    // ALPHA pipe just before the UI segs, which gives exactly the builder's
    // declared order -- FACES, then OUTLINES.
    int deathFadeFirst = -1, deathFadeCount = 0;
    // The wordmark's own strokes inside `ui`, so the world fade below can skip
    // them: everything else in that stream withdraws, the words do not.
    int goSegFirst = 0, goSegCount = 0;
    // Gated on the RAMP, not nolives_animation -- the same fix as the 3DS.
    // The ramp starts at the death and counts the dive's ticks first; nolives
    // does not begin until 190 ticks later, so gating on it skips the entire
    // window the sequence lives in and the wordmark pops in at full alpha
    // instead of fading.
    bool goChamberOn = false;
    if (ts::gameoverRamp(engine) > 0.0f) {
        const float ramp = ts::gameoverRamp(engine);
        deathFadeFirst = uiTris.count;
        ts::gameoverStepState(r.goFx, engine);

        ts::gameoverBuild(r.goTris, r.goStrokes, r.goFx, (float)r.height);
        for (int i = 0; i < r.goTris.n; ++i) {
            const ts::GoTri& t = r.goTris.t[i];
            const float col[4] = { t.r, t.g, t.b, t.a };
            TriVert v{};
            v.pos[0] = t.x[0]; v.pos[1] = t.y[0]; std::memcpy(v.col, col, 16); uiTris.push(v);
            v.pos[0] = t.x[1]; v.pos[1] = t.y[1]; std::memcpy(v.col, col, 16); uiTris.push(v);
            v.pos[0] = t.x[2]; v.pos[1] = t.y[2]; std::memcpy(v.col, col, 16); uiTris.push(v);
        }
        deathFadeCount = uiTris.count - deathFadeFirst;
        goSegFirst = ui.count;
        for (int i = 0; i < r.goStrokes.n; ++i) {
            const ts::warpgeom::WarpStroke& k = r.goStrokes.s[i];
            // Half-widths are authored at the 240-line reference (*_REF_H) and
            // the shared builder already scaled them by h/240, so they arrive
            // in this framebuffer's pixels.
            segPushUI(ui, k.x1, k.y1, k.x2, k.y2, k.w, k.r, k.g, k.b, k.a);
            // THE WORDMARK IS THE LOOP'S INJECTOR, exactly as the title screen
            // injects its logo. This is only viable because the wordmark was
            // shrunk to title-logo scale: its SIZE was the entire cause of the
            // saturation, so with that fixed the original intent works with no
            // backdrop at all.
            segPushUI(injSegs, k.x1, k.y1, k.x2, k.y2, k.w, k.r, k.g, k.b, k.a);
        }
        goSegCount = ui.count - goSegFirst;

        // ---- the chamber runs LAST, after its injector is complete ---------
        // Order is load-bearing: injSegs must already hold the wordmark before
        // the pass consumes it. With the pass placed ABOVE the wordmark push,
        // the loop is fed an EMPTY stream every frame and the plume never
        // appears at all -- the failure looks exactly like the chamber being
        // switched off.
        // ---- the MELT CHAMBER ----------------------------------------------
        // THE PC HAD NONE OF THIS. gameoverMeltT and gameoverFeedbackParams are
        // declared in the shared header specifically so both backends could
        // drive one melt, and until now they had exactly one consumer, on the
        // 3DS -- the parity violation the game-over commit's own body admitted
        // ("NOT DONE: the melt chamber as the full-screen background").
        //
        // IT INJECTS THE WORDMARK (the push at the top of this block), exactly
        // as the title screen drives its chamber from its logo. That is the
        // whole arrangement -- there is no backdrop object of any kind.
        // It works because the wordmark is OUTLINE ONLY: a near-black body with
        // a thin neon rim, so the same screen area injects a small fraction of
        // the light the old FILLED gold slab did, and the loop keeps the black
        // it needs to stream trails into. See gameover_geometry.h's
        // GO_TARGET_SPAN_Y block -- the wordmark is 2.8x now, and the size was
        // never the problem on its own.
        // (A rose window was built to supply that black and REJECTED on the
        // console -- docs/design/game_over.md. armature_geometry.{h,cpp} is
        // deleted; do not propose it again.)
        //
        // Chamber law is the TITLE's, not the pause's: FB_DECAY/FB_INJECT give
        // a loop gain of 0.12/0.08 = 1.5 and FB_ZOOM its 4.09x edge reach, where
        // the pause's 0.75/0.10 = 7.5 at a near-identity zoom is precisely the
        // saturated, non-travelling haze being fixed.
        // NB the chamber pass runs AFTER the wordmark has been pushed into
        // injSegs by the goStrokes loop ABOVE -- see the ordering note at the
        // top of this block.
        if (engine.warp_feedback && ts::gameoverMeltT(ramp) > 0.001f
            && chamberEnsure(r, r.chamberGameover)) {
            Chamber& ch = r.chamberGameover;
            if (r.goFx.entered) ch.primed = false;
            const bool safe = engine.audio_safe_mode;
            const float inject = safe ? railgeom::FB_INJECT_SAFE : railgeom::FB_INJECT;
            float rot = 0.0f, wobPhase = 0.0f;
            ts::gameoverFeedbackParams(r.goFx, rot, wobPhase);
            static railgeom::FbMesh goMesh;   // fixed storage, .bss, no growth
            railgeom::warpFeedbackMeshBuild(goMesh, railgeom::FB_ZOOM, rot, wobPhase,
                                            safe ? 0.5f : 1.0f, UI_VP_W, UI_VP_H);
            chamberPass(r, ch, goMesh, railgeom::FB_DECAY,
                        inject * ts::GO_INJECT_SCALE,
                        injSegs.count > 0 ? &injSegs : nullptr, injSegs.count > 0 ? 1 : 0,
                        nullptr, 0);
            goChamberOn = true;
        }

    }

    // ---- 11. HUD icons (renderer_c3d build_InfosIcons, verbatim) ----------------
    const int iconTriFirst = uiTris.count;
    {
        auto tri = [&](float x0, float y0, float x1, float y1, float x2, float y2, const float* c0, const float* c1, const float* c2) {
            TriVert v; v.pos[2] = 0.0f; v.uv[0] = v.uv[1] = 0.0f; v.flags = SEG_UI;
            v.pos[0] = x0; v.pos[1] = y0; std::memcpy(v.col, c0, 16); uiTris.push(v);
            v.pos[0] = x1; v.pos[1] = y1; std::memcpy(v.col, c1, 16); uiTris.push(v);
            v.pos[0] = x2; v.pos[1] = y2; std::memcpy(v.col, c2, 16); uiTris.push(v);
        };
        const float HALFPX_DEFAULT = 0.5f;
        // The claw row: the SHARED builder (rendering/hud_icons.h). One claw
        // per remaining attempt; this backend only submits.
        {
            static ts::hudicons::IconTriPool lifeTris;   // fixed storage, .bss
            lifeTris.n = 0;
            ts::hudicons::buildLifeIcons(lifeTris, engine);
            for (int i = 0; i < lifeTris.n; ++i) {
                const ts::hudicons::IconTri& q = lifeTris.t[i];
                tri(q.x[0], q.y[0], q.x[1], q.y[1], q.x[2], q.y[2], q.col[0], q.col[1], q.col[2]);
            }
        }
        // Warp triangles: the SHARED builder (rendering/hud_icons.h). They now
        // sit on the powerup glyphs' own columns; this backend only submits.
        {
            static ts::hudicons::IconTriPool warpTris;   // fixed storage, .bss
            warpTris.n = 0;
            ts::hudicons::buildWarpIcons(warpTris, engine);
            for (int i = 0; i < warpTris.n; ++i) {
                const ts::hudicons::IconTri& q = warpTris.t[i];
                tri(q.x[0], q.y[0], q.x[1], q.y[1], q.x[2], q.y[2], q.col[0], q.col[1], q.col[2]);
            }
        }
        // Powerup glyphs (zapp/tremor/jump/droid): the SHARED builder
        // (rendering/hud_icons.h) — the art and the animation/colour law live
        // there; this backend only submits the finished segments.
        {
            static ts::hudicons::IconSegPool iconSegs;   // fixed storage, .bss
            iconSegs.n = 0;
            ts::hudicons::buildPowerupIcons(iconSegs, engine);
            for (int i = 0; i < iconSegs.n; ++i) {
                const ts::hudicons::IconSeg& q = iconSegs.s[i];
                segPushUI(ui, q.x1, q.y1, q.x2, q.y2, HALFPX_DEFAULT, q.r, q.g, q.b, q.a);
            }
        }
    }
    const int iconTriCount = uiTris.count - iconTriFirst;

    // ---- 12. HUD text (renderer_c3d HUD block, verbatim) ---------------------
    {
        auto& pl = engine.player;
        fontSetTarget(&r, &ui, &uiTris);
        // The score is ONE CRISP VECTOR LAYER, and nothing behind it. The five faded
        // wobbling layers the mark-up asked to remove are retired, and the
        // melt-o-vision bed that briefly replaced them is retired too (user
        // call: "the clean vector method is correct given that we need every
        // cycle we can for the OG 3DS version" -- the bed cost more 3DS fill
        // than the layers it replaced). The score is now the cheapest thing in
        // the HUD: one string, one pass.
        char sbuf[24];
        std::snprintf(sbuf, sizeof(sbuf), "%d", pl.score);
        const float sx = (float)std::strlen(sbuf) * 0.025f + 0.05f;
        writeAfont(sbuf, sx, 0.925f, 0.042f, 0.042f, 0.0f, 0.15f,
                   1.0f, 1.0f, 1.0f, 0.95f, true, false, 0, 0, 1.0f);

        // ATTRACT: the word DEMO, in the wordmark letterforms. Same shared call
        // and the same position as the C3D twin.
        if (engine.demo_mode)
            ts::demoui::drawDemoWord(engine.time, engine.demo_word_fade);
        // THE UPPER LEFT IS THE SCORE AND THE CLAW COUNT, AND NOTHING ELSE
        // (user call 2026-09-02). The multiplier readout that used to sit
        // under them is gone: three stippled, wobbling, rotating layers of
        // "x1.00" at a peak alpha of 0.12, 63 screen pixels under the lives
        // numeral on a 1080p monitor and FOURTEEN on the 3DS panel -- close
        // enough at 240 lines to read as one crowded line with the count, and
        // too faint to read at all. The multiplier is still the scoring
        // economy (award_score, and the camera's own wobble law reads it); it
        // simply has no readout.
        //
        // THE CLAW COUNT IS THE CLAWS. The yellow `lives / 5` numeral that
        // used to sit beside them is gone (hud_icons.h LIFE_ICON_MAX): it was
        // the high digit of a base-5 readout, which meant it printed "0" for
        // the whole early game and was overlapped by the first claw while it
        // did. The row draws one claw per attempt now.
    }
    fontSetTarget(nullptr, nullptr, nullptr);

    // ---- 13. the bottom message as particles (ui/popup_fx.h) ------------------
    // Particles strung ALONG the glyph stroke paths -- converge from every
    // bearing, hold, burst outward. One shared law; this backend only draws
    // the dots. It replaces a raster-lattice popup that the desktop drew and
    // the 3DS forked away from on old hardware, so one event was rendered
    // three different ways across three configurations.
    {
        fontSetTarget(&r, &ui, &uiTris);
        const int orgtime = engine.time;
        // Same budget law as the 3DS even though this backend's dot is one SDF
        // capsule rather than twelve vertices: the density a message gets must
        // not depend on which machine is drawing it.
        int liveSlots = 0;
        for (int slot = 0; slot < 2; ++slot) {
            if (engine.powerup_text[slot].empty()) continue;
            if (ts::popupfx::slotLive(orgtime - engine.powerup_text_starttime[slot]))
                ++liveSlots;
        }
        const int slotCap = ts::popupfx::slotCap(liveSlots);
        for (int slot = 0; slot < 2; ++slot) {
            std::string& text = engine.powerup_text[slot];
            if (text.empty()) continue;
            const int age = orgtime - engine.powerup_text_starttime[slot];
            if (!ts::popupfx::slotLive(age)) { text = ""; continue; }
            static ts::popupfx::Dot dots[ts::popupfx::MAX_DOTS];   // .bss
            const int nd = ts::popupfx::build(
                dots, slotCap, text.c_str(), (int)text.size(), age,
                (unsigned)engine.powerup_text_starttime[slot],
                engine.current_level, orgtime);
            for (int i = 0; i < nd; ++i) {
                const ts::popupfx::Dot& d = dots[i];
                renderDot(d.x, d.y, d.size, d.r, d.g, d.b, d.a);
            }
            // THE VECTOR FLASH: the dots ignite into real strokes of the same
            // font, on the same layout, for a moment at the peak.
            float vsx, vsy, vy;
            const float va = ts::popupfx::vectorFlash(age, vsx, vsy, vy);
            if (va > 0.0f && nd > 0) {
                writeAfont(text.c_str(), ts::popupfx::X_CENTER, vy, vsx, vsy,
                           0.0f, ts::popupfx::THICKNESS,
                           dots[0].r, dots[0].g, dots[0].b, va, true, false, 0, 0, 1.0f);
            }
        }
        fontSetTarget(nullptr, nullptr, nullptr);
    }

    // ---- 14. pixel-shatter text as world dots (renderer_c3d build_ShatterWorld) ---
    {
        using namespace shatter;
        EventView ev[SLOTS];
        FrameCtx ctx;
        if (beginShatterFrame(engine, ev, ctx)) {
            if (engine.state == GameState::WARP)
                for (int i = 0; i < SLOTS; ++i) if (warpgeom::warpOwnsEvent(ev[i].text)) ev[i] = {"", -1, 0};
            sync(r.shatterState, ev, SLOTS);
            static WorldDot dots[MAX_WORLD_OUT];
            const float uvq[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
            for (int slot = 0; slot < SLOTS; ++slot) {
                const int n = evalWorld(r.shatterState, slot, engine.time, ctx.beat, engine.audio_safe_mode,
                                        engine.current_level, ctx.anchorZn, engine.yes_beat_ms, engine.yes_beat_count,
                                        dots, MAX_WORLD_OUT);
                for (int i = 0; i < n; ++i) {
                    const WorldDot& d = dots[i];
                    const float wx = ctx.cx + d.tx * ctx.R, wy = ctx.cy + d.ty * ctx.R;
                    const float wz = -d.zn * GRID_ELEMENT_LENGTH;
                    const float rw = d.size * ctx.R;
                    const float q[4][3] = {{wx - rw, wy - rw, wz}, {wx + rw, wy - rw, wz}, {wx + rw, wy + rw, wz}, {wx - rw, wy + rw, wz}};
                    const float col[4] = {d.r, d.g, d.b, d.a};
                    pushQuad(shat, q, col, uvq, 0);
                }
            }
        }
    }

    // ---- UFO RIM LIGHTS (mirror of c3d/06_builders.inc build_ShatterWorld) ---
    // The saucer's music-reactive band, filled into the shared world-space pool
    // by the core builder (entity_geometry.cpp emitAdroidRimDots). Appended to
    // the SAME shatter TriStream so it draws with the same dotTex sprite
    // (triTexAdd + dotSet, line ~1234) -- the round primitive. Runs regardless
    // of whether a celebration fired this frame.
    {
        const float uvq[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        const int rimN = entitygeom::g_rimDotCount;
        for (int i = 0; i < rimN; ++i) {
            const entitygeom::RimDot& d = entitygeom::g_rimDots[i];
            const float rw = d.radius;
            const float q[4][3] = {{d.x - rw, d.y - rw, d.z}, {d.x + rw, d.y - rw, d.z},
                                  {d.x + rw, d.y + rw, d.z}, {d.x - rw, d.y + rw, d.z}};
            const float col[4] = {d.r, d.g, d.b, d.a};
            pushQuad(shat, q, col, uvq, 0);
        }
    }

    // ---- the PAUSE melt-o-vision (ui/pause_fx.h, OUTSIDE the scene pass) -----
    // While pause_fx ramps, the starfield feeds the chamber and the melt lerps
    // in around the crisp web. The loop re-primes from black on entry.
    bool pauseChamber = false;
    if (pauseMelt) {
        if (engine.time - r.pauseLastMs > ENTRY_GAP_MS) r.chamberPause.primed = false;
        r.pauseLastMs = engine.time;
        float zoom = 1.0f, rot = 0.0f, wobPhase = 0.0f, distort = pauseT, lobePhase = 0.0f;
        pauseMeltParams(engine.time, pauseT, zoom, rot, wobPhase, distort, lobePhase);
        if (engine.audio_safe_mode) distort *= 0.5f;
        static railgeom::FbMesh pauseMesh;   // fixed storage, .bss, no growth
        // The melt radiates from the middle of the screen (the web) rather
        // than the bonus round's tunnel anchor, braided into MELT_LOBES exits
        // around the periphery -- the mark-up's "lerps to points around the
        // screen". ONE builder, so the 3DS twin cannot warp differently.
        railgeom::warpFeedbackMeshBuildRect(pauseMesh, 0.0f, 0.0f, railgeom::UI_W, 1.0f,
                                            MELT_FIX_U, MELT_FIX_V, zoom, rot, wobPhase,
                                            distort, MELT_VP_W, MELT_VP_H,
                                            MELT_LOBES, lobePhase, MELT_SWIRL_K);
        const float inject = engine.audio_safe_mode ? PAUSE_MELT_INJECT_SAFE
                                                    : PAUSE_MELT_INJECT;
        // The inject draws the SAME star instances the eye draws, but into a
        // MELT_TEX-wide target, so the core half-width is the chamber-space
        // constant (pause_fx.h MELT_STAR_HALFPX), never the screen's pxScale.
        // params.x already carries the chamber's scale for the streak half.
        float injPush[2][16];
        int injFirst[2], injCount[2], nInj = 0;
        for (int i = 0; i < nStarDraws; ++i) {
            std::memcpy(injPush[nInj], starDraws[i].push, sizeof(injPush[0]));
            float* q = injPush[nInj];
            q[4]  = 0.0f;                                  // whiteMix bypasses the tint
            q[11] = MELT_STAR_HALFPX;                      // chamber pixels, not screen
            q[12] = starDraws[i].tint[0] * inject;         // the UNDIMMED tint
            q[13] = starDraws[i].tint[1] * inject;
            q[14] = starDraws[i].tint[2] * inject;
            injFirst[nInj] = starDraws[i].first;
            injCount[nInj] = starDraws[i].count;
            ++nInj;
        }
        chamberPassUI(r, r.chamberPause, pauseMesh, PAUSE_MELT_DECAY, inject,
                      nullptr, 0, nullptr, 0, uiOrthoMatrix(),
                      starOff, injFirst, injCount, injPush, nInj);
        pauseChamber = true;
    }

    // ---- THE GAME OVER TAKES THE WORLD WITH IT (gameoverWebT, shared) --------
    // THE PC NEVER HAD THIS. gameoverWebT had five call sites, all on the 3DS,
    // so on the desktop the tube, the enemies and the HUD stayed lit underneath
    // the wordmark for the whole screen -- the "live gameplay behind GAME OVER"
    // the game-over commit's own body flagged as not done.
    //
    // The 3DS applies the same factor inside its own build loops (goEntFade /
    // goSegFade); these streams are plain CPU-mapped arrays that every builder
    // has finished writing by this point, so one pass over them here is the
    // same arithmetic in one place instead of eight. The starfield is
    // deliberately NOT faded: it is what warps up to hyperspace and carries the
    // screen after the tube has gone.
    {
        const float goWorld = 1.0f - ts::gameoverWebT(ts::gameoverRamp(engine));
        if (goWorld < 0.999f) {
            const float k = goWorld < 0.0f ? 0.0f : goWorld;
            auto fadeSegs = [&](SegStream& st, int skipFirst, int skipCount) {
                for (int i = 0; i < st.count; ++i) {
                    if (skipCount > 0 && i >= skipFirst && i < skipFirst + skipCount) continue;
                    st.base[i].colA[3] *= k; st.base[i].colB[3] *= k;
                }
            };
            fadeSegs(lines, 0, 0);
            fadeSegs(glow, 0, 0);
            // `ui` holds the wordmark's outline as well as the HUD; the words
            // are the one thing on screen that must NOT withdraw.
            fadeSegs(ui, goSegFirst, goSegCount);
            TriStream* tris[3] = { &ents, &bonus, &shat };
            for (TriStream* st : tris)
                for (int i = 0; i < st->count; ++i) st->base[i].col[3] *= k;
            // The HUD's own tris (claws, warp and powerup glyphs) withdraw with
            // the world; the wordmark's faces must NOT, so only the icon range
            // is touched and deathFadeFirst..+Count is left alone.
            for (int i = iconTriFirst; i < iconTriFirst + iconTriCount; ++i)
                uiTris.base[i].col[3] *= k;
        }
    }

    // ============================ ISSUE ==========================================
    const float clear[4] = {bg0, bg1, bg2, 1.0f};
    sceneBegin(r, clear);
    VkCommandBuffer cmd = r.cmd;
    const VkExtent2D ext{r.width, r.height};

    // stars
    if (starBase && nStarDraws > 0) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipes.star);
        bindFrameSet(r, r.pipes.layoutStar);
        vkpipe::setDynamic(cmd, ext, false, false);
        vkCmdBindVertexBuffers(cmd, 0, 1, &r.ring->buf.buf, &starOff);
        for (int i = 0; i < nStarDraws; ++i) {
            vkCmdPushConstants(cmd, r.pipes.layoutStar, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, starDraws[i].push);
            vkCmdDraw(cmd, 4, (uint32_t)starDraws[i].count, 0, (uint32_t)starDraws[i].first);
        }
    }
    // The pause melt, composited over the dimming starfield (and under the
    // crisp web): the surroundings lerping into the loop. MASKED, so the
    // clear pool the web sits in is genuinely clear -- see pause_fx.h.
    if (pauseChamber) chamberCompositeMasked(r, r.chamberPause, pauseT * PAUSE_MELT_COMPOSITE);

    // web
    if (gridVerts > 0 && gridIdx > 0
        && ts::gameoverWebT(ts::gameoverRamp(engine)) < 0.996f) {
        const bool translucent = engine.web_transparent;
        float waFactor = translucent ? engine.web_alpha : 1.0f;
        if (waFactor < 0.0f) waFactor = 0.0f; if (waFactor > 1.0f) waFactor = 1.0f;
        // The tube leaves with the world. The grid is a DRAW rather than one of
        // the streams faded above, so it takes the same factor here through its
        // own alpha push constant.
        waFactor *= 1.0f - ts::gameoverWebT(ts::gameoverRamp(engine));
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, translucent ? r.pipes.gridTranslucent : r.pipes.gridOpaque);
        bindFrameSet(r, r.pipes.layoutGrid);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipes.layoutGrid, 1, 1, &r.texgen[0].set2, 0, nullptr);
        const float push[8] = {0.0f, 0.0f, 0.0f, 0.0f, engine.web_tex_bright * engine.web_band_additive, waFactor, 1.0f, 0.0f};
        vkCmdPushConstants(cmd, r.pipes.layoutGrid, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
        vkpipe::setDynamic(cmd, ext, true, !translucent);
        vkCmdBindVertexBuffers(cmd, 0, 1, &r.ring->buf.buf, &gridOff);
        vkCmdBindIndexBuffer(cmd, r.ring->buf.buf, gridIdxOff, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, (uint32_t)gridIdx, 1, 0, 0, 0);
    }

    // entities: one draw per slice run of equal depth flags
    for (int i = 0; i < nSlices;) {
        int j = i;
        while (j < nSlices && slices[j].depthWrite == slices[i].depthWrite && slices[j].depthTest == slices[i].depthTest) ++j;
        const int first = slices[i].first, count = slices[j - 1].first + slices[j - 1].count - first;
        triStreamDraw(r, r.pipes.triAdd, r.pipes.layoutTri, ents, first, count, slices[i].depthTest, slices[i].depthWrite, 1.0f);
        i = j;
    }

    // line entities (depth test on, no write)
    segStreamDraw(r, lines, 0, lines.count, true);

    // pickups
    triStreamDraw(r, r.pipes.triTexScreen, r.pipes.layoutTriTex, bonus, 0, bonus.count, false, false, 0.0f, r.bonusSet);

    // web glow wire
    segStreamDraw(r, glow, 0, glow.count, false);

    // The GAME OVER melt, composited as the BACKGROUND -- the title screen's
    // own path. The wordmark's dark slot then cuts through it, so the words are
    // read against their own darkness rather than against a live loop.
    if (goChamberOn) chamberComposite(r, r.chamberGameover, ts::GO_PLUME_GAIN);

    // death fade (alpha), icons (alpha), then every UI stroke (additive)
    if (deathFadeCount > 0)
        triStreamDraw(r, r.pipes.triAlpha, r.pipes.layoutTri, uiTris, deathFadeFirst, deathFadeCount, false, false, 0.0f);
    if (iconTriCount > 0)
        triStreamDraw(r, r.pipes.triAlpha, r.pipes.layoutTri, uiTris, iconTriFirst, iconTriCount, false, false, 0.0f);
    segStreamDraw(r, ui, 0, ui.count, false);

    // shatter dots (world, additive, dot sprite) -- BEFORE the pause box, or a
    // celebration caught mid-flight punches through the glass and the rows.
    triStreamDraw(r, r.pipes.triTexAdd, r.pipes.layoutTriTex, shat, 0, shat.count, false, false, 1.0f, r.dotSet);

    // The pause frost box over the web, then the menu overlay text on top of
    // it (the menu's rows were CPU-staged before this frame ran). Both ride
    // the ramp, so the un-pause fades them out together on the normal frame.
    if (pauseT > 0.001f) {
        drawPauseBox(r, pauseT, pauseChamber ? &r.chamberPause : nullptr,
                     pauseBoxHW(engine.pause_box_hw),
                     pauseBoxHH(engine.pause_box_hh));
        flushPauseText(r, pauseT);
    }

    // T2K_DEBUG=1: one line per second of the frame's vital signs.
    static const bool dbg = std::getenv("T2K_DEBUG") != nullptr;
    if (dbg && (r.frameCounter % 60) == 0) {
        const Vec3 eye = camera_eye(engine);
        std::fprintf(stderr, "[vk-dbg] t=%d eye=(%.2f %.2f %.2f) camWebZ=%.2f vel=%.3f init_anim=%d hold=%d ready=%d "
                             "grid v=%d i=%d ents=%d lines=%d glow=%d stars=%d ui=%d tris=%d ring=%llu/%llu drop=%u\n",
                     engine.time, eye.x, eye.y, eye.z, engine.cam_web_z, engine.cam_web_vel,
                     engine.player.init_animation, engine.level_hold_ticks, (int)engine.level_assets_ready,
                     gridVerts, gridIdx, ents.count, lines.count, glow.count, starCount, ui.count, uiTris.count,
                     (unsigned long long)r.ring->head, (unsigned long long)r.ring->buf.size, r.ring->overflow);
    }
}

} // namespace vkr
} // namespace ts
