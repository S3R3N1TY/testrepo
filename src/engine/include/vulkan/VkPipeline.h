#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>
#include <optional>

// parasoft-begin-suppress ALL "suppress all violations"
#include <vulkan/vulkan.h>
// parasoft-end-suppress ALL "suppress all violations"

#include "UniqueHandle.h"
#include "VkUtils.h"

class PipelineCacheManager {
public:
    PipelineCacheManager() noexcept = default;
    PipelineCacheManager(VkDevice device, VkPhysicalDevice physicalDevice, const std::string& cachePath);

    PipelineCacheManager(const PipelineCacheManager&) = delete;
    PipelineCacheManager& operator=(const PipelineCacheManager&) = delete;

    PipelineCacheManager(PipelineCacheManager&&) noexcept = default;
    PipelineCacheManager& operator=(PipelineCacheManager&&) noexcept = default;

    ~PipelineCacheManager() noexcept;

    [[nodiscard]] VkPipelineCache get() const noexcept { return cache_.get(); }
    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(cache_); }

    void save() const;

private:
    std::string cachePath_{};
    VkPhysicalDevice physicalDevice_{ VK_NULL_HANDLE };
    vkhandle::DeviceUniqueHandle<VkPipelineCache, PFN_vkDestroyPipelineCache> cache_;
};


struct VulkanRenderPassAttachmentDesc {
    VkFormat format{ VK_FORMAT_UNDEFINED };
    VkSampleCountFlagBits samples{ VK_SAMPLE_COUNT_1_BIT };
    VkAttachmentLoadOp loadOp{ VK_ATTACHMENT_LOAD_OP_CLEAR };
    VkAttachmentStoreOp storeOp{ VK_ATTACHMENT_STORE_OP_STORE };
    VkAttachmentLoadOp stencilLoadOp{ VK_ATTACHMENT_LOAD_OP_DONT_CARE };
    VkAttachmentStoreOp stencilStoreOp{ VK_ATTACHMENT_STORE_OP_DONT_CARE };
    VkImageLayout initialLayout{ VK_IMAGE_LAYOUT_UNDEFINED };
    VkImageLayout finalLayout{ VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    bool transient{ false };
};

struct VulkanRenderPassAttachmentRef {
    uint32_t attachment{ VK_ATTACHMENT_UNUSED };
    VkImageLayout layout{ VK_IMAGE_LAYOUT_UNDEFINED };
};

struct VulkanRenderPassSubpassDesc {
    VkPipelineBindPoint bindPoint{ VK_PIPELINE_BIND_POINT_GRAPHICS };
    std::vector<VulkanRenderPassAttachmentRef> inputAttachments{};
    std::vector<VulkanRenderPassAttachmentRef> colorAttachments{};
    std::vector<VulkanRenderPassAttachmentRef> resolveAttachments{};
    std::vector<uint32_t> preserveAttachments{};
    std::optional<VulkanRenderPassAttachmentRef> depthStencilAttachment{};
};

struct VulkanRenderPassDescription {
    std::vector<VulkanRenderPassAttachmentDesc> attachments{};
    std::vector<VulkanRenderPassSubpassDesc> subpasses{};
    std::vector<VkSubpassDependency> dependencies{};
};

class VulkanRenderPass {
public:
    VulkanRenderPass() noexcept = default;

    [[nodiscard]] static vkutil::VkExpected<VulkanRenderPass> createResult(
        VkDevice device,
        VkFormat colorFormat,
        VkFormat depthFormat = VK_FORMAT_UNDEFINED,
        VkImageLayout finalColorLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        VkAttachmentLoadOp colorLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        VkImageLayout initialColorLayout = VK_IMAGE_LAYOUT_UNDEFINED);

    [[nodiscard]] static vkutil::VkExpected<VulkanRenderPass> createResult(
        VkDevice device,
        const std::vector<VkFormat>& colorFormats,
        VkFormat depthFormat = VK_FORMAT_UNDEFINED,
        VkImageLayout finalColorLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        VkAttachmentLoadOp colorLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        VkImageLayout initialColorLayout = VK_IMAGE_LAYOUT_UNDEFINED);

    [[nodiscard]] static vkutil::VkExpected<VulkanRenderPass> createResult(VkDevice device, const VulkanRenderPassDescription& description);

    VulkanRenderPass(
        VkDevice device,
        VkFormat colorFormat,
        VkFormat depthFormat = VK_FORMAT_UNDEFINED,
        VkImageLayout finalColorLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        VkAttachmentLoadOp colorLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        VkImageLayout initialColorLayout = VK_IMAGE_LAYOUT_UNDEFINED);

    VulkanRenderPass(
        VkDevice device,
        const std::vector<VkFormat>& colorFormats,
        VkFormat depthFormat = VK_FORMAT_UNDEFINED,
        VkImageLayout finalColorLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        VkAttachmentLoadOp colorLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        VkImageLayout initialColorLayout = VK_IMAGE_LAYOUT_UNDEFINED);

    VulkanRenderPass(VkDevice device, const VulkanRenderPassDescription& description);

    VulkanRenderPass(const VulkanRenderPass&) = delete;
    VulkanRenderPass& operator=(const VulkanRenderPass&) = delete;

    VulkanRenderPass(VulkanRenderPass&&) noexcept = default;
    VulkanRenderPass& operator=(VulkanRenderPass&&) noexcept = default;

    ~VulkanRenderPass() = default;

    [[nodiscard]] VkRenderPass get() const noexcept { return handle.get(); }
    [[nodiscard]] VkDevice     getDevice() const noexcept { return handle.getDevice(); }
    [[nodiscard]] bool         valid() const noexcept { return static_cast<bool>(handle); }

private:
    vkhandle::DeviceUniqueHandle<VkRenderPass, PFN_vkDestroyRenderPass> handle;
};

class VulkanPipelineLayout {
public:
    VulkanPipelineLayout() noexcept = default;

    VulkanPipelineLayout(
        VkDevice device,
        const std::vector<VkDescriptorSetLayout>& setLayouts = {},
        const std::vector<VkPushConstantRange>& pushConstants = {},
        const void* pNext = nullptr);

    VulkanPipelineLayout(const VulkanPipelineLayout&) = delete;
    VulkanPipelineLayout& operator=(const VulkanPipelineLayout&) = delete;

    VulkanPipelineLayout(VulkanPipelineLayout&&) noexcept = default;
    VulkanPipelineLayout& operator=(VulkanPipelineLayout&&) noexcept = default;

    ~VulkanPipelineLayout() = default;

    [[nodiscard]] VkPipelineLayout get() const noexcept { return handle.get(); }
    [[nodiscard]] VkDevice         getDevice() const noexcept { return handle.getDevice(); }
    [[nodiscard]] bool             valid() const noexcept { return static_cast<bool>(handle); }

private:
    vkhandle::DeviceUniqueHandle<VkPipelineLayout, PFN_vkDestroyPipelineLayout> handle;
};

struct VulkanPipelineBuildInfo {
    VkPipelineLayout pipelineLayout{ VK_NULL_HANDLE };
    VkRenderPass renderPass{ VK_NULL_HANDLE };
    uint32_t subpass{ 0 };

    bool useDynamicRendering{ false };
    std::vector<VkFormat> colorFormats{};
    VkFormat depthFormat{ VK_FORMAT_UNDEFINED };
    VkFormat stencilFormat{ VK_FORMAT_UNDEFINED };

    VkPipelineCache pipelineCache{ VK_NULL_HANDLE };
    VkPipelineCreateFlags createFlags{ 0 };
};

class VulkanPipeline {
public:
    VulkanPipeline() noexcept = default;

    [[nodiscard]] static vkutil::VkExpected<VulkanPipeline> createResult(
        VkDevice device,
        const std::vector<VkPipelineShaderStageCreateInfo>& shaderStages,
        const VkGraphicsPipelineCreateInfo& pipelineCreateInfo,
        const VulkanPipelineBuildInfo& buildInfo);

    VulkanPipeline(
        VkDevice device,
        const std::vector<VkPipelineShaderStageCreateInfo>& shaderStages,
        const VkGraphicsPipelineCreateInfo& pipelineCreateInfo,
        const VulkanPipelineBuildInfo& buildInfo);

    VulkanPipeline(const VulkanPipeline&) = delete;
    VulkanPipeline& operator=(const VulkanPipeline&) = delete;

    VulkanPipeline(VulkanPipeline&&) noexcept = default;
    VulkanPipeline& operator=(VulkanPipeline&&) noexcept = default;

    ~VulkanPipeline() = default;

    [[nodiscard]] VkPipeline get() const noexcept { return handle.get(); }
    [[nodiscard]] VkDevice   getDevice() const noexcept { return handle.getDevice(); }
    [[nodiscard]] bool       valid() const noexcept { return static_cast<bool>(handle); }

private:
    vkhandle::DeviceUniqueHandle<VkPipeline, PFN_vkDestroyPipeline> handle;
};

class VulkanComputePipeline {
public:
    VulkanComputePipeline() noexcept = default;

    [[nodiscard]] static vkutil::VkExpected<VulkanComputePipeline> createResult(VkDevice device,
        const VkComputePipelineCreateInfo& pipelineCreateInfo,
        VkPipelineCache pipelineCache = VK_NULL_HANDLE,
        VkPipelineCreateFlags createFlags = 0);

    VulkanComputePipeline(VkDevice device,
        const VkComputePipelineCreateInfo& pipelineCreateInfo,
        VkPipelineCache pipelineCache = VK_NULL_HANDLE,
        VkPipelineCreateFlags createFlags = 0);

    VulkanComputePipeline(const VulkanComputePipeline&) = delete;
    VulkanComputePipeline& operator=(const VulkanComputePipeline&) = delete;

    VulkanComputePipeline(VulkanComputePipeline&&) noexcept = default;
    VulkanComputePipeline& operator=(VulkanComputePipeline&&) noexcept = default;

    ~VulkanComputePipeline() = default;

    [[nodiscard]] VkPipeline get() const noexcept { return handle.get(); }
    [[nodiscard]] VkDevice getDevice() const noexcept { return handle.getDevice(); }
    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(handle); }

private:
    vkhandle::DeviceUniqueHandle<VkPipeline, PFN_vkDestroyPipeline> handle;
};

class GraphicsPipelineBuilder {
public:
    GraphicsPipelineBuilder& setShaderStages(std::vector<VkPipelineShaderStageCreateInfo> stages);
    GraphicsPipelineBuilder& setCreateInfo(const VkGraphicsPipelineCreateInfo& ci);
    GraphicsPipelineBuilder& setBuildInfo(const VulkanPipelineBuildInfo& buildInfo);

    [[nodiscard]] VulkanPipeline build(VkDevice device) const;

private:
    std::vector<VkPipelineShaderStageCreateInfo> shaderStages_{};
    VkGraphicsPipelineCreateInfo createInfo_{};
    VulkanPipelineBuildInfo buildInfo_{};
};

class ComputePipelineBuilder {
public:
    ComputePipelineBuilder& setCreateInfo(const VkComputePipelineCreateInfo& ci);
    ComputePipelineBuilder& setPipelineCache(VkPipelineCache cache) noexcept;
    ComputePipelineBuilder& setCreateFlags(VkPipelineCreateFlags flags) noexcept;

    [[nodiscard]] VulkanComputePipeline build(VkDevice device) const;

private:
    VkComputePipelineCreateInfo createInfo_{};
    VkPipelineCache pipelineCache_{ VK_NULL_HANDLE };
    VkPipelineCreateFlags createFlags_{ 0 };
};
