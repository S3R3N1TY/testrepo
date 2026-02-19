// PipelineResources.h
#pragma once

#include <memory>
#include <string>

// parasoft-begin-suppress ALL "suppress all violations"
#include <vulkan/vulkan.h>
// parasoft-end-suppress ALL "suppress all violations"

// Forward declarations to reduce include bloat.
struct DeviceContext;

class VulkanRenderPass;
class VulkanPipelineLayout;
class VulkanPipeline;

// Bundle of pipeline-dependent resources that must NOT be destroyed immediately
// when recreating the pipeline/renderpass. recreate() returns this bundle so
// the caller can defer destruction until the GPU is done with it.
struct PipelineGarbage
{
    std::unique_ptr<VulkanRenderPass>     renderPassPtr;
    std::unique_ptr<VulkanPipelineLayout> pipelineLayoutPtr;
    std::unique_ptr<VulkanPipeline>       pipelinePtr;

    VkFormat colorFmt = VK_FORMAT_UNDEFINED;
    VkFormat depthFmt = VK_FORMAT_UNDEFINED;

    [[nodiscard]] bool empty() const noexcept
    {
        return renderPassPtr == nullptr &&
            pipelineLayoutPtr == nullptr &&
            pipelinePtr == nullptr &&
            colorFmt == VK_FORMAT_UNDEFINED &&
            depthFmt == VK_FORMAT_UNDEFINED;
    }
};

class PipelineResources
{
public:
    PipelineResources() = default;
    ~PipelineResources() noexcept { cleanupIdle(); } // safe only if GPU idle at shutdown

    PipelineResources(const PipelineResources&) = delete;
    PipelineResources& operator=(const PipelineResources&) = delete;

    PipelineResources(PipelineResources&&) noexcept = default;
    PipelineResources& operator=(PipelineResources&&) noexcept = default;

    // Optional: configure shader paths (relative to working directory)
    void setShaderPaths(const std::string& vertSpvPath, const std::string& fragSpvPath);

    // First-time creation. No garbage produced.
    // Convention:
    //   set=0: frame/global
    //   set=1: material (placeholder)
    //   set=2: object/instance
    void init(const DeviceContext& devCtx,
        VkFormat colorFormat,
        VkFormat depthFormat,
        VkDescriptorSetLayout frameDSL,
        VkDescriptorSetLayout materialDSL,
        VkDescriptorSetLayout objectDSL,
        VkImageLayout finalColorLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
    {
        createInternal(devCtx, colorFormat, depthFormat, frameDSL, materialDSL, objectDSL, finalColorLayout);
    }

    // Recreate renderpass + pipeline + layout. Populates outGarbage with old resources
    // that MUST be destroyed later (after GPU completes).
    void recreate(const DeviceContext& devCtx,
        VkFormat colorFormat,
        VkFormat depthFormat,
        VkDescriptorSetLayout frameDSL,
        VkDescriptorSetLayout materialDSL,
        VkDescriptorSetLayout objectDSL,
        VkImageLayout finalColorLayout,
        PipelineGarbage& outGarbage);

    // Retire current pipeline resources into outGarbage and leave empty.
    void retire(PipelineGarbage& outGarbage);

    // Immediately destroy currently-owned pipeline resources.
    // Use ONLY when you know the GPU is idle (e.g., after vkDeviceWaitIdle).
    void cleanupIdle() noexcept;

    [[nodiscard]] VkRenderPass     renderPass() const noexcept;
    [[nodiscard]] VkPipelineLayout pipelineLayout() const noexcept;
    [[nodiscard]] VkPipeline       pipeline() const noexcept;

    [[nodiscard]] VkFormat colorFormat() const noexcept { return colorFmt; }
    [[nodiscard]] VkFormat depthFormat() const noexcept { return depthFmt; }

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool compatible(VkFormat newColor, VkFormat newDepth) const noexcept;

private:
    std::unique_ptr<VulkanRenderPass>     renderPassPtr;
    std::unique_ptr<VulkanPipelineLayout> pipelineLayoutPtr;
    std::unique_ptr<VulkanPipeline>       pipelinePtr;

    VkFormat colorFmt = VK_FORMAT_UNDEFINED;
    VkFormat depthFmt = VK_FORMAT_UNDEFINED;

    std::string vertPath = string_constants::kShaderVertPath;
    std::string fragPath = string_constants::kShaderFragPath;

private:
    void createInternal(const DeviceContext& devCtx,
        VkFormat colorFormat,
        VkFormat depthFormat,
        VkDescriptorSetLayout frameDSL,
        VkDescriptorSetLayout materialDSL,
        VkDescriptorSetLayout objectDSL,
        VkImageLayout finalColorLayout);
};
