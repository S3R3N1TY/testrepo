// PipelineResources.cpp
#include <stdexcept>
#include <string>
#include <vector>

// parasoft-begin-suppress ALL "suppress all violations"
#include <glm/glm.hpp>
// parasoft-end-suppress ALL "suppress all violations"

 #include "PipelineResources.h"
#include "engine/mesh/VertexInputState.h"
 #include "DeviceContext.h"
 #include "VkPipeline.h"
 #include "VkUtils.h"
 #include "PipelineStateUtils.h"
void PipelineResources::setShaderPaths(const std::string& vertSpvPath, const std::string& fragSpvPath)
{
    if (vertSpvPath.empty() || fragSpvPath.empty()) {
        throw std::runtime_error("PipelineResources::setShaderPaths: shader path(s) cannot be empty");
    }
    vertPath = vertSpvPath;
    fragPath = fragSpvPath;
}

void PipelineResources::cleanupIdle() noexcept
{
    // Only correct if the GPU is idle (caller responsibility).
    pipelinePtr.reset();
    pipelineLayoutPtr.reset();
    renderPassPtr.reset();

    colorFmt = VK_FORMAT_UNDEFINED;
    depthFmt = VK_FORMAT_UNDEFINED;
}

void PipelineResources::retire(PipelineGarbage& outGarbage)
{
    outGarbage = PipelineGarbage{};

    outGarbage.renderPassPtr = std::move(renderPassPtr);
    outGarbage.pipelineLayoutPtr = std::move(pipelineLayoutPtr);
    outGarbage.pipelinePtr = std::move(pipelinePtr);

    outGarbage.colorFmt = colorFmt;
    outGarbage.depthFmt = depthFmt;

    colorFmt = VK_FORMAT_UNDEFINED;
    depthFmt = VK_FORMAT_UNDEFINED;

    return;
}

void PipelineResources::recreate(const DeviceContext& devCtx,
    VkFormat colorFormat,
    VkFormat depthFormat,
    VkDescriptorSetLayout frameDSL,
    VkDescriptorSetLayout materialDSL,
    VkDescriptorSetLayout objectDSL,
    VkImageLayout finalColorLayout,
    PipelineGarbage& outGarbage)
{
    outGarbage = PipelineGarbage{};
    if (!valid()) {
        createInternal(devCtx, colorFormat, depthFormat, frameDSL, materialDSL, objectDSL, finalColorLayout);
        return;
    }

    retire(outGarbage);
    createInternal(devCtx, colorFormat, depthFormat, frameDSL, materialDSL, objectDSL, finalColorLayout);
    return;
}

VkRenderPass PipelineResources::renderPass() const noexcept
{
    return renderPassPtr != nullptr ? renderPassPtr->get() : VK_NULL_HANDLE;
}

VkPipelineLayout PipelineResources::pipelineLayout() const noexcept
{
    return pipelineLayoutPtr != nullptr ? pipelineLayoutPtr->get() : VK_NULL_HANDLE;
}

VkPipeline PipelineResources::pipeline() const noexcept
{
    return pipelinePtr != nullptr ? pipelinePtr->get() : VK_NULL_HANDLE;
}

bool PipelineResources::valid() const noexcept
{
    return renderPassPtr && pipelineLayoutPtr && pipelinePtr &&
        colorFmt != VK_FORMAT_UNDEFINED && depthFmt != VK_FORMAT_UNDEFINED;
}

bool PipelineResources::compatible(VkFormat newColor, VkFormat newDepth) const noexcept
{
    return newColor == colorFmt && newDepth == depthFmt;
}

void PipelineResources::createInternal(const DeviceContext& devCtx,
    VkFormat colorFormat,
    VkFormat depthFormat,
    VkDescriptorSetLayout frameDSL,
    VkDescriptorSetLayout materialDSL,
    VkDescriptorSetLayout objectDSL,
    VkImageLayout finalColorLayout)
{
    if (colorFormat == VK_FORMAT_UNDEFINED || depthFormat == VK_FORMAT_UNDEFINED) {
        throw std::runtime_error("PipelineResources::createInternal called with undefined format(s)");
    }
    if (frameDSL == VK_NULL_HANDLE || materialDSL == VK_NULL_HANDLE || objectDSL == VK_NULL_HANDLE) {
        throw std::runtime_error("PipelineResources::createInternal called with VK_NULL_HANDLE descriptor set layout(s)");
    }
    if (vertPath.empty() || fragPath.empty()) {
        throw std::runtime_error("PipelineResources::createInternal shader path(s) are empty");
    }

    const VkDevice dev = devCtx.vkDevice();
    if (dev == VK_NULL_HANDLE) {
        throw std::runtime_error("PipelineResources::createInternal: DeviceContext has VK_NULL_HANDLE device");
    }

    colorFmt = colorFormat;
    depthFmt = depthFormat;

    // -----------------------------------------------------
    // Render pass
    // -----------------------------------------------------
    renderPassPtr = std::make_unique<VulkanRenderPass>(dev, colorFmt, depthFmt, finalColorLayout);

    // -----------------------------------------------------
    // Pipeline layout (set=0 frame, set=1 material, set=2 object)
    // Order defines the set numbers.
    // -----------------------------------------------------
    VkPushConstantRange materialPush{};
    materialPush.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    materialPush.offset = 0;
    materialPush.size = sizeof(glm::vec4);

    pipelineLayoutPtr = std::make_unique<VulkanPipelineLayout>(
        dev,
        std::vector<VkDescriptorSetLayout>{ frameDSL, materialDSL, objectDSL },
        std::vector<VkPushConstantRange>{ materialPush }
    );

    // -----------------------------------------------------
    // Shaders
    // -----------------------------------------------------
    const auto shaderStages = vkpipeline::loadShaderStages(dev, vertPath, fragPath);

    // -----------------------------------------------------
    // Fixed function
    // -----------------------------------------------------
    const VertexInputState vertexInput{};
    const vkpipeline::BasicPipelineState state(
        VK_CULL_MODE_BACK_BIT,
        VK_TRUE,
        VK_TRUE,
        VK_COMPARE_OP_LESS_OR_EQUAL,
        VK_FALSE);

    const VkGraphicsPipelineCreateInfo gp = vkpipeline::makeGraphicsPipelineCreateInfo(
        shaderStages.stages,
        vertexInput.info,
        state.inputAssembly,
        state.viewport,
        state.raster,
        state.multisample,
        state.depthStencil,
        state.blend,
        state.dynamic.info,
        pipelineLayoutPtr->get(),
        renderPassPtr->get()
    );

    VulkanPipelineBuildInfo buildInfo{};
    buildInfo.pipelineLayout = pipelineLayoutPtr->get();
    buildInfo.renderPass = renderPassPtr->get();

    GraphicsPipelineBuilder builder{};
    pipelinePtr = std::make_unique<VulkanPipeline>(
        builder
            .setShaderStages(shaderStages.stages)
            .setCreateInfo(gp)
            .setBuildInfo(buildInfo)
            .build(dev));

    if (devCtx.enableValidation) {
        vkutil::setObjectName(dev, VK_OBJECT_TYPE_RENDER_PASS, renderPassPtr->get(), "MainRenderPass");
        vkutil::setObjectName(dev, VK_OBJECT_TYPE_PIPELINE_LAYOUT, pipelineLayoutPtr->get(), "PipelineLayout: Frame+Material+Object");
        vkutil::setObjectName(dev, VK_OBJECT_TYPE_PIPELINE, pipelinePtr->get(), "Pipeline: Main");
    }
}
