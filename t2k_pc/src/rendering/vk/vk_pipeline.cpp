// ============================================================================
// vk_pipeline.cpp — see vk_pipeline.h.
// ============================================================================

#include "vk_pipeline.h"
#include "vk_context.h"

#include <cstdio>

#include <volk.h>

namespace ts {
namespace vkpipe {

VkShaderModule createShader(vkctx::Context& c, const uint32_t* spirv, size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = spirv;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(c.device, &ci, nullptr, &m) != VK_SUCCESS) {
        std::fprintf(stderr, "[vk] vkCreateShaderModule failed (%zu bytes)\n", bytes);
        return VK_NULL_HANDLE;
    }
    return m;
}

static VkPipelineColorBlendAttachmentState blendState(Blend b) {
    VkPipelineColorBlendAttachmentState s{};
    s.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                       VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    s.blendEnable = b != BLEND_NONE;
    s.colorBlendOp = VK_BLEND_OP_ADD;
    s.alphaBlendOp = VK_BLEND_OP_ADD;
    s.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    s.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    switch (b) {
        case BLEND_ADD:
            s.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; s.dstColorBlendFactor = VK_BLEND_FACTOR_ONE; break;
        case BLEND_ADD_SRC_ALPHA:
            s.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; s.dstColorBlendFactor = VK_BLEND_FACTOR_ONE; break;
        case BLEND_PREMUL:
            s.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; s.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            s.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; s.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; break;
        case BLEND_ALPHA:
            s.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; s.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            s.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; s.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; break;
        case BLEND_SCREEN_INV_DST:
            s.srcColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR; s.dstColorBlendFactor = VK_BLEND_FACTOR_ONE; break;
        case BLEND_REVERSE_SUBTRACT:
            s.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; s.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            s.colorBlendOp = VK_BLEND_OP_REVERSE_SUBTRACT;
            s.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; s.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE; break;
        default: break;
    }
    return s;
}

VkPipeline createGraphics(vkctx::Context& c, const Desc& d) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = d.vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = d.frag;
    stages[1].pName = "main";

    VkVertexInputBindingDescription bindings[4]{};
    VkVertexInputAttributeDescription attrs[16]{};
    for (uint32_t i = 0; i < d.bindingCount && i < 4; ++i) {
        bindings[i].binding = d.bindings[i].binding;
        bindings[i].stride = d.bindings[i].stride;
        bindings[i].inputRate = d.bindings[i].perInstance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
    }
    for (uint32_t i = 0; i < d.attrCount && i < 16; ++i) {
        attrs[i].location = d.attrs[i].location;
        attrs[i].binding = d.attrs[i].binding;
        attrs[i].format = d.attrs[i].format;
        attrs[i].offset = d.attrs[i].offset;
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = d.bindingCount;
    vi.pVertexBindingDescriptions = bindings;
    vi.vertexAttributeDescriptionCount = d.attrCount;
    vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = d.topology;

    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;     // the camera is inside the tube; never cull
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    rs.depthClampEnable = d.depthClamp ? VK_TRUE : VK_FALSE;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = d.samples;

    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;      // dynamic
    ds.depthWriteEnable = VK_TRUE;     // dynamic
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;   // dynamic

    VkPipelineColorBlendAttachmentState blends[4];
    for (uint32_t i = 0; i < 4; ++i) blends[i] = blendState(d.blend);
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = d.colorCount;
    cb.pAttachments = blends;

    const VkDynamicState dyn[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
    };
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = sizeof(dyn) / sizeof(dyn[0]);
    dy.pDynamicStates = dyn;

    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.viewMask = d.viewMask;
    ri.colorAttachmentCount = d.colorCount;
    ri.pColorAttachmentFormats = d.colorFormats;
    ri.depthAttachmentFormat = d.depthFormat;

    VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.pNext = &ri;
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pDepthStencilState = (d.depthFormat != VK_FORMAT_UNDEFINED) ? &ds : nullptr;
    pci.pColorBlendState = &cb;
    pci.pDynamicState = &dy;
    pci.layout = d.layout;
    pci.renderPass = VK_NULL_HANDLE;

    VkPipeline p = VK_NULL_HANDLE;
    const VkResult r = vkCreateGraphicsPipelines(c.device, VK_NULL_HANDLE, 1, &pci, nullptr, &p);
    if (r != VK_SUCCESS) std::fprintf(stderr, "[vk] vkCreateGraphicsPipelines failed (%d)\n", (int)r);
    return p;
}

VkDescriptorSetLayout createSetLayout(vkctx::Context& c, const VkDescriptorSetLayoutBinding* b, uint32_t n) {
    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = n;
    ci.pBindings = b;
    VkDescriptorSetLayout l = VK_NULL_HANDLE;
    vkCreateDescriptorSetLayout(c.device, &ci, nullptr, &l);
    return l;
}

VkPipelineLayout createLayout(vkctx::Context& c, const VkDescriptorSetLayout* sets, uint32_t n,
                              uint32_t pushBytes, VkShaderStageFlags pushStages) {
    VkPushConstantRange pr{pushStages, 0, pushBytes};
    VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    ci.setLayoutCount = n;
    ci.pSetLayouts = sets;
    ci.pushConstantRangeCount = pushBytes ? 1 : 0;
    ci.pPushConstantRanges = pushBytes ? &pr : nullptr;
    VkPipelineLayout l = VK_NULL_HANDLE;
    vkCreatePipelineLayout(c.device, &ci, nullptr, &l);
    return l;
}

void setDynamic(VkCommandBuffer cmd, VkExtent2D extent, bool depthTest, bool depthWrite, VkCompareOp compare) {
    VkViewport v{0.0f, 0.0f, (float)extent.width, (float)extent.height, 0.0f, 1.0f};
    VkRect2D s{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &v);
    vkCmdSetScissor(cmd, 0, 1, &s);
    vkCmdSetDepthTestEnable(cmd, depthTest ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthWriteEnable(cmd, depthWrite ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthCompareOp(cmd, compare);
}

void setDynamicRect(VkCommandBuffer cmd, VkRect2D rect, bool depthTest, bool depthWrite, VkCompareOp compare) {
    VkViewport v{(float)rect.offset.x, (float)rect.offset.y, (float)rect.extent.width, (float)rect.extent.height, 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &v);
    vkCmdSetScissor(cmd, 0, 1, &rect);
    vkCmdSetDepthTestEnable(cmd, depthTest ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthWriteEnable(cmd, depthWrite ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthCompareOp(cmd, compare);
}

} // namespace vkpipe
} // namespace ts
