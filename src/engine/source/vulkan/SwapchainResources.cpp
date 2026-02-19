// SwapchainResources.cpp
#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

 #include "SwapchainResources.h"
 #include "DeviceContext.h"
 #include "VkUtils.h"
namespace
{
    constexpr const char kSwapchainNotInitialized[] = "SwapchainResources::swapchain() called before init";
    constexpr const char kCloseBracket[] = "]";

}

// =========================================================
// Swapchain support query + choices
// =========================================================

void SwapchainResources::querySupport(VkPhysicalDevice physical,
    VkSurfaceKHR surface,
    SwapchainSupportInfo& outInfo) const
{
    outInfo = SwapchainSupportInfo{};

    VkResult res = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &outInfo.caps);
    if (res != VK_SUCCESS) {
        std::string msg = "SwapchainResources::querySupport: vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed (";
        msg += vkutil::vkResultToString(res);
        msg += ")";
        throw std::runtime_error(msg);
    }

    uint32_t fmtCount = 0;
    res = vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &fmtCount, nullptr);
    if (res != VK_SUCCESS) {
        std::string msg = "SwapchainResources::querySupport: vkGetPhysicalDeviceSurfaceFormatsKHR(count) failed (";
        msg += vkutil::vkResultToString(res);
        msg += ")";
        throw std::runtime_error(msg);
    }
    outInfo.formats.resize(fmtCount);
    if (fmtCount > 0) {
        res = vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &fmtCount, outInfo.formats.data());
        if (res != VK_SUCCESS && res != VK_INCOMPLETE) {
            std::string msg = "SwapchainResources::querySupport: vkGetPhysicalDeviceSurfaceFormatsKHR(data) failed (";
            msg += vkutil::vkResultToString(res);
            msg += ")";
            throw std::runtime_error(msg);
        }
        outInfo.formats.resize(fmtCount);
    }

    uint32_t modeCount = 0;
    res = vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &modeCount, nullptr);
    if (res != VK_SUCCESS) {
        std::string msg = "SwapchainResources::querySupport: vkGetPhysicalDeviceSurfacePresentModesKHR(count) failed (";
        msg += vkutil::vkResultToString(res);
        msg += ")";
        throw std::runtime_error(msg);
    }
    outInfo.modes.resize(modeCount);
    if (modeCount > 0) {
        res = vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &modeCount, outInfo.modes.data());
        if (res != VK_SUCCESS && res != VK_INCOMPLETE) {
            std::string msg = "SwapchainResources::querySupport: vkGetPhysicalDeviceSurfacePresentModesKHR(data) failed (";
            msg += vkutil::vkResultToString(res);
            msg += ")";
            throw std::runtime_error(msg);
        }
        outInfo.modes.resize(modeCount);
    }
    return;
}

void SwapchainResources::chooseSurfaceFormat(
    const std::vector<VkSurfaceFormatKHR>& formats,
    VkSurfaceFormatKHR& outFormat) const
{
    if (formats.empty()) {
        throw std::runtime_error("SwapchainResources: no surface formats reported");
    }

    // Prefer SRGB if available.
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            outFormat = f;
            return;
        }
    }

    outFormat = formats[0];
    return;
}

VkPresentModeKHR SwapchainResources::choosePresentMode(
    const std::vector<VkPresentModeKHR>& modes) const
{
    // Prefer mailbox if available (low latency + no tearing).
    for (const auto m : modes) {
        if (m == VK_PRESENT_MODE_MAILBOX_KHR) {
            return m;
        }
    }

    // Guaranteed to exist.
    return VK_PRESENT_MODE_FIFO_KHR;
}

void SwapchainResources::chooseExtent(const VkSurfaceCapabilitiesKHR& caps,
    uint32_t width,
    uint32_t height,
    VkExtent2D& outExtent) const
{
    if (caps.currentExtent.width != UINT32_MAX) {
        outExtent = caps.currentExtent;
        return;
    }

    outExtent = VkExtent2D{ width, height };
    outExtent.width = std::max(caps.minImageExtent.width,
        std::min(caps.maxImageExtent.width, outExtent.width));
    outExtent.height = std::max(caps.minImageExtent.height,
        std::min(caps.maxImageExtent.height, outExtent.height));
    return;
}

// =========================================================
// Depth helpers
// =========================================================

VkFormat SwapchainResources::findDepthFormat(VkPhysicalDevice physical) const
{
    const VkFormat candidates[] = {
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D24_UNORM_S8_UINT,
        VK_FORMAT_D32_SFLOAT_S8_UINT
    };

    for (const VkFormat f : candidates) {
        VkFormatProperties p{};
        vkGetPhysicalDeviceFormatProperties(physical, f, &p);
        if (p.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            return f;
        }
    }

    throw std::runtime_error("SwapchainResources: no supported depth format found");
}

VkImageAspectFlags SwapchainResources::depthAspectMask(VkFormat format) const noexcept
{
    // If you later decide to actually use stencil, this is ready.
    switch (format) {
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default:
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    }
}

void SwapchainResources::makeDepthImageCI(const VkExtent2D& extent, VkFormat format, VkImageCreateInfo& outInfo) const
{
    outInfo = VkImageCreateInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    outInfo.imageType = VK_IMAGE_TYPE_2D;
    outInfo.extent = { extent.width, extent.height, 1 };
    outInfo.mipLevels = 1;
    outInfo.arrayLayers = 1;
    outInfo.format = format;
    outInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    outInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    outInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    outInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    outInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    return;
}

// =========================================================
// Public API
// =========================================================

void SwapchainResources::retire(SwapchainGarbage& outGarbage)
{
    outGarbage = SwapchainGarbage{};

    outGarbage.swap = std::move(swap);
    outGarbage.swapImageViews = std::move(swapImageViews);

    outGarbage.depthImage = std::move(depthImage);
    outGarbage.depthView = std::move(depthView);

    outGarbage.framebuffers = std::move(framebuffers);

    outGarbage.depthFmt = depthFmt;

    // Leave this object empty/neutral.
    depthFmt = VK_FORMAT_UNDEFINED;
    minImageCountValue = 0;

    return;
}

void SwapchainResources::recreateBase(const DeviceContext& devCtx,
    uint32_t width,
    uint32_t height,
    SwapchainGarbage& outGarbage)
{
    outGarbage = SwapchainGarbage{};
    // If we have nothing yet, just create fresh.
    if (!swap) {
        createBaseInternal(devCtx, width, height, VK_NULL_HANDLE);
        return;
    }

    // Retire current swapchain-dependent resources (DO NOT destroy now).
    retire(outGarbage);

    // Use old swapchain handle for smooth handover during vkCreateSwapchainKHR.
    const VkSwapchainKHR oldSwap = outGarbage.swap != nullptr ? outGarbage.swap->get() : VK_NULL_HANDLE;

    // Build the new swapchain + base dependent resources (NO fbos).
    createBaseInternal(devCtx, width, height, oldSwap);

    // Return old bundle to be deferred-destroyed by caller.
    return;
}

void SwapchainResources::buildFramebuffers(const DeviceContext& devCtx, VkRenderPass renderPass)
{
    if (!swap) {
        throw std::runtime_error("SwapchainResources::buildFramebuffers called before init/recreateBase");
    }
    if (renderPass == VK_NULL_HANDLE) {
        throw std::runtime_error("SwapchainResources::buildFramebuffers called with VK_NULL_HANDLE renderPass");
    }
    if (!depthView) {
        throw std::runtime_error("SwapchainResources::buildFramebuffers: missing depth view");
    }

    const VkDevice dev = devCtx.vkDevice();

    framebuffers.clear();
    framebuffers.reserve(swapImageViews.size());

    for (size_t i = 0; i < swapImageViews.size(); ++i) {
        std::vector<VkImageView> atts{
            swapImageViews[i].get(),
            depthView->get()
        };

        framebuffers.emplace_back(
            dev,
            renderPass,
            atts,
            swap->getExtent().width,
            swap->getExtent().height
        );
    }

    if (devCtx.enableValidation) {
        for (size_t i = 0; i < framebuffers.size(); ++i) {
            const std::string n = "Framebuffer[" + std::to_string(i) + kCloseBracket;
            vkutil::setObjectName(dev, VK_OBJECT_TYPE_FRAMEBUFFER, framebuffers[i].get(), n.c_str());
        }
    }
}

void SwapchainResources::cleanupIdle() noexcept
{
    // Only call this when GPU is idle (e.g., after vkDeviceWaitIdle).
    framebuffers.clear();

    depthView.reset();
    depthImage.reset();

    swapImageViews.clear();
    swap.reset();

    depthFmt = VK_FORMAT_UNDEFINED;
    minImageCountValue = 0;
}

VulkanSwapchain& SwapchainResources::swapchain()
{
    if (!swap) {
        throw std::runtime_error(kSwapchainNotInitialized);
    }
    return *swap;
}

const VulkanSwapchain& SwapchainResources::swapchain() const
{
    if (!swap) {
        throw std::runtime_error(kSwapchainNotInitialized);
    }
    return *swap;
}

void SwapchainResources::extent(VkExtent2D& outExtent) const noexcept
{
    outExtent = swap != nullptr ? swap->getExtent() : VkExtent2D{ 0, 0 };
    return;
}

VkFormat SwapchainResources::colorFormat() const noexcept
{
    return swap != nullptr ? swap->getImageFormat() : VK_FORMAT_UNDEFINED;
}


VkFramebuffer SwapchainResources::framebuffer(uint32_t imageIndex) const
{
    // Let this explode loudly if you index wrong.
    return framebuffers.at(imageIndex).get();
}

uint32_t SwapchainResources::imageCount() const noexcept
{
    return swap != nullptr ? static_cast<uint32_t>(swap->getImages().size()) : 0u;
}


// =========================================================
// Internal creation (swapchain + views + depth, NO fbos)
// =========================================================

void SwapchainResources::createBaseInternal(const DeviceContext& devCtx,
    uint32_t width,
    uint32_t height,
    VkSwapchainKHR oldSwap)
{
    if (width == 0 || height == 0) {
        // Common when window is minimized. Caller should skip recreate until non-zero.
        throw std::runtime_error("SwapchainResources: cannot create swapchain with extent 0x0 (window minimized?)");
    }

    const VkDevice dev = devCtx.vkDevice();
    const VkPhysicalDevice pd = devCtx.vkPhysical();
    VkSurfaceKHR surface = devCtx.vkSurface();

    SwapchainSupportInfo sc{};
    querySupport(pd, surface, sc);
    if (sc.formats.empty() || sc.modes.empty()) {
        throw std::runtime_error("SwapchainResources: swapchain support query returned no formats or no present modes");
    }

    minImageCountValue = sc.caps.minImageCount;
    VkSurfaceFormatKHR chosenFmt{};
    chooseSurfaceFormat(sc.formats, chosenFmt);
    const VkPresentModeKHR   chosenMode = choosePresentMode(sc.modes);
    VkExtent2D ex{};
    chooseExtent(sc.caps, width, height, ex);

    // -----------------------------------------------------
    // Swapchain
    // -----------------------------------------------------
    swap = std::make_unique<VulkanSwapchain>(
        dev,
        surface,
        ex.width,
        ex.height,
        chosenFmt,
        chosenMode,
        sc.caps,
        devCtx.graphicsFamilyIndex(),
        devCtx.presentFamilyIndex(),
        oldSwap,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
    );

    // -----------------------------------------------------
    // Swapchain image views
    // -----------------------------------------------------
    {
        const auto& imgs = swap->getImages();
        swapImageViews.clear();
        swapImageViews.reserve(imgs.size());
        for (auto img : imgs) {
            swapImageViews.emplace_back(
                dev,
                img,
                swap->getImageFormat(),
                VK_IMAGE_ASPECT_COLOR_BIT
            );
        }
    }

    if (devCtx.enableValidation) {
        vkutil::setObjectName(dev, VK_OBJECT_TYPE_SWAPCHAIN_KHR, swap->get(), "Swapchain");
        for (size_t i = 0; i < swapImageViews.size(); ++i) {
            const std::string n = "SwapImageView[" + std::to_string(i) + kCloseBracket;
            vkutil::setObjectName(dev, VK_OBJECT_TYPE_IMAGE_VIEW, swapImageViews[i].get(), n.c_str());
        }
    }

    // -----------------------------------------------------
    // Depth format + image + view
    // -----------------------------------------------------
    depthFmt = findDepthFormat(pd);

    {
        VkImageCreateInfo ci{};
        makeDepthImageCI(swap->getExtent(), depthFmt, ci);
        depthImage = std::make_unique<VulkanImage>(dev, pd, ci, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        depthView = std::make_unique<VulkanImageView>(
            dev,
            depthImage->get(),
            depthFmt,
            depthAspectMask(depthFmt)
        );
    }

    if (devCtx.enableValidation) {
        vkutil::setObjectName(dev, VK_OBJECT_TYPE_IMAGE, depthImage->get(), "DepthImage");
        vkutil::setObjectName(dev, VK_OBJECT_TYPE_IMAGE_VIEW, depthView->get(), "DepthView");
    }

    // No framebuffers created here (needs render pass).
    framebuffers.clear();
}
