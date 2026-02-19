// VkSwapchain.h
#pragma once

#include <cstdint>
#include <stdexcept>
#include <vector>
#include <memory>
#include <algorithm> // std::max, std::clamp

// parasoft-begin-suppress ALL "suppress all violations"
#include <vulkan/vulkan.h>
// parasoft-end-suppress ALL "suppress all violations"

 #include "UniqueHandle.h"
 #include "GpuAllocator.h"
 #include "VkUtils.h"
// ===================== Image =====================
// NOTE: Per your rule: we do NOT use UniqueHandle for VkImage (or VkBuffer).
// VulkanImage owns VkImage + VkDeviceMemory manually.

class VulkanImage {
public:
    VulkanImage() = default;

    VulkanImage(VkDevice device,
        VkPhysicalDevice physicalDevice,
        const VkImageCreateInfo& createInfo,
        VkMemoryPropertyFlags memoryProps = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        GpuAllocator::LifetimeClass lifetimeClass = GpuAllocator::LifetimeClass::Persistent);

    VulkanImage(GpuAllocator& allocator,
        const VkImageCreateInfo& createInfo,
        VkMemoryPropertyFlags memoryProps = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        GpuAllocator::LifetimeClass lifetimeClass = GpuAllocator::LifetimeClass::Persistent);

    [[nodiscard]] static vkutil::VkExpected<VulkanImage> createResult(VkDevice device,
        VkPhysicalDevice physicalDevice,
        const VkImageCreateInfo& createInfo,
        VkMemoryPropertyFlags memoryProps = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        GpuAllocator::LifetimeClass lifetimeClass = GpuAllocator::LifetimeClass::Persistent);

    [[nodiscard]] static vkutil::VkExpected<VulkanImage> createResult(GpuAllocator& allocator,
        const VkImageCreateInfo& createInfo,
        VkMemoryPropertyFlags memoryProps = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        GpuAllocator::LifetimeClass lifetimeClass = GpuAllocator::LifetimeClass::Persistent);

    VulkanImage(const VulkanImage&) = delete;
    VulkanImage& operator=(const VulkanImage&) = delete;

    VulkanImage(VulkanImage&& other) noexcept;
    VulkanImage& operator=(VulkanImage&& other) noexcept;

    ~VulkanImage() noexcept;

    [[nodiscard]] VkImage        get()       const noexcept { return image; }
    [[nodiscard]] VkDeviceMemory getMemory() const noexcept { return memory; }
    [[nodiscard]] bool           valid()     const noexcept { return image != VK_NULL_HANDLE; }

private:
    VkDevice              device{ VK_NULL_HANDLE };
    VkPhysicalDevice      physicalDevice{ VK_NULL_HANDLE };
    VkImage               image{ VK_NULL_HANDLE };
    VkDeviceMemory        memory{ VK_NULL_HANDLE };
    VkMemoryPropertyFlags desiredProps{};
    GpuAllocator::LifetimeClass lifetimeClass_{ GpuAllocator::LifetimeClass::Persistent };

    std::unique_ptr<GpuAllocator> ownedAllocator{};
    GpuAllocator* allocator{ nullptr };
    GpuAllocator::Allocation allocation{};

    void     allocateAndBind();
};

// ===================== Image view =====================
// Owns VkImageView (device-owned) via UniqueHandle.

class VulkanImageView {
public:
    VulkanImageView() noexcept = default;

    VulkanImageView(VkDevice device,
        VkImage image,
        VkFormat format,
        VkImageAspectFlags aspectFlags,
        uint32_t mipLevels = 1);

    VulkanImageView(const VulkanImageView&) = delete;
    VulkanImageView& operator=(const VulkanImageView&) = delete;

    VulkanImageView(VulkanImageView&&) noexcept = default;
    VulkanImageView& operator=(VulkanImageView&&) noexcept = default;

    ~VulkanImageView() = default;

    [[nodiscard]] VkImageView get() const noexcept { return handle.get(); }
    [[nodiscard]] VkDevice    getDevice() const noexcept { return handle.getDevice(); }
    [[nodiscard]] bool        valid() const noexcept { return static_cast<bool>(handle); }

private:
    vkhandle::DeviceUniqueHandle<VkImageView, PFN_vkDestroyImageView> handle;
};

// ===================== Framebuffer =====================
// Owns VkFramebuffer (device-owned) via UniqueHandle.

class VulkanFramebuffer {
public:
    VulkanFramebuffer() noexcept = default;

    VulkanFramebuffer(VkDevice device,
        VkRenderPass renderPass,
        const std::vector<VkImageView>& attachments,
        uint32_t width,
        uint32_t height);

    VulkanFramebuffer(const VulkanFramebuffer&) = delete;
    VulkanFramebuffer& operator=(const VulkanFramebuffer&) = delete;

    VulkanFramebuffer(VulkanFramebuffer&&) noexcept = default;
    VulkanFramebuffer& operator=(VulkanFramebuffer&&) noexcept = default;

    ~VulkanFramebuffer() = default;

    [[nodiscard]] VkFramebuffer get() const noexcept { return handle.get(); }
    [[nodiscard]] VkDevice      getDevice() const noexcept { return handle.getDevice(); }
    [[nodiscard]] bool          valid() const noexcept { return static_cast<bool>(handle); }

private:
    vkhandle::DeviceUniqueHandle<VkFramebuffer, PFN_vkDestroyFramebuffer> handle;
};

// ===================== Swapchain =====================
// Owns VkSwapchainKHR (device-owned) via UniqueHandle.

class VulkanSwapchain {
public:
    VulkanSwapchain() = default;

    VulkanSwapchain(VkDevice device,
        VkSurfaceKHR surface,
        uint32_t width,
        uint32_t height,
        const VkSurfaceFormatKHR& surfaceFormat,
        VkPresentModeKHR presentMode,
        const VkSurfaceCapabilitiesKHR& capabilities,
        uint32_t graphicsFamily,
        uint32_t presentFamily,
        VkSwapchainKHR oldSwapchain = VK_NULL_HANDLE,
        VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);

    VulkanSwapchain(const VulkanSwapchain&) = delete;
    VulkanSwapchain& operator=(const VulkanSwapchain&) = delete;

    VulkanSwapchain(VulkanSwapchain&&) noexcept = default;
    VulkanSwapchain& operator=(VulkanSwapchain&&) noexcept = default;

    ~VulkanSwapchain() = default;

    [[nodiscard]] VkSwapchainKHR              get()            const noexcept { return handle.get(); }
    [[nodiscard]] VkDevice                    getDevice()      const noexcept { return handle.getDevice(); }
    [[nodiscard]] bool                        valid()          const noexcept { return static_cast<bool>(handle); }
    [[nodiscard]] const std::vector<VkImage>& getImages()      const noexcept { return images; }
    [[nodiscard]] VkFormat                    getImageFormat() const noexcept { return imageFormat; }
    [[nodiscard]] VkExtent2D                  getExtent()      const noexcept { return extent; }

private:
    vkhandle::DeviceUniqueHandle<VkSwapchainKHR, PFN_vkDestroySwapchainKHR> handle;
    std::vector<VkImage> images;
    VkFormat             imageFormat{ VK_FORMAT_UNDEFINED };
    VkExtent2D           extent{};

    void create(VkDevice device,
        VkSurfaceKHR surface,
        uint32_t width,
        uint32_t height,
        const VkSurfaceFormatKHR& surfaceFormat,
        VkPresentModeKHR presentMode,
        const VkSurfaceCapabilitiesKHR& capabilities,
        uint32_t graphicsFamily,
        uint32_t presentFamily,
        VkSwapchainKHR oldSwapchain,
        VkImageUsageFlags usage);
};
