#pragma once

// ============================================================================
// vk_pipeline.h — shader modules from embedded SPIR-V and a small graphics
// pipeline builder for dynamic rendering.
//
// Every pipeline in the renderer uses the same dynamic state set (viewport,
// scissor, depth test/write enable, depth compare) so ONE pipeline per
// shader+blend combination serves every draw; the per-pass depth flags
// the 3DS backend expresses as C3D_DepthTest calls become
// vkCmdSetDepthTestEnable calls here, which is the same shape.
//
// Cull is NOT in that set: it is baked to VK_CULL_MODE_NONE for every pipeline
// (the camera is inside the tube), which is what vk_warp.cpp's strip leans on.
// A pass that ever needs culling therefore needs its OWN pipeline -- there is
// no per-draw call for it, and vkCmdSetCullMode would be undeclared state.
// ============================================================================

#include <stdint.h>
#include <stddef.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

namespace ts {
namespace vkctx { struct Context; }
namespace vkpipe {

VkShaderModule createShader(vkctx::Context& c, const uint32_t* spirv, size_t bytes);

enum Blend {
    BLEND_NONE = 0,          // opaque
    BLEND_ADD,               // ONE, ONE (colour premultiplied by the shader)
    BLEND_ADD_SRC_ALPHA,     // SRC_ALPHA, ONE (the house blend)
    BLEND_PREMUL,            // ONE, ONE_MINUS_SRC_ALPHA
    BLEND_ALPHA,             // SRC_ALPHA, ONE_MINUS_SRC_ALPHA
    BLEND_SCREEN_INV_DST,    // ONE_MINUS_DST_COLOR, ONE (bonus pickups)
    // ONE, ONE, reverse-subtract; alpha left alone (ZERO/ONE). This is the
    // 3DS chamber's residue sweep and NO PC PIPELINE REQUESTS IT: the Vulkan
    // history is UNORM16, whose 1.5e-5 LSB puts the freeze threshold far below
    // one 8-bit step, so FB_SWEEP_* is 3DS-only (rail_geometry.h "residue
    // sweep"; renderer_vk.cpp's chamber-format block).
    BLEND_REVERSE_SUBTRACT,
    BLEND_COUNT
};

struct VertexAttr {
    uint32_t location;
    uint32_t binding;
    VkFormat format;
    uint32_t offset;
};

struct VertexBinding {
    uint32_t binding;
    uint32_t stride;
    bool perInstance;
};

struct Desc {
    VkShaderModule vert = VK_NULL_HANDLE;
    VkShaderModule frag = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    const VertexBinding* bindings = nullptr; uint32_t bindingCount = 0;
    const VertexAttr* attrs = nullptr;       uint32_t attrCount = 0;
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    Blend blend = BLEND_ADD;
    // Attachments for dynamic rendering.
    const VkFormat* colorFormats = nullptr; uint32_t colorCount = 1;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    uint32_t viewMask = 0;      // multiview: 0b11 for two views, 0 for none
    bool depthClamp = false;
};

VkPipeline createGraphics(vkctx::Context& c, const Desc& d);

// Descriptor-set-layout helpers.
VkDescriptorSetLayout createSetLayout(vkctx::Context& c, const VkDescriptorSetLayoutBinding* b, uint32_t n);
VkPipelineLayout createLayout(vkctx::Context& c, const VkDescriptorSetLayout* sets, uint32_t n,
                              uint32_t pushBytes, VkShaderStageFlags pushStages);

// Set the whole dynamic state block for a draw range.
void setDynamic(VkCommandBuffer cmd, VkExtent2D extent, bool depthTest, bool depthWrite,
                VkCompareOp compare = VK_COMPARE_OP_LESS_OR_EQUAL);
// Same, for a viewport + scissor that is a sub-rect of the target (the arcade
// letterbox: the composite lands in the 16:9 rect, the bars stay cleared).
void setDynamicRect(VkCommandBuffer cmd, VkRect2D rect, bool depthTest, bool depthWrite,
                    VkCompareOp compare = VK_COMPARE_OP_LESS_OR_EQUAL);

} // namespace vkpipe
} // namespace ts
