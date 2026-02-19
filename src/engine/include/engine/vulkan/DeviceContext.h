// DeviceContext.h
#pragma once

#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <atomic>
#include <vector>
#include <stdexcept>

// parasoft-begin-suppress ALL "suppress all violations"
#include <vulkan/vulkan.h>
// parasoft-end-suppress ALL "suppress all violations"

 #include "VkCore.h"
 #include "GpuAllocator.h"
 #include "VkSync.h"
struct GLFWwindow;

struct DeviceRuntimeView
{
    std::shared_ptr<const struct DeviceRuntimeSnapshot> snapshot{};

    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(snapshot); }
    [[nodiscard]] VkInstance instance() const noexcept;
    [[nodiscard]] VkPhysicalDevice physicalDevice() const noexcept;
    [[nodiscard]] VkDevice device() const noexcept;
    [[nodiscard]] VkSurfaceKHR surface() const noexcept;
    [[nodiscard]] VkQueue graphicsQueue() const noexcept;
    [[nodiscard]] VkQueue presentQueue() const noexcept;
    [[nodiscard]] VkQueue transferQueue() const noexcept;
    [[nodiscard]] VkQueue computeQueue() const noexcept;
    [[nodiscard]] const VulkanDeviceCapabilities* capabilities() const noexcept;
    [[nodiscard]] const VulkanInstanceCapabilityProfile* instanceCapabilities() const noexcept;
    [[nodiscard]] uint64_t generation() const noexcept;
};


struct DeviceQueueCapabilityProfile
{
    bool hasGraphicsQueue{ false };
    bool hasPresentQueue{ false };
    bool hasTransferQueue{ false };
    bool hasComputeQueue{ false };
    bool transferQueueDedicated{ false };
    bool computeQueueDedicated{ false };
    uint32_t graphicsFamilyIndex{ UINT32_MAX };
    uint32_t presentFamilyIndex{ UINT32_MAX };
    uint32_t transferFamilyIndex{ UINT32_MAX };
    uint32_t computeFamilyIndex{ UINT32_MAX };
};

struct DeviceRuntimeSnapshot
{
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkQueue presentQueue = VK_NULL_HANDLE;
    VkQueue transferQueue = VK_NULL_HANDLE;
    VkQueue computeQueue = VK_NULL_HANDLE;
    VulkanDeviceCapabilities capabilities{};
    VulkanInstanceCapabilityProfile instanceCapabilities{};
    uint64_t generation = 0;
};

struct DeviceContext
{
    enum class State : uint8_t {
        Uninitialized,
        Alive,
        ShuttingDown,
        Destroyed
    };

    class RuntimeHandle {
    public:
        RuntimeHandle() = default;
        RuntimeHandle(const DeviceContext* owner, uint64_t expectedGeneration) noexcept
            : owner_(owner), generation_(expectedGeneration)
        {
        }
        RuntimeHandle(const DeviceContext* owner, uint64_t expectedGeneration,
            std::shared_ptr<const DeviceRuntimeSnapshot> snapshot) noexcept
            : owner_(owner), generation_(expectedGeneration), snapshot_(std::move(snapshot))
        {
        }

        [[nodiscard]] bool valid() const noexcept;
        [[nodiscard]] DeviceRuntimeView view() const;
        [[nodiscard]] std::shared_ptr<const DeviceRuntimeSnapshot> snapshot() const noexcept { return snapshot_; }
        [[nodiscard]] uint64_t generation() const noexcept { return generation_; }

    private:
        const DeviceContext* owner_ = nullptr;
        uint64_t generation_ = 0;
        std::shared_ptr<const DeviceRuntimeSnapshot> snapshot_{};
    };

    enum class QueueSelection : uint8_t {
        Graphics,
        Present,
        Transfer,
        Compute
    };

    class QueueSubmissionToken {
    public:
        QueueSubmissionToken() = default;
        QueueSubmissionToken(const DeviceContext* owner, uint64_t expectedGeneration, QueueSelection queueSelection) noexcept
            : owner_(owner), generation_(expectedGeneration), queueSelection_(queueSelection)
        {
        }

        [[nodiscard]] bool valid() const noexcept;
        [[nodiscard]] uint64_t generation() const noexcept { return generation_; }
        [[nodiscard]] VkQueue queue() const noexcept;

        [[nodiscard]] vkutil::VkExpected<void> submit(
            const std::vector<VkSubmitInfo>& submitInfos,
            VkFence fence = VK_NULL_HANDLE,
            const char* subsystem = "queue") const;

        [[nodiscard]] vkutil::VkExpected<void> submit2(
            const std::vector<VkSubmitInfo2>& submitInfos,
            VkFence fence = VK_NULL_HANDLE,
            const char* subsystem = "queue") const;

        [[nodiscard]] VkResult present(
            VkSwapchainKHR swapchain,
            uint32_t imageIndex,
            VkSemaphore waitSemaphore = VK_NULL_HANDLE) const;

        [[nodiscard]] VkResult present(
            VkSwapchainKHR swapchain,
            uint32_t imageIndex,
            const std::vector<VkSemaphore>& waitSemaphores) const;

        [[nodiscard]] vkutil::VkExpected<void> waitIdle() const;

    private:
        const DeviceContext* owner_ = nullptr;
        uint64_t generation_ = 0;
        QueueSelection queueSelection_ = QueueSelection::Graphics;
    };

    // Core Vulkan ownership (RAII via your wrapper classes)
    std::unique_ptr<VulkanInstance>            instance;
    std::unique_ptr<VulkanDebugUtilsMessenger> debugMessenger;
    std::unique_ptr<VulkanSurface>             surface;

    std::unique_ptr<VulkanPhysicalDevice> physical;
    std::unique_ptr<VulkanDevice>         device;

    std::unique_ptr<VulkanQueue> graphicsQ;
    std::unique_ptr<VulkanQueue> presentQ;
    std::unique_ptr<VulkanQueue> transferQ;
    std::unique_ptr<VulkanQueue> computeQ;

    bool enableValidation = false;
    VkPhysicalDeviceFeatures supportedFeatures{};
    VkPhysicalDeviceFeatures enabledFeatures{};
    VulkanDeviceCapabilities capabilities{};
    VkPhysicalDeviceProperties physicalProperties{};
    bool samplerAnisotropyEnabled = false;
    float maxSamplerAnisotropy = 1.0f;
    bool timelineSemaphoreSupported = false;
    bool timelineSemaphoreEnabled = false;

    std::unique_ptr<GpuAllocator> gpuAllocator;
    std::unique_ptr<SyncContext> syncContext;

    DeviceContext() = default;
    DeviceContext(GLFWwindow* window, bool enableValidation);

    ~DeviceContext() noexcept;

    DeviceContext(const DeviceContext&) = delete;
    DeviceContext& operator=(const DeviceContext&) = delete;

    DeviceContext(DeviceContext&& other) noexcept;
    DeviceContext& operator=(DeviceContext&& other) noexcept;

    void cleanup() noexcept;

    [[nodiscard]] bool waitDeviceIdle() noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] uint64_t generation() const noexcept { return generation_; }

    [[nodiscard]] RuntimeHandle runtimeHandle() const noexcept;
    [[nodiscard]] DeviceRuntimeView runtimeView() const;
    [[nodiscard]] QueueSubmissionToken graphicsQueueToken() const noexcept;
    [[nodiscard]] QueueSubmissionToken presentQueueToken() const noexcept;
    [[nodiscard]] QueueSubmissionToken transferQueueToken() const noexcept;
    [[nodiscard]] QueueSubmissionToken computeQueueToken() const noexcept;
    [[nodiscard]] const VulkanDeviceCapabilities& deviceCapabilities() const;
    [[nodiscard]] DeviceQueueCapabilityProfile queueCapabilityProfile() const noexcept;

    [[nodiscard]] bool isFeatureSupportedBufferDeviceAddress() const noexcept;
    [[nodiscard]] bool isFeatureEnabledBufferDeviceAddress() const noexcept;
    [[nodiscard]] bool isFeatureSupportedTimelineSemaphore() const noexcept;
    [[nodiscard]] bool isFeatureEnabledTimelineSemaphore() const noexcept;
    [[nodiscard]] bool isFeatureSupportedSynchronization2() const noexcept;
    [[nodiscard]] bool isFeatureEnabledSynchronization2() const noexcept;
    [[nodiscard]] bool isFeatureSupportedDynamicRendering() const noexcept;
    [[nodiscard]] bool isFeatureEnabledDynamicRendering() const noexcept;
    [[nodiscard]] bool isFeatureSupportedDescriptorIndexing() const noexcept;
    [[nodiscard]] bool isFeatureEnabledDescriptorIndexing() const noexcept;

    [[nodiscard]] VkDevice         vkDevice() const;
    [[nodiscard]] VkPhysicalDevice vkPhysical() const;
    [[nodiscard]] VkInstance       vkInstance() const;
    [[nodiscard]] VkSurfaceKHR     vkSurface() const;

    [[nodiscard]] uint32_t graphicsFamilyIndex() const;
    [[nodiscard]] uint32_t presentFamilyIndex() const;
    [[nodiscard]] uint32_t transferFamilyIndex() const;
    [[nodiscard]] uint32_t computeFamilyIndex() const;

    [[nodiscard]] VulkanQueue graphicsQueue() const;
    [[nodiscard]] VulkanQueue presentQueue() const;
    [[nodiscard]] VulkanQueue transferQueue() const;
    [[nodiscard]] VulkanQueue computeQueue() const;

private:
    struct QueueRuntimeSnapshot {
        VulkanQueue queue{};
        uint64_t generation{ 0 };
        bool valid{ false };
    };

    [[nodiscard]] QueueRuntimeSnapshot snapshotQueueForTokenLocked(QueueSelection selection, uint64_t expectedGeneration) const noexcept;
    [[nodiscard]] bool generationMatchesLocked(uint64_t generation) const noexcept;
    [[nodiscard]] bool hasLiveRuntimeLocked() const noexcept;
    [[nodiscard]] bool isShuttingDownFast() const noexcept { return shuttingDownFast_.load(std::memory_order_acquire); }
    [[nodiscard]] std::shared_ptr<const DeviceRuntimeSnapshot> buildRuntimeSnapshotLocked() const;
    void refreshRuntimeSnapshotLocked() noexcept;
    void becomeMovedFrom() noexcept;
    void requireAliveLocked(const char* apiName) const;
    [[nodiscard]] const VulkanQueue* queueForSelectionLocked(QueueSelection selection) const noexcept;

    mutable std::shared_mutex runtimeMutex_{};
    State state_ = State::Uninitialized;
    uint64_t generation_ = 1;
    std::shared_ptr<const DeviceRuntimeSnapshot> runtimeSnapshot_{};
    std::atomic<bool> shuttingDownFast_{ false };

    friend class QueueSubmissionToken;
};
