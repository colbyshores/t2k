// ============================================================================
// vk_resources.cpp — see vk_resources.h.
// ============================================================================

#include "vk_resources.h"
#include "vk_context.h"

#include <cstdio>
#include <cstring>

#include <volk.h>

namespace ts {
namespace vkres {

namespace {

// Pick a memory type satisfying `typeBits` with all of `required` and, if
// possible, `preferred` too.
int findMemoryType(const vkctx::Context& c, uint32_t typeBits,
                   VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred) {
    const auto& mp = c.memProps;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if (!(typeBits & (1u << i))) continue;
        const VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
        if ((f & (required | preferred)) == (required | preferred)) return (int)i;
    }
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if (!(typeBits & (1u << i))) continue;
        const VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
        if ((f & required) == required) return (int)i;
    }
    return -1;
}

bool allocate(vkctx::Context& c, const VkMemoryRequirements& req,
              VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred,
              VkDeviceMemory& out) {
    const int type = findMemoryType(c, req.memoryTypeBits, required, preferred);
    if (type < 0) { std::fprintf(stderr, "[vk] no memory type for 0x%x\n", required); return false; }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = (uint32_t)type;
    if (vkAllocateMemory(c.device, &ai, nullptr, &out) != VK_SUCCESS) {
        std::fprintf(stderr, "[vk] vkAllocateMemory(%llu) failed\n", (unsigned long long)req.size);
        return false;
    }
    return true;
}

} // namespace

// ---- buffers ---------------------------------------------------------------

bool createBuffer(vkctx::Context& c, Buffer& b, VkDeviceSize size,
                  VkBufferUsageFlags usage, bool hostVisible) {
    b = Buffer{};
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(c.device, &bci, nullptr, &b.buf) != VK_SUCCESS) return false;
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(c.device, b.buf, &req);
    const VkMemoryPropertyFlags required = hostVisible
        ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
        : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    const VkMemoryPropertyFlags preferred = hostVisible ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT : 0;
    if (!allocate(c, req, required, preferred, b.mem)) { destroyBuffer(c, b); return false; }
    vkBindBufferMemory(c.device, b.buf, b.mem, 0);
    b.size = size;
    if (hostVisible) {
        if (vkMapMemory(c.device, b.mem, 0, size, 0, &b.mapped) != VK_SUCCESS) { destroyBuffer(c, b); return false; }
    }
    return true;
}

void destroyBuffer(vkctx::Context& c, Buffer& b) {
    if (b.mapped) vkUnmapMemory(c.device, b.mem);
    if (b.buf) vkDestroyBuffer(c.device, b.buf, nullptr);
    if (b.mem) vkFreeMemory(c.device, b.mem, nullptr);
    b = Buffer{};
}

// ---- ring ------------------------------------------------------------------

bool createRing(vkctx::Context& c, Ring& r, VkDeviceSize size) {
    r = Ring{};
    return createBuffer(c, r.buf, size,
                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        true);
}

void destroyRing(vkctx::Context& c, Ring& r) {
    destroyBuffer(c, r.buf);
    r = Ring{};
}

void* ringAlloc(Ring& r, VkDeviceSize size, VkDeviceSize align, VkDeviceSize& offsetOut) {
    VkDeviceSize head = (r.head + align - 1) / align * align;
    if (size == 0 || head + size > r.buf.size) { ++r.overflow; return nullptr; }
    offsetOut = head;
    r.head = head + size;
    if (r.head > r.peak) r.peak = r.head;
    return static_cast<char*>(r.buf.mapped) + head;
}

// ---- images ----------------------------------------------------------------

bool createImage(vkctx::Context& c, Image& im, const ImageDesc& d) {
    im = Image{};
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = d.format;
    ici.extent = {d.width, d.height, 1};
    ici.mipLevels = d.mips;
    ici.arrayLayers = d.layers;
    ici.samples = d.samples;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = d.usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(c.device, &ici, nullptr, &im.img) != VK_SUCCESS) {
        std::fprintf(stderr, "[vk] vkCreateImage %ux%u fmt %d failed\n", d.width, d.height, (int)d.format);
        return false;
    }
    VkMemoryRequirements req; vkGetImageMemoryRequirements(c.device, im.img, &req);
    if (!allocate(c, req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, im.mem)) { destroyImage(c, im); return false; }
    vkBindImageMemory(c.device, im.img, im.mem, 0);
    im.format = d.format; im.width = d.width; im.height = d.height;
    im.layers = d.layers; im.mips = d.mips; im.samples = d.samples; im.aspect = d.aspect;
    im.layout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = im.img;
    vci.viewType = (d.layers > 1 || d.arrayView) ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    vci.format = d.format;
    vci.subresourceRange = {d.aspect, 0, d.mips, 0, d.layers};
    if (vkCreateImageView(c.device, &vci, nullptr, &im.view) != VK_SUCCESS) { destroyImage(c, im); return false; }
    im.arrayView = d.layers > 1 || d.arrayView;
    return true;
}

void destroyImage(vkctx::Context& c, Image& im) {
    if (im.view) vkDestroyImageView(c.device, im.view, nullptr);
    if (im.img) vkDestroyImage(c.device, im.img, nullptr);
    if (im.mem) vkFreeMemory(c.device, im.mem, nullptr);
    im = Image{};
}

VkImageView createMipView(vkctx::Context& c, const Image& im, uint32_t mip) {
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = im.img;
    vci.viewType = im.arrayView ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    vci.format = im.format;
    vci.subresourceRange = {im.aspect, mip, 1, 0, im.layers};
    VkImageView v = VK_NULL_HANDLE;
    vkCreateImageView(c.device, &vci, nullptr, &v);
    return v;
}

bool uploadImage(vkctx::Context& c, Image& im, const void* pixels, size_t bytes) {
    Buffer staging;
    if (!createBuffer(c, staging, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true)) return false;
    std::memcpy(staging.mapped, pixels, bytes);
    VkCommandBuffer cmd = vkctx::beginOneShot(c);
    transitionRaw(cmd, im.img, im.aspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const size_t perLayer = bytes / im.layers;
    for (uint32_t l = 0; l < im.layers; ++l) {
        VkBufferImageCopy bic{};
        bic.bufferOffset = perLayer * l;
        bic.imageSubresource = {im.aspect, 0, l, 1};
        bic.imageExtent = {im.width, im.height, 1};
        vkCmdCopyBufferToImage(cmd, staging.buf, im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    }
    transitionRaw(cmd, im.img, im.aspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vkctx::endOneShot(c, cmd);
    destroyBuffer(c, staging);
    im.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return true;
}

// ---- barriers --------------------------------------------------------------

void transitionRaw(VkCommandBuffer cmd, VkImage img, VkImageAspectFlags aspect,
                   VkImageLayout from, VkImageLayout to,
                   uint32_t baseMip, uint32_t mipCount, uint32_t baseLayer, uint32_t layerCount) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {aspect, baseMip, mipCount, baseLayer, layerCount};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}

void transition(VkCommandBuffer cmd, Image& im, VkImageLayout to,
                VkPipelineStageFlags2 srcStage, VkPipelineStageFlags2 dstStage) {
    if (im.layout == to && to != VK_IMAGE_LAYOUT_GENERAL) {
        // Same layout: still need a memory barrier for write->read hazards.
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = srcStage; mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        mb.dstStageMask = dstStage; mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.memoryBarrierCount = 1; di.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &di);
        return;
    }
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
    b.dstStageMask = dstStage;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
    b.oldLayout = im.layout;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im.img;
    b.subresourceRange = {im.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
    im.layout = to;
}

void barrierColorToSample(VkCommandBuffer cmd) {
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    mb.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1; di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &di);
}

// ---- samplers --------------------------------------------------------------

VkSampler createSampler(vkctx::Context& c, VkFilter filter, VkSamplerAddressMode wrap,
                        bool mipmaps, float maxAniso) {
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = filter;
    sci.minFilter = filter;
    sci.mipmapMode = mipmaps ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = wrap; sci.addressModeV = wrap; sci.addressModeW = wrap;
    sci.maxLod = mipmaps ? VK_LOD_CLAMP_NONE : 0.0f;
    if (maxAniso > 1.0f && c.hasSamplerAnisotropy) {
        sci.anisotropyEnable = VK_TRUE;
        sci.maxAnisotropy = maxAniso < c.props.limits.maxSamplerAnisotropy ? maxAniso : c.props.limits.maxSamplerAnisotropy;
    }
    VkSampler s = VK_NULL_HANDLE;
    vkCreateSampler(c.device, &sci, nullptr, &s);
    return s;
}

} // namespace vkres
} // namespace ts
