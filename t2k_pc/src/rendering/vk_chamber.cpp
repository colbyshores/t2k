// ============================================================================
// vk_chamber.cpp — the feedback chamber (renderer_vk.h contract).
//
// The Vulkan twin of the 3DS backend's fbEnsure / fbBuildVerts /
// fbChamberPass / fbComposite (renderer_c3d.cpp) and the GL oracle's
// two-capture loop: ONE recurrence, rail_geometry.h's FB_* constants, the
// shared 16x12 melt mesh (warpFeedbackMeshBuild, feedbackSafeZoom inside it).
//
//     H_N = decay * warp(H_{N-1}) + inject * geom_N ;   screen = H_N + geom_N
//
// What differs from the 3DS is plumbing, and each difference is a parameter
// rather than a different algorithm (DOCTRINE.md "TARGET PARITY IS POLICY"):
//   * the history is 16-bit NORMALIZED (R16G16B16A16_UNORM, CHAMBER_RES^2 vs
//     the PICA's 128^2 RGBA8; renderer_vk.cpp falls back to SFLOAT16 only on
//     a device that cannot blend into UNORM16). The LSB is 1.5e-5, so the
//     8-bit quantization floor that forces the 3DS's every-3rd-pass 1-LSB
//     residue sweep (FB_SWEEP_*) is NOT reached by anything visible: the
//     freeze threshold (1-FB_DECAY)*v < 0.5 LSB sits at v < 9e-5, a fortieth
//     of one 8-bit step. No sweep is run here.
//     THE STORAGE BOUND IS LOAD-BEARING, and it is why this is not SFLOAT:
//     FB_ZOOM spreads each generation over zoom^2 = 1.08x the area while
//     FB_DECAY removes only 8% of its light, so the loop CONSERVES ~99.5% of
//     the injected light per generation and holds ~30 generations of it on
//     screen at once (~3.6 wordmarks' worth on the title). On the 8-bit
//     targets that series is clamped to 1.0 per channel every pass, which is
//     the ceiling FB_INJECT/FB_DECAY were tuned against; a float history has
//     no ceiling and integrated the title plume past 2.0 (looked at: the
//     wordmark washed to a white band). Normalized storage restores the same
//     bound by the same mechanism -- the write clamps -- not by a new knob.
//   * the melt samples through the SAME seg/tri shaders the scene uses, bound
//     to a FrameUBO VARIANT whose viewport is the chamber (so pxScale, the
//     thin-line floor and the halo reach are all in chamber pixels). The
//     3DS re-projects the mesh's (u*UI_W, v) through its ortho; here the UI
//     ortho is fixed (uiProj), so the sample point becomes chamber uv
//     directly: (u, 1 - v) -- Vulkan images are y-down, the UI box is y-up.
//
// GAIN AND FLOOR (the two D3 knobs), and who applies them:
//   * `inject` is applied HERE, not by the caller: the seg layout has no push
//     block and the frame block carries no gain, so the pass COPIES each
//     inject stream into a fresh ring region with RGB x inject (the 3DS's
//     stage-1 RGB-only MODULATE; alpha is left alone exactly as there) and
//     draws the copy. Callers hand in the same streams they draw on screen at
//     full brightness -- one body, two gains, no second projection.
//   * the width floor FB_MIN_CHAMBER_PX rides the variant UBO's params2.z, the
//     seg shader's own thin-line floor, in CHAMBER pixels: a capsule narrower
//     than the floor is drawn AT the floor with its light scaled down by the
//     ratio (this backend's thin-line law, the same one distant web lines
//     obey). The 3DS clamps its flat quads up without dimming; both exist for
//     the same reason -- a sub-pixel inject flickers frame to frame.
//
// The chamber images live in GENERAL for their whole life (sampled one frame,
// rendered the next, like the texgen planes). They are never cleared by the
// CPU: an unprimed pass runs the melt at decay 0 -- a full-coverage opaque
// draw of history x 0 -- so the loop always bootstraps from black whatever
// the image held. That emergence is the title screen's intro.
// ============================================================================

#include "renderer_vk.h"

#include <cstring>

#include "ui/pause_fx.h"

#include <volk.h>

namespace ts {
namespace vkr {

namespace {

// The 3DS's FB_MIN_CHAMBER_PX (renderer_c3d.cpp): the minimum half-width of
// an injected stroke in chamber pixels. Shared value, chamber-relative unit.
constexpr float FB_MIN_CHAMBER_PX = 1.5f;

// The melt mesh: two triangles per cell, FbMesh order.
constexpr int MELT_VERTS = railgeom::FB_MESH_COLS * railgeom::FB_MESH_ROWS * 6;

inline void meltVert(TriStream& s, const railgeom::FbMeshVert& m, float decay) {
    TriVert v;
    v.pos[0] = m.x; v.pos[1] = m.y; v.pos[2] = 0.0f;
    v.col[0] = v.col[1] = v.col[2] = decay; v.col[3] = 1.0f;
    v.uv[0] = m.u; v.uv[1] = 1.0f - m.v;      // UI y-up -> image y-down
    v.flags = SEG_UI;
    s.push(v);
}

// Begin a single-sample colour-only dynamic rendering pass on one chamber image.
void beginChamberPass(VkCommandBuffer cmd, const vkres::Image& im) {
    VkRenderingAttachmentInfo col{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    col.imageView = im.view;
    col.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    // The melt is full-coverage and opaque, so the previous contents are
    // never read; a clear is the cheapest correct load op and guards the
    // mesh's outer ring against any sub-texel gap at the edge.
    col.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    col.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    for (int i = 0; i < 4; ++i) col.clearValue.color.float32[i] = 0.0f;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {im.width, im.height}};
    ri.layerCount = 1;
    ri.viewMask = 0;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &col;
    vkCmdBeginRendering(cmd, &ri);
}

// Draw one seg stream into the chamber at `inject` gain: copy into a fresh
// ring region with RGB scaled, then draw the copy through chamberInject.
void injectSegs(Renderer& r, const Chamber& ch, const SegStream& s, float inject, VkDeviceSize uboOff) {
    if (!s.base || s.count <= 0) return;
    VkDeviceSize off = 0;
    void* p = vkres::ringAlloc(*r.ring, (VkDeviceSize)s.count * sizeof(SegInst), 64, off);
    if (!p) return;   // ring full: the loop simply gets no new light this frame
    SegInst* dst = static_cast<SegInst*>(p);
    for (int i = 0; i < s.count; ++i) {
        SegInst o = s.base[i];
        o.colA[0] *= inject; o.colA[1] *= inject; o.colA[2] *= inject;
        o.colB[0] *= inject; o.colB[1] *= inject; o.colB[2] *= inject;
        dst[i] = o;
    }
    vkCmdBindPipeline(r.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipes.chamberInject);
    bindFrameSetAt(r, r.pipes.layoutSeg, uboOff);
    vkpipe::setDynamic(r.cmd, {ch.w, ch.h}, false, false);
    vkCmdBindVertexBuffers(r.cmd, 0, 1, &r.ring->buf.buf, &off);
    vkCmdDraw(r.cmd, 4, (uint32_t)s.count, 0, 0);
}

void injectTris(Renderer& r, const Chamber& ch, const TriStream& s, float inject, VkDeviceSize uboOff) {
    if (!s.base || s.count <= 0) return;
    VkDeviceSize off = 0;
    void* p = vkres::ringAlloc(*r.ring, (VkDeviceSize)s.count * sizeof(TriVert), 64, off);
    if (!p) return;
    TriVert* dst = static_cast<TriVert*>(p);
    for (int i = 0; i < s.count; ++i) {
        TriVert o = s.base[i];
        o.col[0] *= inject; o.col[1] *= inject; o.col[2] *= inject;
        dst[i] = o;
    }
    vkCmdBindPipeline(r.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipes.chamberInjectTri);
    bindFrameSetAt(r, r.pipes.layoutTri, uboOff);
    const float push[4] = {1.0f, 0.0f, 0.0f, 0.0f};   // premultiply: additive light
    vkCmdPushConstants(r.cmd, r.pipes.layoutTri, VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(push), push);
    vkpipe::setDynamic(r.cmd, {ch.w, ch.h}, false, false);
    vkCmdBindVertexBuffers(r.cmd, 0, 1, &r.ring->buf.buf, &off);
    vkCmdDraw(r.cmd, (uint32_t)s.count, 1, 0, 0);
}

} // namespace

bool chamberEnsureSized(Renderer& r, Chamber& ch, uint32_t w, uint32_t h) {
    if (ch.hist[0].img && ch.hist[1].img && ch.w == w && ch.h == h) return true;
    if (ch.hist[0].img || ch.hist[1].img) chamberDestroy(r, ch);   // size change re-allocates
    if (!r.ctx.device) return false;
    vkres::ImageDesc d;
    d.width = w; d.height = h;
    d.format = r.chamberFormat;
    d.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    for (int i = 0; i < 2; ++i) {
        if (ch.hist[i].img) continue;
        if (!vkres::createImage(r.ctx, ch.hist[i], d)) {
            chamberDestroy(r, ch);
            return false;
        }
    }
    // GENERAL for life (rendered to and sampled on alternate frames). This is
    // a one-shot submit exactly like texgenEnsure / postCreateTargets, which
    // already run mid-frame-record on resize; the frame's own command buffer
    // is not yet submitted, so ordering is trivially correct.
    {
        VkCommandBuffer cmd = vkctx::beginOneShot(r.ctx);
        vkres::transition(cmd, ch.hist[0], VK_IMAGE_LAYOUT_GENERAL);
        vkres::transition(cmd, ch.hist[1], VK_IMAGE_LAYOUT_GENERAL);
        vkctx::endOneShot(r.ctx, cmd);
    }
    // CLAMP_TO_EDGE (D1): the outward zoom samples inside the box, but the
    // wobble can push an edge vertex a hair outside; repeat would wrap the
    // opposite edge's trail into frame, which in a feedback loop is permanent.
    for (int i = 0; i < 2; ++i) {
        if (ch.histSet[i] == VK_NULL_HANDLE)
            ch.histSet[i] = allocTexSet(r, r.pipes.setTex1, ch.hist[i].view, r.sampLinearClamp);
        else
            updateTexSet(r, ch.histSet[i], ch.hist[i].view, r.sampLinearClamp);
        if (ch.histSet[i] == VK_NULL_HANDLE) { chamberDestroy(r, ch); return false; }
    }
    ch.flip = 0;
    ch.primed = false;
    ch.w = w; ch.h = h;
    return true;
}

bool chamberEnsure(Renderer& r, Chamber& ch) {
    return chamberEnsureSized(r, ch, CHAMBER_RES, CHAMBER_RES);
}

void chamberDestroy(Renderer& r, Chamber& ch) {
    for (int i = 0; i < 2; ++i) vkres::destroyImage(r.ctx, ch.hist[i]);
    // Descriptor sets are pool-owned and live for the renderer's life; a
    // re-created chamber re-points them through updateTexSet.
    const VkDescriptorSet keep0 = ch.histSet[0], keep1 = ch.histSet[1];
    ch = Chamber{};
    ch.histSet[0] = keep0; ch.histSet[1] = keep1;
}

void chamberPassUI(Renderer& r, Chamber& ch, const railgeom::FbMesh& mesh, float decay, float inject,
                   const SegStream* segs, int segCount, const TriStream* tris, int triCount,
                   const glm::mat4& uiProj,
                   VkDeviceSize starOff, const int* starFirst, const int* starCount,
                   const float (*starPush)[16], int starRuns) {
    if (!r.frameOpen || r.sceneOpen) return;
    if (!ch.hist[0].img || !ch.hist[1].img) return;
    VkCommandBuffer cmd = r.cmd;
    const int dst = ch.flip;
    const int src = ch.flip ^ 1;

    // 1. The frame block VARIANT: the chamber's viewport, pxScale and floor.
    //    uiProj is the caller's, so UI-space geometry lands in the chamber
    //    exactly where it lands on screen (the composite quad is the identity;
    //    the pause melt passes the flat UI ortho).
    FrameUBO u = r.ubo;
    const float cw = (float)ch.w, chh = (float)ch.h;
    u.viewport = glm::vec4(cw, chh, 1.0f / cw, 1.0f / chh);
    // The chamber is a FLAT UI-space image whatever the eye's projection is:
    // in stereo the frame block's uiProj is the per-eye perspective onto the
    // UI plane (renderer_vk.cpp fillUbo), which must not reach the loop.
    for (int v = 0; v < MAX_VIEWS; ++v) u.uiProj[v] = uiProj;
    // pxScale at chamber resolution: injected strokes carry half-widths
    // authored at the 240-line reference (the *_REF_H convention), and every
    // chamber left covers the whole UI box. (A sub-rect chamber would need
    // "chamber pixels per REFERENCE pixel" instead -- the score bed's sliver
    // did, and got 1/7 the intended weight until it was given one; it is gone
    // now, so the simple law is exact again.)
    u.params.x = chh / 240.0f;
    // NO PHOSPHOR HALO INSIDE THE LOOP. The halo is the SDF line renderer's
    // screen presentation (seg.frag: an exp skirt ~6 ref px wide around every
    // capsule). Inside a feedback loop that skirt is the worst possible
    // emitter: 20+ chamber px wide, it overlaps its own previous contribution
    // almost fully under FB_ZOOM's ~5 px/frame slide, so it integrates at
    // ~1/(1-0.92*0.8) = 4x and washes the wordmark to white in seconds
    // (looked at: agentchamber_title_6.0s, first run). The 3DS injects flat
    // halo-less quads, and FB_INJECT was tuned against those; the loop gets
    // the same -- core coverage only -- and the halo stays on the eye's draw.
    u.params2.x = 0.0f;                           // halo gain
    u.params2.y = 0.0f;                           // halo reach
    u.params2.z = FB_MIN_CHAMBER_PX;              // the inject width floor
    // Same rule for the analytic strokes (SEG_GLOW): inside the loop only the
    // hot core is injected -- the wide Gaussian skirt would integrate exactly
    // like the halo above. The core Gaussian carries the flat quad's light.
    u.glowA = glm::vec4(u.glowA.x, 0.0f, 0.0f, 0.0f);
    u.glowA2 = glm::vec4(u.glowA2.x, 0.0f, 0.0f, 0.0f);
    const VkDeviceSize uboOff = writeUboVariant(r, u);

    // 2. The melt mesh, this frame's geometry from the shared builder.
    TriStream melt;
    if (!triStreamBegin(r, melt, MELT_VERTS)) return;
    const float gain = ch.primed ? decay : 0.0f;  // unprimed: history x 0 = black
    for (int i = 0; i < railgeom::FB_MESH_ROWS; ++i)
        for (int j = 0; j < railgeom::FB_MESH_COLS; ++j) {
            const railgeom::FbMeshVert& a = mesh.v[i][j];
            const railgeom::FbMeshVert& b = mesh.v[i][j + 1];
            const railgeom::FbMeshVert& c = mesh.v[i + 1][j];
            const railgeom::FbMeshVert& d = mesh.v[i + 1][j + 1];
            meltVert(melt, a, gain); meltVert(melt, c, gain); meltVert(melt, d, gain);
            meltVert(melt, a, gain); meltVert(melt, d, gain); meltVert(melt, b, gain);
        }

    // Cross-frame hazards, both directions: the history written last frame
    // (a previous submission, possibly still in flight) is sampled now, and
    // the image this pass writes was sampled by last frame's composite. A
    // GENERAL->GENERAL image barrier is an all-commands / all-access fence on
    // exactly these two images, which is what transition() emits here.
    vkres::transition(cmd, ch.hist[src], VK_IMAGE_LAYOUT_GENERAL);
    vkres::transition(cmd, ch.hist[dst], VK_IMAGE_LAYOUT_GENERAL);

    // 3. The pass: melt (opaque, x gain) then inject (additive, x inject).
    beginChamberPass(cmd, ch.hist[dst]);
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipes.chamberMelt);
        bindFrameSetAt(r, r.pipes.layoutTriTex, uboOff);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipes.layoutTriTex, 1, 1,
                                &ch.histSet[src], 0, nullptr);
        const float push[4] = {0.0f, 0.0f, 0.0f, 0.0f};   // straight: colour x history
        vkCmdPushConstants(cmd, r.pipes.layoutTriTex, VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(push), push);
        vkpipe::setDynamic(cmd, {ch.w, ch.h}, false, false);
        const VkDeviceSize off = melt.offset;
        vkCmdBindVertexBuffers(cmd, 0, 1, &r.ring->buf.buf, &off);
        vkCmdDraw(cmd, (uint32_t)melt.count, 1, 0, 0);
    }
    for (int i = 0; i < segCount; ++i) if (segs) injectSegs(r, ch, segs[i], inject, uboOff);
    for (int i = 0; i < triCount; ++i) if (tris) injectTris(r, ch, tris[i], inject, uboOff);
    // The starfield inject (the pause melt): the SAME instance ranges the eye
    // draws, through the chamberStar pipe, each run's push block pre-scaled by
    // the inject gain (star.vert's colour scales with it linearly). Every run
    // is injected, not just the first -- during a level cross-dissolve the
    // field is TWO pools and half the melt would otherwise be missing.
    if (starPush && starFirst && starCount && starRuns > 0) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipes.chamberStar);
        bindFrameSetAt(r, r.pipes.layoutStar, uboOff);
        vkpipe::setDynamic(cmd, {ch.w, ch.h}, false, false);
        vkCmdBindVertexBuffers(cmd, 0, 1, &r.ring->buf.buf, &starOff);
        for (int i = 0; i < starRuns; ++i) {
            if (starCount[i] <= 0) continue;
            vkCmdPushConstants(cmd, r.pipes.layoutStar, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, starPush[i]);
            vkCmdDraw(cmd, 4, (uint32_t)starCount[i], 0, (uint32_t)starFirst[i]);
        }
    }
    vkCmdEndRendering(cmd);

    // The scene pass's composite samples what was just written.
    vkres::barrierColorToSample(cmd);
    ch.primed = true;
    ch.flip ^= 1;
}

void chamberPass(Renderer& r, Chamber& ch, const railgeom::FbMesh& mesh, float decay, float inject,
                 const SegStream* segs, int segCount, const TriStream* tris, int triCount) {
    // The flat UI ortho: the fullscreen chambers' identity mapping.
    chamberPassUI(r, ch, mesh, decay, inject, segs, segCount, tris, triCount, uiOrthoMatrix());
}

void chamberCompositeRect(Renderer& r, Chamber& ch, float x0, float y0, float x1, float y1, float gain) {
    if (!r.sceneOpen) return;
    if (!ch.hist[0].img || !ch.hist[1].img || !ch.primed) return;
    const int src = ch.flip ^ 1;               // the history chamberPass just wrote
    TriStream q;
    if (!triStreamBegin(r, q, 6)) return;
    // One UI-space quad over the rect, uv the identity of the chamber's own
    // mapping (y flipped, see meltVert). Pure additive: the chamber already
    // holds premultiplied light, so premul is 0 and the colour is the gain.
    const float p[4][3] = {{x0, y0, 0.0f}, {x1, y0, 0.0f}, {x1, y1, 0.0f}, {x0, y1, 0.0f}};
    const float uv[4][2] = {{0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}, {0.0f, 0.0f}};
    static const int idx[6] = {0, 1, 2, 0, 2, 3};
    for (int k = 0; k < 6; ++k) {
        TriVert v;
        const int i = idx[k];
        v.pos[0] = p[i][0]; v.pos[1] = p[i][1]; v.pos[2] = p[i][2];
        v.col[0] = v.col[1] = v.col[2] = gain; v.col[3] = 1.0f;
        v.uv[0] = uv[i][0]; v.uv[1] = uv[i][1];
        v.flags = SEG_UI;
        q.push(v);
    }
    triStreamDraw(r, r.pipes.triTexAdd, r.pipes.layoutTriTex, q, 0, q.count, false, false, 0.0f, ch.histSet[src]);
}

void chamberComposite(Renderer& r, Chamber& ch, float gain) {
    chamberCompositeRect(r, ch, 0.0f, 0.0f, railgeom::UI_W, 1.0f, gain);
}

void chamberCompositeMasked(Renderer& r, Chamber& ch, float gain) {
    if (!r.sceneOpen) return;
    if (!ch.hist[0].img || !ch.hist[1].img || !ch.primed) return;
    const int src = ch.flip ^ 1;
    static MeltMaskVert mask[MELT_MASK_VERTS];   // fixed storage, .bss
    const int n = pauseMeltMaskBuild(mask, gain);
    TriStream q;
    if (!triStreamBegin(r, q, n)) return;
    for (int i = 0; i < n; ++i) {
        TriVert v;
        v.pos[0] = mask[i].x; v.pos[1] = mask[i].y; v.pos[2] = 0.0f;
        v.col[0] = v.col[1] = v.col[2] = mask[i].gain; v.col[3] = 1.0f;
        v.uv[0] = mask[i].u; v.uv[1] = mask[i].v;
        v.flags = SEG_UI;
        q.push(v);
    }
    triStreamDraw(r, r.pipes.triTexAdd, r.pipes.layoutTriTex, q, 0, q.count, false, false, 0.0f, ch.histSet[src]);
}

void bindFrameSetAt(Renderer& r, VkPipelineLayout layout, VkDeviceSize uboOffset) {
    const uint32_t dyn = (uint32_t)uboOffset;
    vkCmdBindDescriptorSets(r.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1,
                            &r.frameSets[r.ctx.frameIndex], 1, &dyn);
}

VkDeviceSize writeUboVariant(Renderer& r, const FrameUBO& ubo) {
    VkDeviceSize off = 0;
    const VkDeviceSize align = r.ctx.props.limits.minUniformBufferOffsetAlignment > 64
                             ? r.ctx.props.limits.minUniformBufferOffsetAlignment : 64;
    void* p = vkres::ringAlloc(*r.ring, sizeof(FrameUBO), align, off);
    if (!p) return r.frameUboOffset[r.ctx.frameIndex];
    std::memcpy(p, &ubo, sizeof(FrameUBO));
    return off;
}

} // namespace vkr
} // namespace ts
