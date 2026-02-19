// SwapchainResources.h
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

// parasoft-begin-suppress ALL "suppress all violations"
#include <vulkan/vulkan.h>
// parasoft-end-suppress ALL "suppress all violations"

 #include "VkSwapchain.h"
 #include "VkPipeline.h" // VulkanFramebuffer wrapper (as per your setup)
struct DeviceContext; // fwd

struct SwapchainSupportInfo
{
    VkSurfaceCapabilitiesKHR        caps{};
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR>   modes;
};

// Bundle of swapchain-dependent resources that must NOT be destroyed immediately
// when recreating the swapchain. retire() / recreateBase() returns this bundle so
// the caller can defer destruction until the GPU is done with it.
struct SwapchainGarbage
{
    std::unique_ptr<VulkanSwapchain> swap;
    std::vector<VulkanImageView>     swapImageViews;

    std::unique_ptr<VulkanImage>     depthImage;
    std::unique_ptr<VulkanImageView> depthView;

    std::vector<VulkanFramebuffer>   framebuffers;

    VkFormat depthFmt = VK_FORMAT_UNDEFINED;

    [[nodiscard]] bool empty() const noexcept
    {
        return swap == nullptr &&
            swapImageViews.empty() &&
            depthImage == nullptr &&
            depthView == nullptr &&
            framebuffers.empty() &&
            depthFmt == VK_FORMAT_UNDEFINED;
    }
};

class SwapchainResources
{
public:
    SwapchainResources() = default;
    ~SwapchainResources() noexcept { cleanupIdle(); }

    SwapchainResources(const SwapchainResources&) = delete;
    SwapchainResources& operator=(const SwapchainResources&) = delete;

    SwapchainResources(SwapchainResources&&) noexcept = default;
    SwapchainResources& operator=(SwapchainResources&&) noexcept = default;

    // Creates the initial swapchain + dependent resources (swap + views + depth).
    // NOTE: does NOT create framebuffers (needs a render pass).
    void init(const DeviceContext& devCtx,
        uint32_t width,
        uint32_t height)
    {
        createBaseInternal(devCtx, width, height, VK_NULL_HANDLE);
    }

    // Recreates swapchain + views + depth. Populates outGarbage with old swapchain-dependent
    // resources that MUST be destroyed later (after GPU is done).
    //
    // NOTE: After recreateBase(), framebuffers are NOT built. Call buildFramebuffers().
    void recreateBase(const DeviceContext& devCtx,
        uint32_t width,
        uint32_t height,
        SwapchainGarbage& outGarbage);

    // Build (or rebuild) framebuffers using the provided render pass.
    // Safe to call after init() or recreateBase().
    void buildFramebuffers(const DeviceContext& devCtx, VkRenderPass renderPass);

    // Retires current resources into outGarbage and leaves this object empty.
    void retire(SwapchainGarbage& outGarbage);

    // Immediately destroy any currently-owned swapchain-dependent resources.
    // Use ONLY when you know the GPU is idle (e.g., after vkDeviceWaitIdle).
    void cleanupIdle() noexcept;

    // Accessors used by Renderer
    VulkanSwapchain& swapchain();
    const VulkanSwapchain& swapchain() const;

    void extent(VkExtent2D& outExtent) const noexcept;
    [[nodiscard]] VkFormat   colorFormat() const noexcept;
    [[nodiscard]] VkFormat   depthFormat() const noexcept { return depthFmt; }

    [[nodiscard]] VkFramebuffer framebuffer(uint32_t imageIndex) const;

    [[nodiscard]] uint32_t imageCount() const noexcept;
    [[nodiscard]] uint32_t minImageCount() const noexcept { return minImageCountValue; }

private:
    // Swapchain + dependent resources (swapchain-dependent)
    std::unique_ptr<VulkanSwapchain> swap;
    std::vector<VulkanImageView>     swapImageViews;

    std::unique_ptr<VulkanImage>     depthImage;
    std::unique_ptr<VulkanImageView> depthView;

    std::vector<VulkanFramebuffer>   framebuffers;

    VkFormat depthFmt = VK_FORMAT_UNDEFINED;
    uint32_t minImageCountValue = 0;

private:
    // helpers
    void querySupport(VkPhysicalDevice physical, VkSurfaceKHR surface, SwapchainSupportInfo& outInfo) const;

    void chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats,
        VkSurfaceFormatKHR& outFormat) const;
    VkPresentModeKHR   choosePresentMode(const std::vector<VkPresentModeKHR>& modes) const;
    void chooseExtent(const VkSurfaceCapabilitiesKHR& caps,
        uint32_t width,
        uint32_t height,
        VkExtent2D& outExtent) const;

    VkFormat          findDepthFormat(VkPhysicalDevice physical) const;
    void makeDepthImageCI(const VkExtent2D& extent, VkFormat format, VkImageCreateInfo& outInfo) const;
    VkImageAspectFlags depthAspectMask(VkFormat format) const noexcept;

    // Create swapchain + swap views + depth (NO framebuffers)
    void createBaseInternal(const DeviceContext& devCtx,
        uint32_t width,
        uint32_t height,
        VkSwapchainKHR oldSwap);
};
