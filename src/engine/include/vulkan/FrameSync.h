// ==============================
// FrameSync.h
// ==============================
#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

// parasoft-begin-suppress ALL "suppress all violations"
#include <vulkan/vulkan.h>
// parasoft-end-suppress ALL "suppress all violations"

 #include "VkCommands.h"
 #include "VkCore.h" // VulkanQueue
 #include "VkSync.h"
class FrameSync
{
public:
    enum class WaitPolicy : uint8_t {
        Wait,
        Poll,
        Timed
    };

    struct RuntimeConfig {
        uint32_t framesInFlight{ 2 };
        WaitPolicy frameReuseWaitPolicy{ WaitPolicy::Wait };
        uint64_t frameReuseWaitTimeoutNs{ UINT64_MAX };
        uint64_t acquireTimeoutNs{ UINT64_MAX };
    };

    struct Diagnostics {
        uint64_t frameReuseWaitCount{ 0 };
        uint64_t frameReuseWaitedNs{ 0 };
        uint64_t frameReuseTimeoutCount{ 0 };
        uint64_t acquireTimeoutCount{ 0 };
        uint64_t framesSkipped{ 0 };
    };

    FrameSync() = default;
    ~FrameSync() noexcept { cleanup(); }

    FrameSync(const FrameSync&) = delete;
    FrameSync& operator=(const FrameSync&) = delete;

    FrameSync(FrameSync&&) noexcept = default;
    FrameSync& operator=(FrameSync&&) noexcept = default;

    void init(VkDevice device,
        uint32_t graphicsFamilyIndex,
        size_t swapchainImageCount,
        bool enableValidation,
        RuntimeConfig config = {});

    void cleanup() noexcept;

    [[nodiscard]] bool valid() const noexcept { return device != VK_NULL_HANDLE && cmdArena != nullptr && timelineSem != nullptr && uploadTimeline != nullptr; }

    struct Frame
    {
        Frame() = default;
        Frame(const Frame&) = default;
        Frame& operator=(const Frame&) = default;
        Frame(Frame&&) noexcept = default;
        Frame& operator=(Frame&&) noexcept = default;

        uint32_t frameIndex = 0;          // 0..framesInFlight-1
        uint32_t imageIndex = 0;          // 0..swapchainImageCount-1
        VkCommandBuffer cmdBuffer = VK_NULL_HANDLE;
    };

    struct Garbage
    {
        std::vector<VulkanSemaphore> oldPresentSems;
        [[nodiscard]] bool empty() const noexcept { return oldPresentSems.empty(); }
    };

    // Swapchain recreation changes the swapchain image count, so we rebuild present semaphores.
    void recreateForSwapchain(size_t newSwapchainImageCount, Garbage& outGarbage);

    // Wait for CPU reuse, acquire swapchain image.
    [[nodiscard]] VkResult acquireFrame(VkSwapchainKHR swapchain, Frame& outFrame);

    // Submit & present for a given frame.
    [[nodiscard]] VkResult submitAndPresent(const VulkanQueue& graphicsQ,
        const VulkanQueue& presentQ,
        VkSwapchainKHR swapchain,
        const Frame& frame,
        uint64_t uploadWaitValue = 0);

    [[nodiscard]] VkCommandBuffer currentCommandBuffer(const Frame& frame) const noexcept
    {
        return frame.cmdBuffer;
    }

    [[nodiscard]] VkSemaphore imageAvailable(uint32_t frameIndex) const;

    void makeRenderPassBegin(const Frame& frame,
        VkRenderPass renderPass,
        VkFramebuffer framebuffer,
        const VkExtent2D& extent,
        const VkClearValue* clearValues,
        uint32_t clearCount,
        VkRenderPassBeginInfo& outInfo) const noexcept;

    // Timeline helpers (for deferred destruction / garbage collection)
    [[nodiscard]] uint64_t lastSubmittedValue() const noexcept { return currentValue; }
    [[nodiscard]] vkutil::VkExpected<uint64_t> completedValue() const;
    [[nodiscard]] uint64_t maxInFlightValue() const noexcept;
    [[nodiscard]] uint64_t nextUploadValue();
    [[nodiscard]] vkutil::VkExpected<uint64_t> uploadCompletedValue() const;
    [[nodiscard]] VkSemaphore uploadSemaphore() const;

    [[nodiscard]] uint32_t frameCount() const noexcept { return framesInFlight_; }
    [[nodiscard]] const Diagnostics& diagnostics() const noexcept { return diagnostics_; }
    [[nodiscard]] size_t   swapchainImageCount() const noexcept { return swapImageCount; }

private:
    void cleanupUnlocked() noexcept;
    mutable std::mutex stateMutex_{};
    VkDevice device{ VK_NULL_HANDLE };

    std::unique_ptr<VulkanCommandArena> cmdArena;
    std::vector<VkCommandBuffer>        cmdBuffers;

    std::vector<VulkanSemaphore> imageAvailableSems; // per frame-in-flight
    std::vector<VulkanSemaphore> presentSems;        // per swapchain image

    std::unique_ptr<VulkanSemaphore> timelineSem;
    std::unique_ptr<TimelineSemaphore> uploadTimeline;
    uint64_t currentValue = 0;
    uint64_t uploadValue = 0;
    std::vector<uint64_t> frameValues;               // per frame-in-flight timeline values

    uint32_t currentFrame = 0;
    uint32_t framesInFlight_{ 2 };
    bool validation = false;

    size_t swapImageCount = 0;
    RuntimeConfig config_{};
    Diagnostics diagnostics_{};

private:
    void nameObjects() const;
};
