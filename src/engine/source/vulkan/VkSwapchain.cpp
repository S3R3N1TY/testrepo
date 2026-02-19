// VkSwapchain.cpp
#include <array>
#include <string>
#include <utility>   // std::move

 #include "VkSwapchain.h"
 #include "VkUtils.h"
 #include "DeferredDeletionService.h"

// ===================== VulkanImage =====================

vkutil::VkExpected<VulkanImage> VulkanImage::createResult(VkDevice device,
    VkPhysicalDevice physicalDevice,
    const VkImageCreateInfo& createInfo,
    VkMemoryPropertyFlags memoryProps,
    GpuAllocator::LifetimeClass lifetimeClass)
{
    try {
        return VulkanImage(device, physicalDevice, createInfo, memoryProps, lifetimeClass);
    } catch (const vkutil::VkException& ex) {
        return vkutil::VkExpected<VulkanImage>(ex.result());
    } catch (...) {
        return vkutil::VkExpected<VulkanImage>(vkutil::exceptionToVkResult());
    }
}

vkutil::VkExpected<VulkanImage> VulkanImage::createResult(GpuAllocator& allocator,
    const VkImageCreateInfo& createInfo,
    VkMemoryPropertyFlags memoryProps,
    GpuAllocator::LifetimeClass lifetimeClass)
{
    try {
        return VulkanImage(allocator, createInfo, memoryProps, lifetimeClass);
    } catch (const vkutil::VkException& ex) {
        return vkutil::VkExpected<VulkanImage>(ex.result());
    } catch (...) {
        return vkutil::VkExpected<VulkanImage>(vkutil::exceptionToVkResult());
    }
}

VulkanImage::VulkanImage(VkDevice device_,
    VkPhysicalDevice pd,
    const VkImageCreateInfo& ci,
    VkMemoryPropertyFlags props,
    GpuAllocator::LifetimeClass lifetimeClass)
    : device(device_)
    , physicalDevice(pd)
    , image(VK_NULL_HANDLE)
    , memory(VK_NULL_HANDLE)
    , desiredProps(props)
    , lifetimeClass_(lifetimeClass)
    , ownedAllocator(std::make_unique<GpuAllocator>(device_, pd))
    , allocator(ownedAllocator.get())
{
    if (device == VK_NULL_HANDLE) {
        throw std::runtime_error("VulkanImage: device is VK_NULL_HANDLE");
    }
    if (physicalDevice == VK_NULL_HANDLE) {
        throw std::runtime_error("VulkanImage: physicalDevice is VK_NULL_HANDLE");
    }

    const VkResult createRes = vkCreateImage(device, &ci, nullptr, &image);
    if (createRes != VK_SUCCESS) {
        vkutil::throwVkError("vkCreateImage", createRes);
    }

    try {
        allocateAndBind();
    }
    catch (...) {
        if (image != VK_NULL_HANDLE) {
            vkDestroyImage(device, image, nullptr);
            image = VK_NULL_HANDLE;
        }
        throw;
    }
}

VulkanImage::VulkanImage(GpuAllocator& allocator_,
    const VkImageCreateInfo& ci,
    VkMemoryPropertyFlags props,
    GpuAllocator::LifetimeClass lifetimeClass)
    : device(allocator_.device())
    , physicalDevice(allocator_.physicalDevice())
    , image(VK_NULL_HANDLE)
    , memory(VK_NULL_HANDLE)
    , desiredProps(props)
    , lifetimeClass_(lifetimeClass)
    , allocator(&allocator_)
{
    if (!allocator || !allocator->valid()) {
        throw std::runtime_error("VulkanImage: allocator is invalid");
    }

    const VkResult createRes = vkCreateImage(device, &ci, nullptr, &image);
    if (createRes != VK_SUCCESS) {
        vkutil::throwVkError("vkCreateImage", createRes);
    }

    try {
        allocateAndBind();
    }
    catch (...) {
        if (image != VK_NULL_HANDLE) {
            vkDestroyImage(device, image, nullptr);
            image = VK_NULL_HANDLE;
        }
        throw;
    }
}

VulkanImage::VulkanImage(VulkanImage&& other) noexcept
    : device(std::exchange(other.device, VK_NULL_HANDLE))
    , physicalDevice(std::exchange(other.physicalDevice, VK_NULL_HANDLE))
    , image(std::exchange(other.image, VK_NULL_HANDLE))
    , memory(std::exchange(other.memory, VK_NULL_HANDLE))
    , desiredProps(std::exchange(other.desiredProps, VkMemoryPropertyFlags{}))
    , lifetimeClass_(std::exchange(other.lifetimeClass_, GpuAllocator::LifetimeClass::Persistent))
    , ownedAllocator(std::move(other.ownedAllocator))
    , allocator(std::exchange(other.allocator, nullptr))
    , allocation(std::exchange(other.allocation, GpuAllocator::Allocation{}))
{
}

VulkanImage& VulkanImage::operator=(VulkanImage&& other) noexcept
{
    if (this != &other) {
        if (image != VK_NULL_HANDLE) {
            vkDestroyImage(device, image, nullptr);
        }
        if (memory != VK_NULL_HANDLE) {
            if (allocator) { allocator->free(allocation); }
            else { vkFreeMemory(device, memory, nullptr); }
        }

        device = other.device;
        physicalDevice = other.physicalDevice;
        image = other.image;
        memory = other.memory;
        desiredProps = other.desiredProps;
        lifetimeClass_ = other.lifetimeClass_;
        ownedAllocator = std::move(other.ownedAllocator);
        allocator = other.allocator;
        allocation = other.allocation;

        other.device = VK_NULL_HANDLE;
        other.physicalDevice = VK_NULL_HANDLE;
        other.image = VK_NULL_HANDLE;
        other.memory = VK_NULL_HANDLE;
        other.desiredProps = 0;
        other.lifetimeClass_ = GpuAllocator::LifetimeClass::Persistent;
        other.allocator = nullptr;
        other.allocation = {};
    }
    return *this;
}

VulkanImage::~VulkanImage() noexcept
{
    if (image != VK_NULL_HANDLE) {
        vkDestroyImage(device, image, nullptr);
        image = VK_NULL_HANDLE;
    }
    if (memory != VK_NULL_HANDLE) {
        if (allocator) {
            allocator->free(allocation);
        } else {
            vkFreeMemory(device, memory, nullptr);
        }
        memory = VK_NULL_HANDLE;
        allocation = {};
    }
}

void VulkanImage::allocateAndBind()
{
    VkMemoryDedicatedRequirements dedicatedReq{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS };
    VkMemoryRequirements2 req2{ VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
    req2.pNext = &dedicatedReq;
    VkImageMemoryRequirementsInfo2 reqInfo{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2 };
    reqInfo.image = image;
    vkGetImageMemoryRequirements2(device, &reqInfo, &req2);

    const bool forceDedicated = allocator->shouldUseDedicatedAllocation(
        req2.memoryRequirements,
        dedicatedReq,
        GpuAllocator::ResourceClass::Image,
        lifetimeClass_,
        desiredProps,
        false);

    allocation = allocator->allocateForImage(req2.memoryRequirements, desiredProps, image, forceDedicated, lifetimeClass_);
    memory = allocation.memory;

    const VkResult bindRes = vkBindImageMemory(device, image, memory, allocation.offset);
    if (bindRes != VK_SUCCESS) {
        allocator->free(allocation);
        allocation = {};
        memory = VK_NULL_HANDLE;
        vkutil::throwVkError("vkBindImageMemory", bindRes);
    }
}

// ===================== VulkanImageView =====================

VulkanImageView::VulkanImageView(VkDevice device,
    VkImage image,
    VkFormat format,
    VkImageAspectFlags aspectFlags,
    uint32_t mipLevels)
    : handle()
{
    if (device == VK_NULL_HANDLE) {
        throw std::runtime_error("VulkanImageView: device is VK_NULL_HANDLE");
    }
    if (image == VK_NULL_HANDLE) {
        throw std::runtime_error("VulkanImageView: image is VK_NULL_HANDLE");
    }

    VkImageViewCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    ci.image = image;
    ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ci.format = format;

    ci.subresourceRange.aspectMask = aspectFlags;
    ci.subresourceRange.baseMipLevel = 0;
    ci.subresourceRange.levelCount = std::max(1u, mipLevels);
    ci.subresourceRange.baseArrayLayer = 0;
    ci.subresourceRange.layerCount = 1;

    VkImageView view = VK_NULL_HANDLE;
    const VkResult res = vkCreateImageView(device, &ci, nullptr, &view);
    if (res != VK_SUCCESS) {
        vkutil::throwVkError("vkCreateImageView", res);
    }

    handle = DeferredDeletionService::instance().makeDeferredHandle<VkImageView, PFN_vkDestroyImageView>(device, view, vkDestroyImageView);
}

// ===================== VulkanFramebuffer =====================

VulkanFramebuffer::VulkanFramebuffer(VkDevice device,
    VkRenderPass renderPass,
    const std::vector<VkImageView>& attachments,
    uint32_t width,
    uint32_t height)
    : handle()
{
    if (device == VK_NULL_HANDLE) {
        throw std::runtime_error("VulkanFramebuffer: device is VK_NULL_HANDLE");
    }
    if (renderPass == VK_NULL_HANDLE) {
        throw std::runtime_error("VulkanFramebuffer: renderPass is VK_NULL_HANDLE");
    }
    if (attachments.empty()) {
        throw std::runtime_error("VulkanFramebuffer: attachments is empty");
    }

    VkFramebufferCreateInfo ci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    ci.renderPass = renderPass;
    ci.attachmentCount = static_cast<uint32_t>(attachments.size());
    ci.pAttachments = attachments.data();
    ci.width = width;
    ci.height = height;
    ci.layers = 1;

    VkFramebuffer fb = VK_NULL_HANDLE;
    const VkResult res = vkCreateFramebuffer(device, &ci, nullptr, &fb);
    if (res != VK_SUCCESS) {
        vkutil::throwVkError("vkCreateFramebuffer", res);
    }

    handle = DeferredDeletionService::instance().makeDeferredHandle<VkFramebuffer, PFN_vkDestroyFramebuffer>(device, fb, vkDestroyFramebuffer);
}

// ===================== VulkanSwapchain =====================

static VkCompositeAlphaFlagBitsKHR pickCompositeAlpha(const VkSurfaceCapabilitiesKHR& caps)
{
    const VkCompositeAlphaFlagBitsKHR prefs[] = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
    };

    for (const auto p : prefs) {
        if (caps.supportedCompositeAlpha & p) return p;
    }

    return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

VulkanSwapchain::VulkanSwapchain(VkDevice device,
    VkSurfaceKHR surface,
    uint32_t width,
    uint32_t height,
    const VkSurfaceFormatKHR& surfaceFormat,
    VkPresentModeKHR presentMode,
    const VkSurfaceCapabilitiesKHR& capabilities,
    uint32_t graphicsFamily,
    uint32_t presentFamily,
    VkSwapchainKHR oldSwapchain,
    VkImageUsageFlags usage)
    : handle()
    , images()
    , imageFormat(VK_FORMAT_UNDEFINED)
    , extent{}
{
    create(device, surface, width, height, surfaceFormat, presentMode, capabilities,
        graphicsFamily, presentFamily, oldSwapchain, usage);
}

void VulkanSwapchain::create(VkDevice device,
    VkSurfaceKHR surface,
    uint32_t width,
    uint32_t height,
    const VkSurfaceFormatKHR& surfaceFormat,
    VkPresentModeKHR presentMode,
    const VkSurfaceCapabilitiesKHR& capabilities,
    uint32_t graphicsFamily,
    uint32_t presentFamily,
    VkSwapchainKHR oldSwapchain,
    VkImageUsageFlags usage)
{
    if (device == VK_NULL_HANDLE) {
        throw std::runtime_error("VulkanSwapchain: device is VK_NULL_HANDLE");
    }
    if (surface == VK_NULL_HANDLE) {
        throw std::runtime_error("VulkanSwapchain: surface is VK_NULL_HANDLE");
    }

    // Reset any previous swapchain owned by this object.
    handle.reset();
    images.clear();
    imageFormat = VK_FORMAT_UNDEFINED;
    extent = {};

    const uint32_t preferred = (presentMode == VK_PRESENT_MODE_MAILBOX_KHR)
        ? 3u
        : (capabilities.minImageCount + 1u);

    uint32_t imageCount = std::max(capabilities.minImageCount, preferred);

    if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount) {
        imageCount = capabilities.maxImageCount;
    }

    VkExtent2D actualExtent = capabilities.currentExtent;
    if (actualExtent.width == UINT32_MAX) {
        actualExtent.width = std::clamp(width,
            capabilities.minImageExtent.width,
            capabilities.maxImageExtent.width);
        actualExtent.height = std::clamp(height,
            capabilities.minImageExtent.height,
            capabilities.maxImageExtent.height);
    }

    const VkSurfaceTransformFlagBitsKHR preTransform =
        (capabilities.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) != 0
        ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
        : capabilities.currentTransform;

    const VkCompositeAlphaFlagBitsKHR compositeAlpha = pickCompositeAlpha(capabilities);

    if ((usage & capabilities.supportedUsageFlags) != usage) {
        std::string msg = "VulkanSwapchain: requested image usage not fully supported (requested=";
        msg += std::to_string(static_cast<uint32_t>(usage));
        msg += ", supported=";
        msg += std::to_string(static_cast<uint32_t>(capabilities.supportedUsageFlags));
        msg += ")";
        throw std::runtime_error(msg);
    }

    VkSwapchainCreateInfoKHR ci{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    ci.surface = surface;
    ci.minImageCount = imageCount;
    ci.imageFormat = surfaceFormat.format;
    ci.imageColorSpace = surfaceFormat.colorSpace;
    ci.imageExtent = actualExtent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = usage;
    ci.preTransform = preTransform;
    ci.compositeAlpha = compositeAlpha;
    ci.presentMode = presentMode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = oldSwapchain;

    const std::array<uint32_t, 2> familyIndices{ graphicsFamily, presentFamily };
    const bool concurrentSharing = (graphicsFamily != presentFamily);
    ci.imageSharingMode = concurrentSharing ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
    ci.queueFamilyIndexCount = concurrentSharing ? static_cast<uint32_t>(familyIndices.size()) : 0;
    ci.pQueueFamilyIndices = concurrentSharing ? familyIndices.data() : nullptr;

    VkSwapchainKHR sc = VK_NULL_HANDLE;
    const VkResult createRes = vkCreateSwapchainKHR(device, &ci, nullptr, &sc);
    if (createRes != VK_SUCCESS) {
        vkutil::throwVkError("vkCreateSwapchainKHR", createRes);
    }

    handle = DeferredDeletionService::instance().makeDeferredHandle<VkSwapchainKHR, PFN_vkDestroySwapchainKHR>(device, sc, vkDestroySwapchainKHR);

    uint32_t count = 0;
    VkResult res = vkGetSwapchainImagesKHR(device, sc, &count, nullptr);
    if (res != VK_SUCCESS) {
        std::string msg = "VulkanSwapchain: vkGetSwapchainImagesKHR(count) failed (";
        msg += vkutil::vkResultToString(res);
        msg += ")";
        throw std::runtime_error(msg);
    }

    images.resize(count);
    if (count > 0) {
        res = vkGetSwapchainImagesKHR(device, sc, &count, images.data());
        if (res != VK_SUCCESS) {
            std::string msg = "VulkanSwapchain: vkGetSwapchainImagesKHR(data) failed (";
            msg += vkutil::vkResultToString(res);
            msg += ")";
            throw std::runtime_error(msg);
        }
    }

    imageFormat = surfaceFormat.format;
    extent = actualExtent;
}
