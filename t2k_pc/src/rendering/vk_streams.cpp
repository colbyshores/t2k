// ============================================================================
// vk_streams.cpp — per-frame geometry streams over the ring (renderer_vk.h).
// ============================================================================

#include "renderer_vk.h"

#include <cstring>

#include <volk.h>

namespace ts {
namespace vkr {

bool segStreamBegin(Renderer& r, SegStream& s, int cap) {
    s = SegStream{};
    VkDeviceSize off = 0;
    void* p = vkres::ringAlloc(*r.ring, (VkDeviceSize)cap * sizeof(SegInst), 64, off);
    if (!p) return false;
    s.base = static_cast<SegInst*>(p);
    s.offset = off;
    s.cap = cap;
    return true;
}

void segStreamDraw(Renderer& r, const SegStream& s, int first, int count, bool depthTest) {
    if (!s.base || count <= 0) return;
    if (first < 0) first = 0;
    if (first + count > s.count) count = s.count - first;
    if (count <= 0) return;
    vkCmdBindPipeline(r.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipes.seg);
    bindFrameSet(r, r.pipes.layoutSeg);
    vkpipe::setDynamic(r.cmd, {r.width, r.height}, depthTest, false);
    const VkDeviceSize off = s.offset;
    vkCmdBindVertexBuffers(r.cmd, 0, 1, &r.ring->buf.buf, &off);
    // 4 strip vertices per instance; the instance index selects the segment.
    vkCmdDraw(r.cmd, 4, (uint32_t)count, 0, (uint32_t)first);
}

bool triStreamBegin(Renderer& r, TriStream& s, int cap) {
    s = TriStream{};
    VkDeviceSize off = 0;
    void* p = vkres::ringAlloc(*r.ring, (VkDeviceSize)cap * sizeof(TriVert), 64, off);
    if (!p) return false;
    s.base = static_cast<TriVert*>(p);
    s.offset = off;
    s.cap = cap;
    return true;
}

void triStreamDraw(Renderer& r, VkPipeline pipe, VkPipelineLayout layout, const TriStream& s,
                   int first, int count, bool depthTest, bool depthWrite, float premul,
                   VkDescriptorSet texSet) {
    if (!s.base || count <= 0) return;
    if (first < 0) first = 0;
    if (first + count > s.count) count = s.count - first;
    if (count <= 0) return;
    vkCmdBindPipeline(r.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    bindFrameSet(r, layout);
    if (texSet != VK_NULL_HANDLE)
        vkCmdBindDescriptorSets(r.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &texSet, 0, nullptr);
    const float push[4] = {premul, 0.0f, 0.0f, 0.0f};
    vkCmdPushConstants(r.cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), push);
    vkpipe::setDynamic(r.cmd, {r.width, r.height}, depthTest, depthWrite);
    const VkDeviceSize off = s.offset;
    vkCmdBindVertexBuffers(r.cmd, 0, 1, &r.ring->buf.buf, &off);
    vkCmdDraw(r.cmd, (uint32_t)count, 1, (uint32_t)first, 0);
}

void segPush(SegStream& s, const float* a, const float* b, const float* rgba, float halfPx, uint32_t flags) {
    SegInst i;
    i.a[0] = a[0]; i.a[1] = a[1]; i.a[2] = a[2]; i.halfPx = halfPx;
    i.b[0] = b[0]; i.b[1] = b[1]; i.b[2] = b[2]; i.flags = flags;
    for (int k = 0; k < 4; ++k) { i.colA[k] = rgba[k]; i.colB[k] = rgba[k]; }
    s.push(i);
}

void segPushUI(SegStream& s, float x1, float y1, float x2, float y2, float halfPx,
               float r, float g, float b, float a, uint32_t extraFlags) {
    const float p0[3] = {x1, y1, 0.0f};
    const float p1[3] = {x2, y2, 0.0f};
    const float c[4] = {r, g, b, a};
    // UI-space strokes never get the phosphor halo: the shared UI code already
    // layers its own passes (neonLine / neonText), and the bloom supplies the
    // bleed. The halo is for world-space lines only.
    segPush(s, p0, p1, c, halfPx, SEG_UI | SEG_NOHALO | extraFlags);
}

} // namespace vkr
} // namespace ts
