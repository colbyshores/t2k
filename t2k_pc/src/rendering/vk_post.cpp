// ============================================================================
// vk_post.cpp — scene targets, the bloom pyramid and the composite.
//
//   scene (HDR, MSAA) --resolve--> scene --down x N--> bloom mips
//   bloom mips <--up (tent, additive)-- ; composite = scene + bloom*gain
//   + flash, tonemapped, into the presentation image.
//
// The bloom has NO threshold: the picture is light on black, and every
// photon blooms as it would on a phosphor. The first downsample carries a
// soft knee so isolated dim texels (fog-dimmed far geometry) do not smear.
// ============================================================================

#include "renderer_vk.h"
#include "png_write.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include <volk.h>

namespace ts {
namespace vkr {

namespace {

// Bloom look. Presentation constants -- free to tune (Aesthetic Contract).
//
// ENERGY BUDGET, so this never drifts back into a blob (user, 2026-08-25:
// "everything looks like a blob"). Every downsample and every tent upsample
// is energy-preserving, so each pyramid level holds ~1x the scene's light at
// a wider blur. The up chain folds the coarser level into the finer one
// scaled by BLOOM_UP_GAIN, so mip 0 = D0 + g*D1 + g^2*D2 + ... : a geometric
// series with total energy 1/(1-g). With g = 0.55 that is ~2.2x the scene,
// dominated by the TIGHT levels -- a hot skirt on every line that decays into
// a long faint tail, which is what a phosphor does. The composite then takes
// BLOOM_GAIN of that: 0.15 * 2.2 = ~0.33x the scene's light in bloom (0.11 /
// 0.25x read a touch dry next to the user's reference frames once the glow
// wire went analytic; 0.15 was A/B'd against them and kept).
// The first cut summed all seven levels at 1.0 (7x) and composited at 0.40
// (2.8x the scene's own light, wide): a uniform haze that swallowed the line
// cores. Keep the PRODUCT (BLOOM_GAIN / (1 - BLOOM_UP_GAIN)) near 0.3.
constexpr float BLOOM_GAIN      = 0.15f;   // pyramid contribution in the composite
constexpr float BLOOM_UP_GAIN   = 0.55f;   // coarser level's weight folded into the finer one
constexpr float BLOOM_KNEE      = 0.35f;   // firefly knee on the first downsample
constexpr float BLOOM_UP_RADIUS = 1.0f;    // tent radius in texels of the coarser level
constexpr float EXPOSURE        = 1.0f;

void beginPass(VkCommandBuffer cmd, VkImageView view, VkExtent2D extent, uint32_t layers,
               uint32_t viewMask, bool clear, const float* clearRgba) {
    VkRenderingAttachmentInfo col{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    col.imageView = view;
    col.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    col.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    col.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    if (clear) for (int i = 0; i < 4; ++i) col.clearValue.color.float32[i] = clearRgba ? clearRgba[i] : 0.0f;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent};
    ri.layerCount = viewMask ? 1 : layers;
    ri.viewMask = viewMask;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &col;
    vkCmdBeginRendering(cmd, &ri);
}

} // namespace

bool postCreateTargets(Renderer& r) {
    postDestroyTargets(r);
    vkctx::Context& c = r.ctx;
    const uint32_t layers = r.viewCount;

    vkres::ImageDesc d;
    d.width = r.width; d.height = r.height; d.layers = layers;
    d.format = r.hdrFormat;
    d.samples = r.sceneSamples;
    d.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    if (r.sceneSamples != VK_SAMPLE_COUNT_1_BIT && !vkres::createImage(c, r.sceneMs, d)) return false;

    d.format = r.depthFormat;
    d.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    d.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (!vkres::createImage(c, r.sceneDepth, d)) return false;

    d.format = r.hdrFormat;
    d.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    d.samples = VK_SAMPLE_COUNT_1_BIT;
    d.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    d.arrayView = true;   // the post shaders sample sampler2DArray (one layer per view)
    if (!vkres::createImage(c, r.scene, d)) return false;

    // Bloom chain: mip 0 = half resolution.
    d.width = r.width / 2 > 1 ? r.width / 2 : 1;
    d.height = r.height / 2 > 1 ? r.height / 2 : 1;
    d.mips = BLOOM_MIPS;
    d.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!vkres::createImage(c, r.bloom, d)) return false;
    for (int i = 0; i < BLOOM_MIPS; ++i) r.bloomMipViews[i] = vkres::createMipView(c, r.bloom, (uint32_t)i);

    // Descriptor sets: the bloom passes sample the whole chain through a
    // mipmapped sampler and pick the level with textureLod; the composite
    // samples (scene, bloom).
    for (int i = 0; i < BLOOM_MIPS; ++i) {
        if (r.bloomMipSets[i] == VK_NULL_HANDLE)
            r.bloomMipSets[i] = allocTexSet(r, r.pipes.setPost, r.bloom.view, r.sampLinearClampMips, r.bloom.view, r.sampLinearClampMips);
        else
            updateTexSet(r, r.bloomMipSets[i], r.bloom.view, r.sampLinearClampMips, r.bloom.view, r.sampLinearClampMips);
    }
    // The first downsample reads the resolved scene instead.
    if (r.compositeSet == VK_NULL_HANDLE)
        r.compositeSet = allocTexSet(r, r.pipes.setPost, r.scene.view, r.sampLinearClamp, r.bloom.view, r.sampLinearClampMips);
    else
        updateTexSet(r, r.compositeSet, r.scene.view, r.sampLinearClamp, r.bloom.view, r.sampLinearClampMips);

    // Initial layouts.
    VkCommandBuffer cmd = vkctx::beginOneShot(c);
    if (r.sceneMs.img) vkres::transition(cmd, r.sceneMs, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    vkres::transition(cmd, r.sceneDepth, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    vkres::transition(cmd, r.scene, VK_IMAGE_LAYOUT_GENERAL);
    vkres::transition(cmd, r.bloom, VK_IMAGE_LAYOUT_GENERAL);
    vkctx::endOneShot(c, cmd);
    return true;
}

void postDestroyTargets(Renderer& r) {
    vkctx::Context& c = r.ctx;
    for (int i = 0; i < BLOOM_MIPS; ++i) {
        if (r.bloomMipViews[i]) { vkDestroyImageView(c.device, r.bloomMipViews[i], nullptr); r.bloomMipViews[i] = VK_NULL_HANDLE; }
    }
    vkres::destroyImage(c, r.bloom);
    vkres::destroyImage(c, r.scene);
    vkres::destroyImage(c, r.sceneDepth);
    vkres::destroyImage(c, r.sceneMs);
}

void postRun(Renderer& r, float flashEnv, const float flashRgb[3], float fade) {
    VkCommandBuffer cmd = r.cmd;
    const uint32_t viewMask = r.viewMask;
    const uint32_t layers = r.viewCount;

    // The scene image was written by the resolve (or directly); make it
    // readable by the fragment stage. It lives in GENERAL throughout.
    vkres::barrierColorToSample(cmd);

    // ---- downsample chain -------------------------------------------------
    uint32_t w = r.bloom.width, h = r.bloom.height;
    for (int i = 0; i < BLOOM_MIPS; ++i) {
        const uint32_t mw = w >> i ? w >> i : 1, mh = h >> i ? h >> i : 1;
        beginPass(cmd, r.bloomMipViews[i], {mw, mh}, layers, viewMask, false, nullptr);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipes.bloomDown);
        vkpipe::setDynamic(cmd, {mw, mh}, false, false);
        float push[4];
        if (i == 0) {
            // src = scene (full res), lod 0
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipes.layoutPost, 0, 1, &r.compositeSet, 0, nullptr);
            push[0] = BLOOM_KNEE; push[1] = 0.0f;
            push[2] = 1.0f / (float)r.width; push[3] = 1.0f / (float)r.height;
        } else {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipes.layoutPost, 0, 1, &r.bloomMipSets[i - 1], 0, nullptr);
            const uint32_t sw = w >> (i - 1) ? w >> (i - 1) : 1, sh = h >> (i - 1) ? h >> (i - 1) : 1;
            push[0] = 0.0f; push[1] = (float)(i - 1);
            push[2] = 1.0f / (float)sw; push[3] = 1.0f / (float)sh;
        }
        vkCmdPushConstants(cmd, r.pipes.layoutPost, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        vkres::barrierColorToSample(cmd);
    }

    // ---- upsample chain (additive into the finer level) --------------------
    for (int i = BLOOM_MIPS - 2; i >= 0; --i) {
        const uint32_t mw = w >> i ? w >> i : 1, mh = h >> i ? h >> i : 1;
        const uint32_t sw = w >> (i + 1) ? w >> (i + 1) : 1, sh = h >> (i + 1) ? h >> (i + 1) : 1;
        beginPass(cmd, r.bloomMipViews[i], {mw, mh}, layers, viewMask, false, nullptr);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipes.bloomUp);
        vkpipe::setDynamic(cmd, {mw, mh}, false, false);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipes.layoutPost, 0, 1, &r.bloomMipSets[i + 1], 0, nullptr);
        const float push[4] = {BLOOM_UP_GAIN, (float)(i + 1), BLOOM_UP_RADIUS / (float)sw, BLOOM_UP_RADIUS / (float)sh};
        vkCmdPushConstants(cmd, r.pipes.layoutPost, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        vkres::barrierColorToSample(cmd);
    }

    // ---- composite into the presentation image -----------------------------
    vkctx::Context& c = r.ctx;
    if (r.viewCount > 1) {
        // STEREO: both eyes into the 2-layer target in ONE multiview pass --
        // the headset's swapchain image (STEREO_XR) or the dump target
        // (STEREO_HEADLESS). Every screen-space step above ran per layer
        // (bloom mips are layered, the composite reads layer gl_ViewIndex), so
        // nothing here is shared between the eyes: stereo-safe by
        // construction, not by exception.
        VkImage target = VK_NULL_HANDLE;
        VkImageView targetView = VK_NULL_HANDLE;
        if (r.stereoMode == STEREO_XR && r.xrSession && r.xrSession->imageAcquired) {
            target = r.xrSession->images[r.xrSession->imageIndex];
            targetView = r.xrViews[r.xrSession->imageIndex];
        } else if (r.stereoMode == STEREO_HEADLESS) {
            target = r.stereoDump.img;
            targetView = r.stereoDump.view;
        }
        if (target == VK_NULL_HANDLE) return;
        vkres::transitionRaw(cmd, target, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo col{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        col.imageView = targetView;
        col.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        col.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        col.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {r.width, r.height}};
        ri.layerCount = 1;
        ri.viewMask = viewMask;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &col;
        vkCmdBeginRendering(cmd, &ri);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipes.compositeXr);
        vkpipe::setDynamic(cmd, {r.width, r.height}, false, false);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipes.layoutPost, 0, 1, &r.compositeSet, 0, nullptr);
        // Layer override -1: each view reads its own layer. The flash and
        // the fade arrive through the same push block as on the flat screen,
        // already attenuated for VR by the caller (flash_display_vr).
        float push[8] = {BLOOM_GAIN, EXPOSURE * fade, flashEnv, 0.5f,
                         flashRgb ? flashRgb[0] : 1.0f, flashRgb ? flashRgb[1] : 1.0f, flashRgb ? flashRgb[2] : 1.0f,
                         -1.0f};
        vkCmdPushConstants(cmd, r.pipes.layoutPost, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);

        if (!c.headless) {
            // The mirror window shows the LEFT eye, aspect-fitted on black.
            VkImage swap = c.swapImages[c.imageIndex];
            vkres::transitionRaw(cmd, target, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            vkres::transitionRaw(cmd, swap, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkClearColorValue black{};
            VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(cmd, swap, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
            const float sw = (float)c.swapExtent.width, sh = (float)c.swapExtent.height;
            const float ea = (float)r.width / (float)r.height;
            float dw = sw, dh = sw / ea;
            if (dh > sh) { dh = sh; dw = sh * ea; }
            const int32_t x0 = (int32_t)((sw - dw) * 0.5f), y0 = (int32_t)((sh - dh) * 0.5f);
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[1] = {(int32_t)r.width, (int32_t)r.height, 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.dstOffsets[0] = {x0, y0, 0};
            blit.dstOffsets[1] = {x0 + (int32_t)dw, y0 + (int32_t)dh, 1};
            vkCmdBlitImage(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swap, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &blit, VK_FILTER_LINEAR);
            vkres::transitionRaw(cmd, swap, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
            // The runtime requires its image back in COLOR_ATTACHMENT_OPTIMAL.
            vkres::transitionRaw(cmd, target, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        }
        return;
    }
    if (!c.headless) {
        VkImage swap = c.swapImages[c.imageIndex];
        vkres::transitionRaw(cmd, swap, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        // The composite lands in r.outRect: the whole swapchain image in
        // windowed mode, the largest centred 16:9 rect in arcade mode. When
        // the rect does not cover the image the bars are CLEARED to black
        // (every frame -- the swapchain image's previous contents are
        // undefined after present).
        const bool letterbox = r.outRect.offset.x != 0 || r.outRect.offset.y != 0 ||
                               r.outRect.extent.width != c.swapExtent.width ||
                               r.outRect.extent.height != c.swapExtent.height;
        VkRenderingAttachmentInfo col{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        col.imageView = c.swapViews[c.imageIndex];
        col.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        col.loadOp = letterbox ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        col.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        for (int i = 0; i < 4; ++i) col.clearValue.color.float32[i] = (i == 3) ? 1.0f : 0.0f;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, c.swapExtent};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &col;
        vkCmdBeginRendering(cmd, &ri);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipes.composite);
        vkpipe::setDynamicRect(cmd, r.outRect, false, false);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipes.layoutPost, 0, 1, &r.compositeSet, 0, nullptr);
        // The front-end fade scales EXPOSURE: the whole picture -- scene, bloom
        // and flash -- goes to black together.
        float push[8] = {BLOOM_GAIN, EXPOSURE * fade, flashEnv, 0.5f,
                         flashRgb ? flashRgb[0] : 1.0f, flashRgb ? flashRgb[1] : 1.0f, flashRgb ? flashRgb[2] : 1.0f,
                         0.0f /* layer 0 on a flat screen */};
        vkCmdPushConstants(cmd, r.pipes.layoutPost, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        vkres::transitionRaw(cmd, swap, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }
}

bool stereoDumpWrite(Renderer& r, const char* path) {
    if (r.stereoMode != STEREO_HEADLESS || !r.stereoDump.img) {
        std::fprintf(stderr, "[vk] --stereo-dump: not in headless stereo mode\n");
        return false;
    }
    vkctx::Context& c = r.ctx;
    vkctx::waitIdle(c);
    const uint32_t w = r.width, h = r.height;
    const VkDeviceSize layerBytes = (VkDeviceSize)w * h * 4;
    vkres::Buffer stage;
    if (!vkres::createBuffer(c, stage, layerBytes * 2, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)) return false;
    {
        VkCommandBuffer cmd = vkctx::beginOneShot(c);
        vkres::transitionRaw(cmd, r.stereoDump.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy regions[2]{};
        for (uint32_t l = 0; l < 2; ++l) {
            regions[l].bufferOffset = layerBytes * l;
            regions[l].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, l, 1};
            regions[l].imageExtent = {w, h, 1};
        }
        vkCmdCopyImageToBuffer(cmd, r.stereoDump.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stage.buf, 2, regions);
        vkres::transitionRaw(cmd, r.stereoDump.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        vkctx::endOneShot(c, cmd);
    }
    // ONE memcpy off the mapped staging memory, then work in system memory:
    // the staging heap is host-visible device memory (write-combined, read
    // through the BAR), and a per-byte swizzle straight out of it measured
    // 46 s for two 1024^2 layers -- the whole rest of the harness is under 2 s.
    std::vector<uint8_t> host((size_t)layerBytes * 2);
    std::memcpy(host.data(), stage.mapped, host.size());
    vkres::destroyBuffer(c, stage);
    // Side by side: left | right. The target is sRGB-encoded 8-bit, i.e. the
    // display value the composite computed (composite.frag), byte for byte.
    const uint8_t* src = host.data();
    std::vector<uint8_t> sbs((size_t)w * 2 * h * 4);
    const bool bgr = r.xrFormat == VK_FORMAT_B8G8R8A8_SRGB;
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t l = 0; l < 2; ++l) {
            const uint8_t* row = src + layerBytes * l + (size_t)y * w * 4;
            uint8_t* dst = sbs.data() + ((size_t)y * w * 2 + (size_t)l * w) * 4;
            for (uint32_t x = 0; x < w; ++x) {
                dst[x * 4 + 0] = bgr ? row[x * 4 + 2] : row[x * 4 + 0];
                dst[x * 4 + 1] = row[x * 4 + 1];
                dst[x * 4 + 2] = bgr ? row[x * 4 + 0] : row[x * 4 + 2];
                dst[x * 4 + 3] = 255;
            }
        }
    }
    const bool ok = pngWriteRgba8(path, sbs.data(), w * 2, h);
    if (ok) std::printf("[vk] --stereo-dump: wrote %s (%ux%u, left | right)\n", path, w * 2, h);
    return ok;
}

} // namespace vkr
} // namespace ts
