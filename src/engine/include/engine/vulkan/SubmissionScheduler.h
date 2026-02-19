#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// parasoft-begin-suppress ALL "suppress all violations"
#include <vulkan/vulkan.h>
// parasoft-end-suppress ALL "suppress all violations"

#include "DeviceContext.h"
#include "VkSync.h"
#include "VkUtils.h"

class SubmissionScheduler {
public:
    enum class QueueClass : uint8_t {
        Graphics,
        Transfer,
        Compute
    };

    using JobId = size_t;

    struct SchedulerPolicy {
        bool allowComputeOnGraphicsFallback{ false };
        bool requireDedicatedComputeQueue{ false };
    };

    struct JobRequest {
        QueueClass queueClass{ QueueClass::Graphics };
        std::vector<VkCommandBuffer> commandBuffers{};
        std::vector<VkSemaphore> waitSemaphores{};
        std::vector<VkPipelineStageFlags2> waitStages{};
        std::vector<VkSemaphore> signalSemaphores{};
        VkFence fence{ VK_NULL_HANDLE };
        const char* debugLabel{ "submission_scheduler_job" };
    };

    struct PresentRequest {
        VkSwapchainKHR swapchain{ VK_NULL_HANDLE };
        uint32_t imageIndex{ 0 };
        std::vector<VkSemaphore> waitSemaphores{};
    };

    struct FrameExecutionResult {
        VkResult presentResult{ VK_SUCCESS };
        uint32_t submittedJobCount{ 0 };
        uint32_t submitBatchCount{ 0 };
        uint32_t autoSemaphoreCount{ 0 };
        bool usedTimelineSubmission{ false };
        bool usedComputeToGraphicsFallback{ false };
        bool computeQueueAvailable{ false };
        bool computeQueueDedicated{ false };
    };

    explicit SubmissionScheduler(const DeviceContext& deviceContext, SchedulerPolicy policy = {}) noexcept
        : deviceContext_(&deviceContext), policy_(policy)
    {
    }

    void beginFrame();

    [[nodiscard]] vkutil::VkExpected<JobId> enqueueJob(const JobRequest& request);

    [[nodiscard]] vkutil::VkExpected<void> enqueueDependency(
        JobId producer,
        JobId consumer,
        VkSemaphore dependencySemaphore = VK_NULL_HANDLE,
        VkPipelineStageFlags2 consumerWaitStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);

    [[nodiscard]] vkutil::VkExpected<void> enqueuePresent(const PresentRequest& request);

    [[nodiscard]] vkutil::VkExpected<FrameExecutionResult> executeFrame();

private:
    struct EnqueuedJob {
        JobId id{ 0 };
        JobRequest request{};
    };

    struct DependencyEdge {
        JobId producer{ 0 };
        JobId consumer{ 0 };
        VkSemaphore semaphore{ VK_NULL_HANDLE };
        VkPipelineStageFlags2 consumerWaitStage{ VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT };
    };

    struct PendingAutoSemaphore {
        VulkanSemaphore semaphore{};
        VkFence retireFence{ VK_NULL_HANDLE };
    };

    struct PreparedJob {
        JobId id{ 0 };
        QueueClass queueClass{ QueueClass::Graphics };
        std::vector<VkCommandBuffer> commandBuffers{};
        std::vector<VkSemaphore> waitSemaphores{};
        std::vector<VkPipelineStageFlags2> waitStages{};
        std::vector<VkSemaphore> signalSemaphores{};
        VkFence fence{ VK_NULL_HANDLE };
        const char* debugLabel{ "submission_scheduler_job" };
    };

    enum class DependencyRuntimeMode : uint8_t {
        TimelinePrimary,
        BinaryFallback
    };

    struct SubmitBatch {
        struct SubmitEntry {
            std::vector<VkPipelineStageFlags> waitStagesLegacy{};
            VkSubmitInfo submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        };

        QueueClass queueClass{ QueueClass::Graphics };
        std::vector<SubmitEntry> entries{};
        std::vector<VkSubmitInfo> submitInfos{};
        VkFence fence{ VK_NULL_HANDLE };
        const char* debugLabel{ "submission_scheduler_batch" };
    };

    struct SubmitBatch2 {
        struct SubmitEntry {
            std::vector<VkSemaphoreSubmitInfo> waitInfos{};
            std::vector<VkSemaphoreSubmitInfo> signalInfos{};
            std::vector<VkCommandBufferSubmitInfo> commandBufferInfos{};
            VkSubmitInfo2 submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
        };

        QueueClass queueClass{ QueueClass::Graphics };
        std::vector<SubmitEntry> entries{};
        std::vector<VkSubmitInfo2> submitInfos{};
        VkFence fence{ VK_NULL_HANDLE };
        const char* debugLabel{ "submission_scheduler_batch2" };
    };

    [[nodiscard]] vkutil::VkExpected<void> validateJobRequest(const JobRequest& request) const;
    [[nodiscard]] vkutil::VkExpected<void> validatePresentRequest(const PresentRequest& request) const;
    [[nodiscard]] vkutil::VkExpected<void> reclaimAutoSemaphores();
    [[nodiscard]] vkutil::VkExpected<std::vector<JobId>> topologicalOrder() const;
    [[nodiscard]] vkutil::VkExpected<std::vector<PreparedJob>> buildPreparedJobs(const std::vector<JobId>& topoOrder,
        std::vector<VulkanSemaphore>& frameAutoSemaphores,
        DependencyRuntimeMode runtimeMode);
    [[nodiscard]] vkutil::VkExpected<std::vector<SubmitBatch2>> buildBatches2(const std::vector<PreparedJob>& preparedJobs) const;
    [[nodiscard]] std::vector<SubmitBatch> buildBatches(const std::vector<PreparedJob>& preparedJobs) const;
    [[nodiscard]] vkutil::VkExpected<DeviceContext::QueueSubmissionToken> queueTokenFor(QueueClass queueClass, bool* outUsedComputeFallback = nullptr) const;
    [[nodiscard]] vkutil::VkExpected<FrameExecutionResult> executeFrameWithTimeline(const std::vector<PreparedJob>& preparedJobs);
    [[nodiscard]] vkutil::VkExpected<VulkanQueue> queueForSyncContext(QueueClass queueClass, bool* outUsedComputeFallback = nullptr) const;


    const DeviceContext* deviceContext_{ nullptr };
    SchedulerPolicy policy_{};
    std::vector<EnqueuedJob> jobs_{};
    std::vector<DependencyEdge> dependencies_{};
    std::vector<PendingAutoSemaphore> pendingAutoSemaphores_{};
    PresentRequest presentRequest_{};
    bool hasPresentRequest_{ false };
    uint64_t frameOrdinal_{ 0 };
};
