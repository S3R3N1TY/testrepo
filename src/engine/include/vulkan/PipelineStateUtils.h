#pragma once

#include <array>
#include <string>
#include <vector>

// parasoft-begin-suppress ALL "suppress all violations"
#include <vulkan/vulkan.h>
// parasoft-end-suppress ALL "suppress all violations"

 #include "VkShaderModule.h"
 #include "VkUtils.h"

namespace vkpipeline {

struct ShaderStages
{
    VulkanShaderModule vert;
    VulkanShaderModule frag;
    std::vector<VkPipelineShaderStageCreateInfo> stages;
};

struct DynamicStateConfig
{
    std::array<VkDynamicState, 2> states{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo info{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };

    DynamicStateConfig()
    {
        info.dynamicStateCount = static_cast<uint32_t>(states.size());
        info.pDynamicStates = states.data();
    }
};

[[nodiscard]] inline ShaderStages loadShaderStages(VkDevice device,
    const std::string& vertPath,
    const std::string& fragPath)
{
    std::vector<char> vertCode{};
    std::vector<char> fragCode{};
    vkutil::readFile(vertPath, vertCode);
    vkutil::readFile(fragPath, fragCode);

    ShaderStages result{
        VulkanShaderModule(device, vertCode),
        VulkanShaderModule(device, fragCode),
        {}
    };

    VkPipelineShaderStageCreateInfo vs{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    vs.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vs.module = result.vert.get();
    vs.pName = string_constants::kShaderEntryPoint;

    VkPipelineShaderStageCreateInfo fs{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    fs.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fs.module = result.frag.get();
    fs.pName = string_constants::kShaderEntryPoint;

    result.stages = { vs, fs };
    return result;
}

[[nodiscard]] inline VkPipelineInputAssemblyStateCreateInfo makeTriangleListInputAssembly()
{
    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    ia.primitiveRestartEnable = VK_FALSE;
    return ia;
}

[[nodiscard]] inline VkPipelineVertexInputStateCreateInfo makeEmptyVertexInputState()
{
    VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount = 0;
    vi.vertexAttributeDescriptionCount = 0;
    return vi;
}

[[nodiscard]] inline VkPipelineViewportStateCreateInfo makeViewportState()
{
    VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    return vp;
}

[[nodiscard]] inline VkPipelineRasterizationStateCreateInfo makeRasterizationState(VkCullModeFlags cullMode)
{
    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.depthClampEnable = VK_FALSE;
    rs.rasterizerDiscardEnable = VK_FALSE;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = cullMode;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.depthBiasEnable = VK_FALSE;
    rs.lineWidth = 1.0f;
    return rs;
}

[[nodiscard]] inline VkPipelineMultisampleStateCreateInfo makeMultisampleState()
{
    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    ms.sampleShadingEnable = VK_FALSE;
    return ms;
}

[[nodiscard]] inline VkPipelineDepthStencilStateCreateInfo makeDepthStencilState(
    VkBool32 depthTestEnable,
    VkBool32 depthWriteEnable,
    VkCompareOp compareOp)
{
    VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable = depthTestEnable;
    ds.depthWriteEnable = depthWriteEnable;
    ds.depthCompareOp = compareOp;
    ds.depthBoundsTestEnable = VK_FALSE;
    ds.stencilTestEnable = VK_FALSE;
    return ds;
}

[[nodiscard]] inline VkPipelineColorBlendAttachmentState makeColorBlendAttachment(VkBool32 blendEnable)
{
    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable = blendEnable;
    cba.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT |
        VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT |
        VK_COLOR_COMPONENT_A_BIT;
    return cba;
}

[[nodiscard]] inline VkPipelineColorBlendStateCreateInfo makeColorBlendState(
    const VkPipelineColorBlendAttachmentState& attachment)
{
    VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.logicOpEnable = VK_FALSE;
    cb.attachmentCount = 1;
    cb.pAttachments = &attachment;
    return cb;
}

[[nodiscard]] inline VkGraphicsPipelineCreateInfo makeGraphicsPipelineCreateInfo(
    const std::vector<VkPipelineShaderStageCreateInfo>& stages,
    const VkPipelineVertexInputStateCreateInfo& vi,
    const VkPipelineInputAssemblyStateCreateInfo& ia,
    const VkPipelineViewportStateCreateInfo& vp,
    const VkPipelineRasterizationStateCreateInfo& rs,
    const VkPipelineMultisampleStateCreateInfo& ms,
    const VkPipelineDepthStencilStateCreateInfo& ds,
    const VkPipelineColorBlendStateCreateInfo& cb,
    const VkPipelineDynamicStateCreateInfo& dyn,
    VkPipelineLayout layout,
    VkRenderPass renderPass,
    uint32_t subpass = 0)
{
    VkGraphicsPipelineCreateInfo gp{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gp.stageCount = static_cast<uint32_t>(stages.size());
    gp.pStages = stages.data();
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dyn;
    gp.layout = layout;
    gp.renderPass = renderPass;
    gp.subpass = subpass;
    return gp;
}

struct BasicPipelineState
{
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo viewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    DynamicStateConfig dynamic{};
    VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineDepthStencilStateCreateInfo depthStencil{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState blendAttachment{};
    VkPipelineColorBlendStateCreateInfo blend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };

    BasicPipelineState(VkCullModeFlags cullMode,
        VkBool32 depthTestEnable,
        VkBool32 depthWriteEnable,
        VkCompareOp compareOp,
        VkBool32 blendEnable)
        : inputAssembly(makeTriangleListInputAssembly())
        , viewport(makeViewportState())
        , dynamic()
        , raster(makeRasterizationState(cullMode))
        , multisample(makeMultisampleState())
        , depthStencil(makeDepthStencilState(depthTestEnable, depthWriteEnable, compareOp))
        , blendAttachment(makeColorBlendAttachment(blendEnable))
        , blend(makeColorBlendState(blendAttachment))
    {
    }
};

} // namespace vkpipeline
