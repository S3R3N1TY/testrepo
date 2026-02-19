#pragma once

#include <cstdint>
#include <stdexcept>
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <memory>

// parasoft-begin-suppress ALL "suppress all violations"
#include <vulkan/vulkan.h>
// parasoft-end-suppress ALL "suppress all violations"

#include "UniqueHandle.h"
#include "VkCore.h"
#include "VkUtils.h"

class VulkanSemaphore {
public:
    VulkanSemaphore() noexcept = default;
    explicit VulkanSemaphore(VkDevice device, bool timeline = false);
    [[nodiscard]] static vkutil::VkExpected<VulkanSemaphore> createResult(VkDevice device, bool timeline = false);

    VulkanSemaphore(const VulkanSemaphore&) = delete;
    VulkanSemaphore& operator=(const VulkanSemaphore&) = delete;

    VulkanSemaphore(VulkanSemaphore&&) noexcept = default;
    VulkanSemaphore& operator=(VulkanSemaphore&&) noexcept = default;

    ~VulkanSemaphore() = default;

    [[nodiscard]] VkSemaphore get() const noexcept { return handle.get(); }
    [[nodiscard]] VkDevice    getDevice() const noexcept { return handle.getDevice(); }
    [[nodiscard]] bool        valid() const noexcept { return static_cast<bool>(handle); }

private:
    vkhandle::DeviceUniqueHandle<VkSemaphore, PFN_vkDestroySemaphore> handle;
};

class TimelineSemaphore {
public:
    TimelineSemaphore() noexcept = default;
    explicit TimelineSemaphore(VkDevice device, uint64_t initialValue = 0);
    [[nodiscard]] static vkutil::VkExpected<TimelineSemaphore> createResult(VkDevice device, uint64_t initialValue = 0);

    TimelineSemaphore(const TimelineSemaphore&) = delete;
    TimelineSemaphore& operator=(const TimelineSemaphore&) = delete;

    TimelineSemaphore(TimelineSemaphore&&) noexcept = default;
    TimelineSemaphore& operator=(TimelineSemaphore&&) noexcept = default;

    ~TimelineSemaphore() = default;

    [[nodiscard]] VkSemaphore get() const noexcept { return handle.get(); }
    [[nodiscard]] VkDevice    getDevice() const noexcept { return handle.getDevice(); }
    [[nodiscard]] bool        valid() const noexcept { return static_cast<bool>(handle); }

    [[nodiscard]] vkutil::VkExpected<uint64_t> value() const;
    [[nodiscard]] vkutil::VkExpected<void> signal(uint64_t value);
    [[nodiscard]] vkutil::VkExpected<void> wait(uint64_t value, uint64_t timeout = UINT64_MAX) const;

private:
    vkhandle::DeviceUniqueHandle<VkSemaphore, PFN_vkDestroySemaphore> handle;
};

class VulkanFence {
public:
    VulkanFence() noexcept = default;

    explicit VulkanFence(VkDevice device, VkFenceCreateFlags flags = 0);
    [[nodiscard]] static vkutil::VkExpected<VulkanFence> createResult(VkDevice device, VkFenceCreateFlags flags = 0);

    VulkanFence(const VulkanFence&) = delete;
    VulkanFence& operator=(const VulkanFence&) = delete;

    VulkanFence(VulkanFence&&) noexcept = default;
    VulkanFence& operator=(VulkanFence&&) noexcept = default;

    ~VulkanFence() = default;

    [[nodiscard]] VkFence  get() const noexcept { return handle.get(); }
    [[nodiscard]] VkDevice getDevice() const noexcept { return handle.getDevice(); }
    [[nodiscard]] bool     valid() const noexcept { return static_cast<bool>(handle); }

    [[nodiscard]] vkutil::VkExpected<void> resetResult();
    [[nodiscard]] vkutil::VkExpected<bool> waitResult(uint64_t timeout = UINT64_MAX);
    void reset();
    [[nodiscard]] VkResult wait(uint64_t timeout = UINT64_MAX);

private:
    vkhandle::DeviceUniqueHandle<VkFence, PFN_vkDestroyFence> handle;
};

struct SyncTicket {
    uint64_t value{ 0 };
    uint32_t frameIndex{ 0 };
};


enum class FenceWaitPolicy : uint8_t {
    Poll,
    Wait,
    AssertSignaled
};

struct SubmitFrameSyncPolicy {
    FenceWaitPolicy fenceWaitPolicy{ FenceWaitPolicy::Wait };
    uint64_t waitTimeout{ UINT64_MAX };
};

enum class SyncDependencyClass : uint8_t {
    Graphics,
    Compute,
    Transfer,
    Host,
    Generic
};

struct SyncSubmitInfo {
    std::vector<SyncTicket> waitTickets;
    std::vector<VkSemaphore> externalWaitSemaphores;
    std::vector<VkPipelineStageFlags2> externalWaitStages;
    std::vector<SyncDependencyClass> externalWaitDependencies;
    std::vector<VkCommandBuffer> commandBuffers;
    std::vector<VkSemaphore> externalSignalSemaphores;
    const char* debugLabel{ nullptr };
    VkPipelineStageFlags2 timelineWaitStageMask{ 0 };
    VkPipelineStageFlags2 timelineSignalStageMask{ 0 };
    VkPipelineStageFlags2 externalSignalStageMask{ 0 };
    SyncDependencyClass timelineWaitDependency{ SyncDependencyClass::Generic };
    SyncDependencyClass timelineSignalDependency{ SyncDependencyClass::Generic };
    SyncDependencyClass externalSignalDependency{ SyncDependencyClass::Generic };
    bool allowAllCommandsFallback{ false };
};

class SyncContext {
public:
    enum class SubmitBackend : uint8_t {
        Submit2,
        LegacySubmit
    };

    SyncContext() noexcept = default;
    SyncContext(VkDevice device,
        uint32_t framesInFlight,
        bool timelineSupported,
        bool synchronization2Enabled,
        VkPipelineStageFlags2 defaultTimelineWaitStage = 0,
        VkPipelineStageFlags2 defaultTimelineSignalStage = 0,
        VkPipelineStageFlags2 defaultExternalSignalStage = 0);

    SyncContext(const SyncContext&) = delete;
    SyncContext& operator=(const SyncContext&) = delete;
    SyncContext(SyncContext&& other) noexcept;
    SyncContext& operator=(SyncContext&& other) noexcept;

    [[nodiscard]] static vkutil::VkExpected<SyncContext> createResult(VkDevice device,
        uint32_t framesInFlight,
        bool timelineSupported,
        bool synchronization2Enabled,
        VkPipelineStageFlags2 defaultTimelineWaitStage = 0,
        VkPipelineStageFlags2 defaultTimelineSignalStage = 0,
        VkPipelineStageFlags2 defaultExternalSignalStage = 0);

    [[nodiscard]] bool timelineMode() const noexcept { return timelineMode_.load(std::memory_order_acquire); }
    [[nodiscard]] SubmitBackend submitBackend() const noexcept { return submitBackend_.load(std::memory_order_acquire); }
    [[nodiscard]] uint32_t framesInFlight() const noexcept { return framesInFlight_.load(std::memory_order_acquire); }

    [[nodiscard]] vkutil::VkExpected<SyncTicket> submit(const VulkanQueue& queue,
        uint32_t frameIndex,
        const SyncSubmitInfo& submitInfo,
        VkFence explicitFence = VK_NULL_HANDLE,
        SubmitFrameSyncPolicy frameSyncPolicy = {});

    [[nodiscard]] vkutil::VkExpected<bool> waitTicket(const SyncTicket& ticket, uint64_t timeout = UINT64_MAX) const;
    [[nodiscard]] vkutil::VkExpected<bool> waitFrame(uint32_t frameIndex, uint64_t timeout = UINT64_MAX);

    [[nodiscard]] vkutil::VkExpected<bool> isTicketComplete(const SyncTicket& ticket) const;
    [[nodiscard]] vkutil::VkExpected<bool> isFrameComplete(uint32_t frameIndex) const;
    [[nodiscard]] vkutil::VkExpected<bool> pollFenceComplete(uint32_t frameIndex) const;
    [[nodiscard]] vkutil::VkExpected<bool> waitFence(uint32_t frameIndex, uint64_t timeout = UINT64_MAX);
    [[nodiscard]] vkutil::VkExpected<void> prepareFrameForSubmit(uint32_t frameIndex, SubmitFrameSyncPolicy policy = {});

    [[nodiscard]] vkutil::VkExpected<void> resetFrame(uint32_t frameIndex);
    void setStagePolicy(VkPipelineStageFlags2 timelineWaitStage,
        VkPipelineStageFlags2 timelineSignalStage,
        VkPipelineStageFlags2 externalSignalStage) noexcept;

private:
    VkDevice device_{ VK_NULL_HANDLE };
    std::atomic<uint32_t> framesInFlight_{ 0 };
    std::atomic<bool> timelineMode_{ false };
    std::atomic<bool> synchronization2Enabled_{ false };
    std::atomic<SubmitBackend> submitBackend_{ SubmitBackend::LegacySubmit };

    mutable std::shared_mutex stateMutex_{};

    TimelineSemaphore timeline_{};
    std::atomic<uint64_t> nextTimelineValue_{ 1 };
    std::vector<std::shared_ptr<std::atomic<uint64_t>>> timelineFrameValues_{};

    std::vector<VulkanFence> frameFences_{};
    std::vector<std::shared_ptr<std::atomic<uint64_t>>> frameSubmittedValues_{};
    mutable std::vector<std::shared_ptr<std::atomic<uint64_t>>> frameCompletedValues_{};
    [[nodiscard]] vkutil::VkExpected<void> init(VkDevice device,
        uint32_t framesInFlight,
        bool timelineSupported,
        bool synchronization2Enabled,
        VkPipelineStageFlags2 defaultTimelineWaitStage,
        VkPipelineStageFlags2 defaultTimelineSignalStage,
        VkPipelineStageFlags2 defaultExternalSignalStage);

    std::atomic<VkPipelineStageFlags2> defaultTimelineWaitStage_{ 0 };
    std::atomic<VkPipelineStageFlags2> defaultTimelineSignalStage_{ 0 };
    std::atomic<VkPipelineStageFlags2> defaultExternalSignalStage_{ 0 };

    [[nodiscard]] static std::vector<std::shared_ptr<std::atomic<uint64_t>>> makeAtomicFrameValues(uint32_t framesInFlight, uint64_t initialValue);
};

[[nodiscard]] vkutil::VkExpected<void> submitWithTimeline2(const VulkanQueue& queue,
    const VkSubmitInfo2& submitInfo,
    VkFence fence = VK_NULL_HANDLE);
