#pragma once

// ============================================================================
// vk_resources.h — buffers, images, the per-frame streaming ring and the
// small helpers every pass needs (barriers, samplers, uploads).
//
// Memory strategy: one VkDeviceMemory per resource. The renderer owns a few
// dozen resources, all created at init or on resize, so the 4096-allocation
// limit is nowhere near; a sub-allocator would be complexity with no payoff.
//
// The streaming ring is how every per-frame vertex stream reaches the GPU:
// one host-visible, host-coherent buffer per frame-in-flight, bump-allocated
// from zero each frame. That is the Vulkan shape of the 3DS backend's
// `buf[frame&1]` ping-pong (port-docs c3d-frame-pipelining-syncdraw-overlap):
// the frame fence guarantees the GPU finished reading slot N before the CPU
// writes it again. Nothing is ever freed mid-frame and nothing is heap
// allocated on the frame path.
// ============================================================================

#include <stdint.h>
#include <stddef.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

namespace ts {
namespace vkctx { struct Context; }
namespace vkres {

struct Buffer {
    VkBuffer       buf  = VK_NULL_HANDLE;
    VkDeviceMemory mem  = VK_NULL_HANDLE;
    VkDeviceSize   size = 0;
    void*          mapped = nullptr;   // non-null when host-visible
};

struct Image {
    VkImage        img    = VK_NULL_HANDLE;
    VkDeviceMemory mem    = VK_NULL_HANDLE;
    VkImageView    view   = VK_NULL_HANDLE;   // whole-image view (all layers/mips)
    VkFormat       format = VK_FORMAT_UNDEFINED;
    uint32_t       width = 0, height = 0, layers = 1, mips = 1;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageLayout  layout = VK_IMAGE_LAYOUT_UNDEFINED;  // tracked for barriers
    bool           arrayView = false;
};

// ---- buffers ---------------------------------------------------------------
// hostVisible: mapped persistently (HOST_VISIBLE|HOST_COHERENT, preferring a
// DEVICE_LOCAL heap when one is host-visible -- ReBAR / integrated GPUs).
bool createBuffer(vkctx::Context& c, Buffer& b, VkDeviceSize size,
                  VkBufferUsageFlags usage, bool hostVisible);
void destroyBuffer(vkctx::Context& c, Buffer& b);

// ---- streaming ring --------------------------------------------------------
struct Ring {
    Buffer   buf;
    VkDeviceSize head = 0;
    VkDeviceSize peak = 0;     // whole-run high-water mark (ringReset leaves it
                               // alone). Written by ringAlloc, read by nothing:
                               // the [vk-dbg]/[vk-warp] lines log head, buf.size
                               // and overflow only. This one is for a debugger.
    uint32_t overflow = 0;     // allocations refused this frame
};
bool createRing(vkctx::Context& c, Ring& r, VkDeviceSize size);
void destroyRing(vkctx::Context& c, Ring& r);
inline void ringReset(Ring& r) { r.head = 0; r.overflow = 0; }
// Bump-allocate `size` bytes aligned to `align`; returns the mapped pointer
// and writes the buffer offset. Returns nullptr (and counts it) when full --
// the caller draws nothing for that stream this frame rather than crashing.
void* ringAlloc(Ring& r, VkDeviceSize size, VkDeviceSize align, VkDeviceSize& offsetOut);

// ---- images ----------------------------------------------------------------
struct ImageDesc {
    uint32_t width = 1, height = 1, layers = 1, mips = 1;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    // Always create the whole-image view as 2D_ARRAY, even for one layer, so
    // a shader declared against sampler2DArray (the multiview-ready post
    // chain) binds it on a flat screen too.
    bool arrayView = false;
};
bool createImage(vkctx::Context& c, Image& im, const ImageDesc& d);
void destroyImage(vkctx::Context& c, Image& im);
// A view of one mip level (all layers), for render-to-mip in the bloom chain.
VkImageView createMipView(vkctx::Context& c, const Image& im, uint32_t mip);

// Upload tightly packed pixels into mip 0 of every layer (RGBA8 etc.), leaving
// the image in SHADER_READ_ONLY_OPTIMAL. Init-time only (one-shot submit).
bool uploadImage(vkctx::Context& c, Image& im, const void* pixels, size_t bytes);

// ---- barriers --------------------------------------------------------------
// Full-image layout transition with a conservative all-commands scope. Used
// at pass boundaries; the per-pass load/store ops do the real work.
void transition(VkCommandBuffer cmd, Image& im, VkImageLayout to,
                VkPipelineStageFlags2 srcStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                VkPipelineStageFlags2 dstStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
void transitionRaw(VkCommandBuffer cmd, VkImage img, VkImageAspectFlags aspect,
                   VkImageLayout from, VkImageLayout to,
                   uint32_t baseMip = 0, uint32_t mipCount = VK_REMAINING_MIP_LEVELS,
                   uint32_t baseLayer = 0, uint32_t layerCount = VK_REMAINING_ARRAY_LAYERS);
// Make preceding colour-attachment writes visible to later fragment sampling
// without a layout change (the image stays in GENERAL / same layout).
void barrierColorToSample(VkCommandBuffer cmd);

// ---- samplers --------------------------------------------------------------
VkSampler createSampler(vkctx::Context& c, VkFilter filter, VkSamplerAddressMode wrap,
                        bool mipmaps, float maxAniso = 0.0f);

} // namespace vkres
} // namespace ts
