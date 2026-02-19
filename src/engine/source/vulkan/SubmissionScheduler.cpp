#include "SubmissionScheduler.h"

#include <algorithm>
#include <string>

namespace
{
VkPipelineStageFlags2 defaultWaitStageMask2(SubmissionScheduler::QueueClass queueClass) noexcept
{
    switch (queueClass) {
    case SubmissionScheduler::QueueClass::Graphics:
        return VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
    case SubmissionScheduler::QueueClass::Compute:
        return VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    case SubmissionScheduler::QueueClass::Transfer:
        return VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    default:
        return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    }
}

bool isWaitStageCompatible(SubmissionScheduler::QueueClass queueClass, VkPipelineStageFlags2 stage) noexcept
{
    if (stage == 0) {
        return false;
    }

    if (queueClass == SubmissionScheduler::QueueClass::Graphics) {
        return true;
    }

    constexpr VkPipelineStageFlags2 kGraphicsOnlyStages =
        VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT
        | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT
        | VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT
        | VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT
        | VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT
        | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
        | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT
        | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT
        | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT
        | VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;

    if ((stage & kGraphicsOnlyStages) != 0) {
        return false;
    }

    if (queueClass == SubmissionScheduler::QueueClass::Transfer) {
        constexpr VkPipelineStageFlags2 kTransferAllowed =
            VK_PIPELINE_STAGE_2_TRANSFER_BIT
            | VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT
            | VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT
            | VK_PIPELINE_STAGE_2_HOST_BIT
            | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        return (stage & kTransferAllowed) != 0;
    }

    return true;
}


VkPipelineStageFlags legacyStageMaskForFallback(SubmissionScheduler::QueueClass queueClass, VkPipelineStageFlags2 stageMask) noexcept
{
    const VkPipelineStageFlags2 resolved = stageMask != 0 ? stageMask : defaultWaitStageMask2(queueClass);
    constexpr VkPipelineStageFlags2 kLegacyMaskBits = 0xFFFFFFFFULL;
    if ((resolved & ~kLegacyMaskBits) != 0) {
        return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }

    const VkPipelineStageFlags legacy = static_cast<VkPipelineStageFlags>(resolved & kLegacyMaskBits);
    return legacy != 0 ? legacy : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
}

VkPipelineStageFlags2 signalStageMask2(SubmissionScheduler::QueueClass queueClass) noexcept
{
    switch (queueClass) {
    case SubmissionScheduler::QueueClass::Graphics:
        return VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
    case SubmissionScheduler::QueueClass::Compute:
        return VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    case SubmissionScheduler::QueueClass::Transfer:
        return VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    default:
        return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    }
}
}

void SubmissionScheduler::beginFrame()
{
    const auto reclaimResult = reclaimAutoSemaphores();
    if (!reclaimResult.hasValue()) {
        // best-effort recycle path; scheduler can still progress this frame.
    }

    jobs_.clear();
    dependencies_.clear();
    presentRequest_ = {};
    hasPresentRequest_ = false;
    frameOrdinal_ += 1;
}

vkutil::VkExpected<void> SubmissionScheduler::validateJobRequest(const JobRequest& request) const
{
    if (deviceContext_ == nullptr || !deviceContext_->valid()) {
        return vkutil::makeError("SubmissionScheduler::validateJobRequest", VK_ERROR_INITIALIZATION_FAILED, "submission_scheduler", "invalid_device_context");
    }
    if (request.commandBuffers.empty()) {
        return vkutil::makeError("SubmissionScheduler::validateJobRequest", VK_ERROR_INITIALIZATION_FAILED, "submission_scheduler", "empty_command_buffers");
    }
    for (const VkCommandBuffer cmd : request.commandBuffers) {
        if (cmd == VK_NULL_HANDLE) {
            return vkutil::makeError("SubmissionScheduler::validateJobRequest", VK_ERROR_INITIALIZATION_FAILED, "submission_scheduler", "null_command_buffer");
        }
    }
    if (request.waitSemaphores.size() != request.waitStages.size()) {
        return vkutil::makeError("SubmissionScheduler::validateJobRequest", VK_ERROR_INITIALIZATION_FAILED, "submission_scheduler", "wait_stage_mismatch");
    }

    for (const VkPipelineStageFlags2 waitStage : request.waitStages) {
        if (waitStage == 0) {
            return vkutil::makeError("SubmissionScheduler::validateJobRequest", VK_ERROR_INITIALIZATION_FAILED, "submission_scheduler", "zero_wait_stage");
        }
        if (!isWaitStageCompatible(request.queueClass, waitStage)) {
            return vkutil::makeError("SubmissionScheduler::validateJobRequest", VK_ERROR_INITIALIZATION_FAILED, "submission_scheduler", "invalid_wait_stage_for_queue");
        }
    }
    for (const VkSemaphore sem : request.waitSemaphores) {
        if (sem == VK_NULL_HANDLE) {
            return vkutil::makeError("SubmissionScheduler::validateJobRequest", VK_ERROR_INITIALIZATION_FAILED, "submission_scheduler", "null_wait_semaphore");
        }
    }
    for (const VkSemaphore sem : request.signalSemaphores) {
        if (sem == VK_NULL_HANDLE) {
            return vkutil::makeError("SubmissionScheduler::validateJobRequest", VK_ERROR_INITIALIZATION_FAILED, "submission_scheduler", "null_signal_semaphore");
        }
    }

    return {};
}

vkutil::VkExpected<void> SubmissionScheduler::validatePresentRequest(const PresentRequest& request) const
{
    if (deviceContext_ == nullptr || !deviceContext_->valid()) {
        return vkutil::makeError("SubmissionScheduler::validatePresentRequest", VK_ERROR_INITIALIZATION_FAILED, "submission_scheduler", "invalid_device_context");
    }
    if (request.swapchain == VK_NULL_HANDLE) {
        return vkutil::makeError("SubmissionScheduler::validatePresentRequest", VK_ERROR_INITIALIZATION_FAILED, "submission_scheduler", "null_swapchain");
    }
    for (const VkSemaphore sem : request.waitSemaphores) {
        if (sem == VK_NULL_HANDLE) {
            return vkutil::makeError("SubmissionScheduler::validatePresentRequest", VK_ERROR_INITIALIZATION_FAILED, "submission_scheduler", "null_present_wait_semaphore");
        }
    }

    return {};
}

vkutil::VkExpected<void> SubmissionScheduler::reclaimAutoSemaphores()
{
    if (pendingAutoSemaphores_.empty()) {
        return {};
    }
    if (deviceContext_ == nullptr || !deviceContext_->valid()) {
        return vkutil::makeError("SubmissionScheduler::reclaimAutoSemaphores", VK_ERROR_INITIALIZATION_FAILED, "submission_scheduler", "invalid_device_context");
    }

    const VkDevice device = deviceContext_->vkDevice();
    auto it = pendingAutoSemaphores_.begin();
    while (it != pendingAutoSemaphores_.end()) {
        if (it->retireFence == VK_NULL_HANDLE) {
            ++it;
            continue;
        }

        const VkResult fenceState = vkGetFenceStatus(device, it->retireFence);
        if (fenceState == VK_SUCCESS) {
            it = pendingAutoSemaphores_.erase(it);
            continue;
        }
        if (fenceState != VK_NOT_READY) {
            return vkutil::checkResult(fenceState, "vkGetFenceStatus", "submission_scheduler");
        }

        ++it;
    }

    return {};
}

vkutil::VkExpected<DeviceContext::QueueSubmissionToken> SubmissionScheduler::queueTokenFor(QueueClass queueClass, bool* outUsedComputeFallback) const
{
    if (outUsedComputeFallback != nullptr) {
        *outUsedComputeFallback = false;
    }

    if (deviceContext_ == nullptr) {
        return vkutil::VkExpected<DeviceContext::QueueSubmissionToken>(
            vkutil::makeError("SubmissionScheduler::queueTokenFor", VK_ERROR_INITIALIZATION_FAILED, "submission_scheduler", "invalid_device_context").context());
    }

    const DeviceQueueCapabilityProfile queueProfile = deviceContext_->queueCapabilityProfile();

    switch (queueClass) {
    case QueueClass::Graphics:
        return vkutil::VkExpected<DeviceContext::QueueSubmissionToken>(deviceContext_->graphicsQueueToken());
    case QueueClass::Transfer:
        return vkutil::VkExpected<DeviceContext::QueueSubmissionToken>(deviceContext_->transferQueueToken());
    case QueueClass::Compute:
    {
        if (policy_.requireDedicatedComputeQueue && !queueProfile.computeQueueDedicated) {
            return vkutil::VkExpected<DeviceContext::QueueSubmissionToken>(
                vkutil::makeError("SubmissionScheduler::queueTokenFor", VK_ERROR_FEATURE_NOT_PRESENT, "submission_scheduler", "compute_queue_not_dedicated").context());
        }

        DeviceContext::QueueSubmissionToken computeToken = deviceContext_->computeQueueToken();
        if (computeToken.valid()) {
            return vkutil::VkExpected<DeviceContext::QueueSubmissionToken>(computeToken);
        }

        if (!policy_.allowComputeOnGraphicsFallback) {
            return vkutil::VkExpected<DeviceContext::QueueSubmissionToken>(
                vkutil::makeError("SubmissionScheduler::queueTokenFor", VK_ERROR_FEATURE_NOT_PRESENT, "submission_scheduler", "compute_queue_unavailable").context());
        }

        DeviceContext::QueueSubmissionToken graphicsToken = deviceContext_->graphicsQueueToken();
        if (!graphicsToken.valid()) {
            return vkutil::VkExpected<DeviceContext::QueueSubmissionToken>(
                vkutil::makeError("SubmissionScheduler::queueTokenFor", VK_ERROR_INITIALIZATION_FAILED, "submission_scheduler", "graphics_queue_unavailable_for_compute_fallback").context());
        }

        if (outUsedComputeFallback != nullptr) {
            *outUsedComputeFallback = true;
        }
        return vkutil::VkExpected<DeviceContext::QueueSubmissionToken>(graphicsToken);
    }
    default:
        return vkutil::VkExpected<DeviceContext::QueueSubmissionToken>(
            vkutil::makeError("SubmissionScheduler::queueTokenFor", VK_ERROR_INITIALIZATION_FAILED, "submission_scheduler", "unknown_queue_class").context());
    }
}

vkutil::VkExpected<SubmissionScheduler::JobId> SubmissionScheduler::enqueueJob(const JobRequest& request)
{
    const auto validation = validateJobRequest(request);
    if (!validation.hasValue()) {
        return vkutil::VkExpected<JobId>(validation.context());
    }

    const JobId id = jobs_.size();
    jobs_.push_back(EnqueuedJob{ .id = id, .request = request });
    return vkutil::VkExpected<JobId>(id);
}

vkutil::VkExpected<void> SubmissionScheduler::enqueueDependency(
    JobId producer,
    JobId consumer,
    VkSemaphore dependencySemaphore,
    VkPipelineStageFlags2 consumerWaitStage)
{
    if (producer >= jobs_.size() || consumer >= jobs_.size()) {
        return vkutil::makeError("SubmissionScheduler::enqueueDependency", VK_ERROR_INITIALIZATION_FAILED, "submission_scheduler", "invalid_job_id");
    }
    if (producer == consumer) {
        return vkutil::makeError("SubmissionScheduler::enqueueDependency", VK_ERROR_INITIALIZATION_FAILED, "submission_scheduler", "self_dependency");
    }
    if (dependencySemaphore == VK_NULL_HANDLE
        && jobs_[producer].request.queueClass == jobs_[consumer].request.queueClass)
    {
        // Same-queue edges are strictly ordered by topological submit order.
    }

    dependencies_.push_back(DependencyEdge{
        .producer = producer,
        .consumer = consumer,
        .semaphore = dependencySemaphore,
        .consumerWaitStage = consumerWaitStage
        });

    return {};
}

vkutil::VkExpected<std::vector<SubmissionScheduler::JobId>> SubmissionScheduler::topologicalOrder() const
{
    const size_t jobCount = jobs_.size();
    std::vector<uint32_t> indegree(jobCount, 0);
    std::vector<std::vector<JobId>> adjacency(jobCount);

    for (const DependencyEdge& edge : dependencies_) {
        if (edge.producer >= jobCount || edge.consumer >= jobCount) {
            return vkutil::VkExpected<std::vector<JobId>>(
                vkutil::makeError("SubmissionScheduler::topologicalOrder", VK_ERROR_INITIALIZATION_FAILED, "submission_scheduler", "invalid_job_id_dependency").context());
        }
        adjacency[edge.producer].push_back(edge.consumer);
        indegree[edge.consumer] += 1;
    }

    std::vector<JobId> ready{};
    ready.reserve(jobCount);
    for (JobId id = 0; id < jobCount; ++id) {
        if (indegree[id] == 0) {
            ready.push_back(id);
        }
    }

    std::vector<JobId> ordered{};
    ordered.reserve(jobCount);
    while (!ready.empty()) {
        const JobId current = ready.back();
        ready.pop_back();
        ordered.push_back(current);

        for (const JobId child : adjacency[current]) {
            uint32_t& childIndegree = indegree[child];
            childIndegree -= 1;
            if (childIndegree == 0) {
                ready.push_back(child);
            }
        }
    }

    if (ordered.size() != jobCount) {
        return vkutil::VkExpected<std::vector<JobId>>(
            vkutil::makeError("SubmissionScheduler::topologicalOrder", VK_ERROR_INITIALIZATION_FAILED, "submission_scheduler", "dependency_cycle_detected").context());
    }

    return ordered;
}

vkutil::VkExpected<std::vector<SubmissionScheduler::PreparedJob>> SubmissionScheduler::buildPreparedJobs(
    const std::vector<JobId>& topoOrder,
    std::vector<VulkanSemaphore>& frameAutoSemaphores,
    SubmissionScheduler::DependencyRuntimeMode runtimeMode)
{
    std::vector<PreparedJob> prepared{};
    prepared.reserve(topoOrder.size());

    for (const JobId id : topoOrder) {
        const EnqueuedJob& source = jobs_[id];
        prepared.push_back(PreparedJob{
            .id = id,
            .queueClass = source.request.queueClass,
            .commandBuffers = source.request.commandBuffers,
            .waitSemaphores = source.request.waitSemaphores,
            .waitStages = source.request.waitStages,
            .signalSemaphores = source.request.signalSemaphores,
            .fence = source.request.fence,
            .debugLabel = source.request.debugLabel
            });
    }

    std::vector<size_t> indexByJobId(jobs_.size(), static_cast<size_t>(-1));
    for (size_t orderIndex = 0; orderIndex < prepared.size(); ++orderIndex) {
        indexByJobId[prepared[orderIndex].id] = orderIndex;
    }

    for (const DependencyEdge& edge : dependencies_) {
        const size_t producerIndex = indexByJobId[edge.producer];
        const size_t consumerIndex = indexByJobId[edge.consumer];
        if (producerIndex == static_cast<size_t>(-1) || consumerIndex == static_cast<size_t>(-1)) {
            return vkutil::VkExpected<std::vector<PreparedJob>>(
                vkutil::makeError("SubmissionScheduler::buildPreparedJobs", VK_ERROR_INITIALIZATION_FAILED, "submission_scheduler", "dependency_topology_mismatch").context());
        }
        if (producerIndex >= consumerIndex) {
            return vkutil::VkExpected<std::vector<PreparedJob>>(
                vkutil::makeError("SubmissionScheduler::buildPreparedJobs", VK_ERROR_INITIALIZATION_FAILED, "submission_scheduler", "dependency_not_topological").context());
        }

        PreparedJob& producer = prepared[producerIndex];
        PreparedJob& consumer = prepared[consumerIndex];

        VkSemaphore dependencySemaphore = edge.semaphore;
        if (dependencySemaphore == VK_NULL_HANDLE) {
            if (producer.queueClass == consumer.queueClass) {
                // same-queue ordering handled by topological submission order.
                continue;
            }

            if (runtimeMode != DependencyRuntimeMode::BinaryFallback) {
                continue;
            }

            auto autoSemaphoreResult = VulkanSemaphore::createResult(deviceContext_->vkDevice());
            if (!autoSemaphoreResult.hasValue()) {
                return vkutil::VkExpected<std::vector<PreparedJob>>(autoSemaphoreResult.context());
            }

            frameAutoSemaphores.push_back(std::move(autoSemaphoreResult.value()));
            dependencySemaphore = frameAutoSemaphores.back().get();
        }

        producer.signalSemaphores.push_back(dependencySemaphore);
        consumer.waitSemaphores.push_back(dependencySemaphore);
        consumer.waitStages.push_back(edge.consumerWaitStage);
    }

    for (const PreparedJob& job : prepared) {
        if (job.waitSemaphores.size() != job.waitStages.size()) {
            return vkutil::VkExpected<std::vector<PreparedJob>>(
                vkutil::makeError("SubmissionScheduler::buildPreparedJobs", VK_ERROR_INITIALIZATION_FAILED, "submission_scheduler", "wait_stage_mismatch").context());
        }
    }

    return prepared;
}

std::vector<SubmissionScheduler::SubmitBatch> SubmissionScheduler::buildBatches(const std::vector<PreparedJob>& preparedJobs) const
{
    std::vector<SubmitBatch> batches{};

    for (const PreparedJob& job : preparedJobs) {
        SubmitBatch::SubmitEntry entry{};
        entry.waitStagesLegacy.reserve(job.waitStages.size());
        for (const VkPipelineStageFlags2 stage2 : job.waitStages) {
            entry.waitStagesLegacy.push_back(legacyStageMaskForFallback(job.queueClass, stage2));
        }

        entry.submitInfo = VkSubmitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        entry.submitInfo.waitSemaphoreCount = static_cast<uint32_t>(job.waitSemaphores.size());
        entry.submitInfo.pWaitSemaphores = job.waitSemaphores.empty() ? nullptr : job.waitSemaphores.data();
        entry.submitInfo.pWaitDstStageMask = entry.waitStagesLegacy.empty() ? nullptr : entry.waitStagesLegacy.data();
        entry.submitInfo.commandBufferCount = static_cast<uint32_t>(job.commandBuffers.size());
        entry.submitInfo.pCommandBuffers = job.commandBuffers.data();
        entry.submitInfo.signalSemaphoreCount = static_cast<uint32_t>(job.signalSemaphores.size());
        entry.submitInfo.pSignalSemaphores = job.signalSemaphores.empty() ? nullptr : job.signalSemaphores.data();

        const bool canAppendToPrevious = !batches.empty()
            && batches.back().queueClass == job.queueClass
            && batches.back().fence == VK_NULL_HANDLE
            && job.fence == VK_NULL_HANDLE;

        if (canAppendToPrevious) {
            batches.back().entries.push_back(std::move(entry));
            continue;
        }

        SubmitBatch batch{};
        batch.queueClass = job.queueClass;
        batch.fence = job.fence;
        batch.debugLabel = (job.debugLabel != nullptr && job.debugLabel[0] != '\0')
            ? job.debugLabel
            : "submission_scheduler_batch";
        batch.entries.push_back(std::move(entry));
        batches.push_back(std::move(batch));
    }

    for (SubmitBatch& batch : batches) {
        batch.submitInfos.clear();
        batch.submitInfos.reserve(batch.entries.size());
        for (SubmitBatch::SubmitEntry& entry : batch.entries) {
            batch.submitInfos.push_back(entry.submitInfo);
        }
    }

    return batches;
}

vkutil::VkExpected<std::vector<SubmissionScheduler::SubmitBatch2>> SubmissionScheduler::buildBatches2(const std::vector<PreparedJob>& preparedJobs) const
{
    std::vector<SubmitBatch2> batches{};

    for (const PreparedJob& job : preparedJobs) {
        SubmitBatch2::SubmitEntry entry{};

        entry.waitInfos.reserve(job.waitSemaphores.size());
        for (size_t i = 0; i < job.waitSemaphores.size(); ++i) {
            const VkSemaphore semaphore = job.waitSemaphores[i];
            const VkPipelineStageFlags2 waitStage = job.waitStages[i];

            VkSemaphoreSubmitInfo waitInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
            waitInfo.semaphore = semaphore;
            waitInfo.value = 0;
            waitInfo.stageMask = waitStage != 0 ? waitStage : defaultWaitStageMask2(job.queueClass);
            waitInfo.deviceIndex = 0;
            entry.waitInfos.push_back(waitInfo);
        }

        entry.signalInfos.reserve(job.signalSemaphores.size());
        for (const VkSemaphore semaphore : job.signalSemaphores) {
            VkSemaphoreSubmitInfo signalInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
            signalInfo.semaphore = semaphore;
            signalInfo.value = 0;
            signalInfo.stageMask = signalStageMask2(job.queueClass);
            signalInfo.deviceIndex = 0;
            entry.signalInfos.push_back(signalInfo);
        }

        entry.commandBufferInfos.reserve(job.commandBuffers.size());
        for (const VkCommandBuffer cmd : job.commandBuffers) {
            VkCommandBufferSubmitInfo cmdInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
            cmdInfo.commandBuffer = cmd;
            cmdInfo.deviceMask = 0;
            entry.commandBufferInfos.push_back(cmdInfo);
        }

        entry.submitInfo = VkSubmitInfo2{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
        entry.submitInfo.flags = 0;
        entry.submitInfo.waitSemaphoreInfoCount = static_cast<uint32_t>(entry.waitInfos.size());
        entry.submitInfo.pWaitSemaphoreInfos = entry.waitInfos.empty() ? nullptr : entry.waitInfos.data();
        entry.submitInfo.commandBufferInfoCount = static_cast<uint32_t>(entry.commandBufferInfos.size());
        entry.submitInfo.pCommandBufferInfos = entry.commandBufferInfos.empty() ? nullptr : entry.commandBufferInfos.data();
        entry.submitInfo.signalSemaphoreInfoCount = static_cast<uint32_t>(entry.signalInfos.size());
        entry.submitInfo.pSignalSemaphoreInfos = entry.signalInfos.empty() ? nullptr : entry.signalInfos.data();

        const bool canAppendToPrevious = !batches.empty()
            && batches.back().queueClass == job.queueClass
            && batches.back().fence == VK_NULL_HANDLE
            && job.fence == VK_NULL_HANDLE;

        if (!canAppendToPrevious) {
            SubmitBatch2 batch{};
            batch.queueClass = job.queueClass;
            batch.fence = job.fence;
            batch.debugLabel = (job.debugLabel != nullptr && job.debugLabel[0] != '\0')
                ? job.debugLabel
                : "submission_scheduler_batch2";
            batches.push_back(std::move(batch));
        }

        batches.back().entries.push_back(std::move(entry));
    }

    for (SubmitBatch2& batch : batches) {
        batch.submitInfos.clear();
        batch.submitInfos.reserve(batch.entries.size());
        for (SubmitBatch2::SubmitEntry& entry : batch.entries) {
            batch.submitInfos.push_back(entry.submitInfo);
        }
    }

    return batches;
}

vkutil::VkExpected<VulkanQueue> SubmissionScheduler::queueForSyncContext(QueueClass queueClass, bool* outUsedComputeFallback) const
{
    if (outUsedComputeFallback != nullptr) {
        *outUsedComputeFallback = false;
    }

    if (deviceContext_ == nullptr) {
        return vkutil::VkExpected<VulkanQueue>(
            vkutil::makeError("SubmissionScheduler::queueForSyncContext", VK_ERROR_INITIALIZATION_FAILED, "submission_scheduler", "invalid_device_context").context());
    }

    const DeviceQueueCapabilityProfile queueProfile = deviceContext_->queueCapabilityProfile();

    switch (queueClass) {
    case QueueClass::Graphics:
        return vkutil::VkExpected<VulkanQueue>(deviceContext_->graphicsQueue());
    case QueueClass::Transfer:
        return vkutil::VkExpected<VulkanQueue>(deviceContext_->transferQueue());
    case QueueClass::Compute:
    {
        if (policy_.requireDedicatedComputeQueue && !queueProfile.computeQueueDedicated) {
            return vkutil::VkExpected<VulkanQueue>(
                vkutil::makeError("SubmissionScheduler::queueForSyncContext", VK_ERROR_FEATURE_NOT_PRESENT, "submission_scheduler", "compute_queue_not_dedicated").context());
        }

        VulkanQueue compute = deviceContext_->computeQueue();
        if (compute.valid()) {
            return vkutil::VkExpected<VulkanQueue>(compute);
        }

        if (!policy_.allowComputeOnGraphicsFallback) {
            return vkutil::VkExpected<VulkanQueue>(
                vkutil::makeError("SubmissionScheduler::queueForSyncContext", VK_ERROR_FEATURE_NOT_PRESENT, "submission_scheduler", "compute_queue_unavailable").context());
        }

        VulkanQueue graphics = deviceContext_->graphicsQueue();
        if (!graphics.valid()) {
            return vkutil::VkExpected<VulkanQueue>(
                vkutil::makeError("SubmissionScheduler::queueForSyncContext", VK_ERROR_INITIALIZATION_FAILED, "submission_scheduler", "graphics_queue_unavailable_for_compute_fallback").context());
        }

        if (outUsedComputeFallback != nullptr) {
            *outUsedComputeFallback = true;
        }
        return vkutil::VkExpected<VulkanQueue>(graphics);
    }
    default:
        return vkutil::VkExpected<VulkanQueue>(
            vkutil::makeError("SubmissionScheduler::queueForSyncContext", VK_ERROR_INITIALIZATION_FAILED, "submission_scheduler", "unknown_queue_class").context());
    }
}

vkutil::VkExpected<SubmissionScheduler::FrameExecutionResult> SubmissionScheduler::executeFrameWithTimeline(const std::vector<PreparedJob>& preparedJobs)
{
    if (deviceContext_ == nullptr || !deviceContext_->valid() || deviceContext_->syncContext == nullptr) {
        return vkutil::VkExpected<FrameExecutionResult>(
            vkutil::makeError("SubmissionScheduler::executeFrameWithTimeline", VK_ERROR_INITIALIZATION_FAILED, "submission_scheduler", "invalid_sync_context").context());
    }

    SyncContext& syncContext = *deviceContext_->syncContext;
    const DeviceQueueCapabilityProfile queueProfile = deviceContext_->queueCapabilityProfile();
    bool usedComputeFallbackAny = false;
    std::vector<std::optional<SyncTicket>> ticketByJob(jobs_.size());

    const uint32_t syncFrameIndex = (syncContext.framesInFlight() == 0)
        ? 0u
        : static_cast<uint32_t>(frameOrdinal_ % syncContext.framesInFlight());

    for (const PreparedJob& job : preparedJobs) {
        bool usedComputeFallback = false;
        const auto queueResult = queueForSyncContext(job.queueClass, &usedComputeFallback);
        usedComputeFallbackAny = usedComputeFallbackAny || usedComputeFallback;
        if (!queueResult.hasValue()) {
            return vkutil::VkExpected<FrameExecutionResult>(queueResult.context());
        }
        VulkanQueue queue = queueResult.value();

        SyncSubmitInfo submitInfo{};
        submitInfo.commandBuffers = job.commandBuffers;
        submitInfo.externalWaitSemaphores = job.waitSemaphores;
        submitInfo.externalSignalSemaphores = job.signalSemaphores;
        submitInfo.debugLabel = job.debugLabel;

        submitInfo.externalWaitStages.reserve(job.waitStages.size());
        for (const VkPipelineStageFlags2 stage : job.waitStages) {
            submitInfo.externalWaitStages.push_back(stage != 0 ? stage : defaultWaitStageMask2(job.queueClass));
        }

        for (const DependencyEdge& edge : dependencies_) {
            if (edge.consumer != job.id || edge.semaphore != VK_NULL_HANDLE) {
                continue;
            }
            if (jobs_[edge.producer].request.queueClass == jobs_[edge.consumer].request.queueClass) {
                continue;
            }
            if (!ticketByJob[edge.producer].has_value()) {
                return vkutil::VkExpected<FrameExecutionResult>(
                    vkutil::makeError("SubmissionScheduler::executeFrameWithTimeline", VK_ERROR_INITIALIZATION_FAILED, "submission_scheduler", "missing_producer_ticket").context());
            }
            submitInfo.waitTickets.push_back(ticketByJob[edge.producer].value());
        }

        const auto ticketResult = syncContext.submit(queue, syncFrameIndex, submitInfo, job.fence);
        if (!ticketResult.hasValue()) {
            return vkutil::VkExpected<FrameExecutionResult>(ticketResult.context());
        }

        if (job.id < ticketByJob.size()) {
            ticketByJob[job.id] = ticketResult.value();
        }
    }

    VkResult presentResult = VK_SUCCESS;
    if (hasPresentRequest_) {
        DeviceContext::QueueSubmissionToken presentToken = deviceContext_->presentQueueToken();
        if (!presentToken.valid()) {
            return vkutil::VkExpected<FrameExecutionResult>(
                vkutil::makeError("SubmissionScheduler::executeFrame", VK_ERROR_INITIALIZATION_FAILED, "submission_scheduler", "invalid_present_token").context());
        }

        presentResult = presentToken.present(
            presentRequest_.swapchain,
            presentRequest_.imageIndex,
            presentRequest_.waitSemaphores);
    }

    return vkutil::VkExpected<FrameExecutionResult>(FrameExecutionResult{
        .presentResult = presentResult,
        .submittedJobCount = static_cast<uint32_t>(preparedJobs.size()),
        .submitBatchCount = static_cast<uint32_t>(preparedJobs.size()),
                .autoSemaphoreCount = 0,
        .usedTimelineSubmission = true,
        .usedComputeToGraphicsFallback = usedComputeFallbackAny,
        .computeQueueAvailable = queueProfile.hasComputeQueue,
        .computeQueueDedicated = queueProfile.computeQueueDedicated
        });
}

vkutil::VkExpected<void> SubmissionScheduler::enqueuePresent(const PresentRequest& request)
{
    const auto validation = validatePresentRequest(request);
    if (!validation.hasValue()) {
        return validation;
    }

    presentRequest_ = request;
    hasPresentRequest_ = true;
    return {};
}

vkutil::VkExpected<SubmissionScheduler::FrameExecutionResult> SubmissionScheduler::executeFrame()
{
    if (deviceContext_ == nullptr || !deviceContext_->valid()) {
        return vkutil::VkExpected<FrameExecutionResult>(
            vkutil::makeError("SubmissionScheduler::executeFrame", VK_ERROR_INITIALIZATION_FAILED, "submission_scheduler", "invalid_device_context").context());
    }

    const auto topoOrderResult = topologicalOrder();
    if (!topoOrderResult.hasValue()) {
        return vkutil::VkExpected<FrameExecutionResult>(topoOrderResult.context());
    }

    const DeviceQueueCapabilityProfile queueProfile = deviceContext_->queueCapabilityProfile();

    if (policy_.requireDedicatedComputeQueue && !queueProfile.computeQueueDedicated) {
        return vkutil::VkExpected<FrameExecutionResult>(
            vkutil::makeError("SubmissionScheduler::executeFrame", VK_ERROR_FEATURE_NOT_PRESENT, "submission_scheduler", "compute_queue_not_dedicated").context());
    }

    const bool timelinePrimary = deviceContext_->syncContext != nullptr
        && deviceContext_->syncContext->timelineMode();

    std::vector<VulkanSemaphore> frameAutoSemaphores{};
    const auto preparedJobsResult = buildPreparedJobs(
        topoOrderResult.value(),
        frameAutoSemaphores,
        timelinePrimary ? DependencyRuntimeMode::TimelinePrimary : DependencyRuntimeMode::BinaryFallback);
    if (!preparedJobsResult.hasValue()) {
        return vkutil::VkExpected<FrameExecutionResult>(preparedJobsResult.context());
    }

    if (timelinePrimary) {
        return executeFrameWithTimeline(preparedJobsResult.value());
    }

    VkFence frameRetireFence = VK_NULL_HANDLE;
    uint32_t submitBatchCount = 0;
    bool usedComputeFallbackAny = false;

    const bool useSubmit2 = deviceContext_->isFeatureEnabledSynchronization2();
    if (useSubmit2) {
        const auto batches2Result = buildBatches2(preparedJobsResult.value());
        if (!batches2Result.hasValue()) {
            return vkutil::VkExpected<FrameExecutionResult>(batches2Result.context());
        }

        const std::vector<SubmitBatch2>& batches2 = batches2Result.value();
        submitBatchCount = static_cast<uint32_t>(batches2.size());
        for (const SubmitBatch2& batch : batches2) {
            bool usedComputeFallback = false;
            const auto tokenResult = queueTokenFor(batch.queueClass, &usedComputeFallback);
            if (!tokenResult.hasValue()) {
                return vkutil::VkExpected<FrameExecutionResult>(tokenResult.context());
            }
            DeviceContext::QueueSubmissionToken token = tokenResult.value();
            usedComputeFallbackAny = usedComputeFallbackAny || usedComputeFallback;

            const auto submitResult = token.submit2(batch.submitInfos, batch.fence, batch.debugLabel);
            if (!submitResult.hasValue()) {
                return vkutil::VkExpected<FrameExecutionResult>(submitResult.context());
            }

            if (batch.fence != VK_NULL_HANDLE) {
                frameRetireFence = batch.fence;
            }
        }
    }
    else {
        std::vector<SubmitBatch> batches = buildBatches(preparedJobsResult.value());
        submitBatchCount = static_cast<uint32_t>(batches.size());
        for (const SubmitBatch& batch : batches) {
            bool usedComputeFallback = false;
            const auto tokenResult = queueTokenFor(batch.queueClass, &usedComputeFallback);
            if (!tokenResult.hasValue()) {
                return vkutil::VkExpected<FrameExecutionResult>(tokenResult.context());
            }
            DeviceContext::QueueSubmissionToken token = tokenResult.value();
            usedComputeFallbackAny = usedComputeFallbackAny || usedComputeFallback;

            const auto submitResult = token.submit(batch.submitInfos, batch.fence, batch.debugLabel);
            if (!submitResult.hasValue()) {
                return vkutil::VkExpected<FrameExecutionResult>(submitResult.context());
            }

            if (batch.fence != VK_NULL_HANDLE) {
                frameRetireFence = batch.fence;
            }
        }
    }

    VkResult presentResult = VK_SUCCESS;
    if (hasPresentRequest_) {
        DeviceContext::QueueSubmissionToken presentToken = deviceContext_->presentQueueToken();
        if (!presentToken.valid()) {
            return vkutil::VkExpected<FrameExecutionResult>(
                vkutil::makeError("SubmissionScheduler::executeFrame", VK_ERROR_INITIALIZATION_FAILED, "submission_scheduler", "invalid_present_token").context());
        }

        presentResult = presentToken.present(
            presentRequest_.swapchain,
            presentRequest_.imageIndex,
            presentRequest_.waitSemaphores);
    }

    const uint32_t autoSemaphoreCount = static_cast<uint32_t>(frameAutoSemaphores.size());
    for (VulkanSemaphore& sem : frameAutoSemaphores) {
        pendingAutoSemaphores_.push_back(PendingAutoSemaphore{
            .semaphore = std::move(sem),
            .retireFence = frameRetireFence
            });
    }

    return vkutil::VkExpected<FrameExecutionResult>(FrameExecutionResult{
        .presentResult = presentResult,
        .submittedJobCount = static_cast<uint32_t>(preparedJobsResult.value().size()),
        .submitBatchCount = submitBatchCount,
        .autoSemaphoreCount = autoSemaphoreCount,
        .usedTimelineSubmission = false,
        .usedComputeToGraphicsFallback = usedComputeFallbackAny,
        .computeQueueAvailable = queueProfile.hasComputeQueue,
        .computeQueueDedicated = queueProfile.computeQueueDedicated
        });
}
