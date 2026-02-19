#include <iterator>
#include <cstddef>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <array>

 #include "DeletionQueue.h"

namespace {
constexpr std::size_t kLocalFlushThreshold = 32;

struct ThreadIngressRegistry {
    std::mutex mutex{};
    std::unordered_map<const DeletionQueue*, std::vector<std::pair<uint64_t, DeletionQueue::DeleteTask>>> buffers{};
};

thread_local ThreadIngressRegistry g_threadIngress{};
}

DeletionQueue::DeletionQueue(std::size_t reserveCount, DrainOrder order)
    : drainOrder(order)
{
    std::lock_guard<std::mutex> ingressLock(ingressMutex_);
    ingressItems_.reserve(reserveCount);
}

void DeletionQueue::reserve(std::size_t count)
{
    std::lock_guard<std::mutex> ingressLock(ingressMutex_);
    ingressItems_.reserve(count);
}

bool DeletionQueue::empty() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return totalItems_ == 0;
}

std::size_t DeletionQueue::size() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return totalItems_;
}

void DeletionQueue::clear()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        readyByFence_.clear();
        totalItems_ = 0;
    }
    std::lock_guard<std::mutex> ingressLock(ingressMutex_);
    ingressItems_.clear();
}

DeletionQueue::CollectStats DeletionQueue::lastCollectStats() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return lastCollectStats_;
}

void DeletionQueue::setDrainOrder(DrainOrder order) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    drainOrder = order;
}

DeletionQueue::DrainOrder DeletionQueue::getDrainOrder() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return drainOrder;
}

void DeletionQueue::setFailurePolicy(FailurePolicy policy) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    failurePolicy = policy;
}

DeletionQueue::FailurePolicy DeletionQueue::getFailurePolicy() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return failurePolicy;
}

void DeletionQueue::setRetryPolicy(const RetryPolicy& policy) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    retryPolicy = policy;
}

DeletionQueue::RetryPolicy DeletionQueue::getRetryPolicy() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return retryPolicy;
}

void DeletionQueue::setFailureEscalationHook(FailureEscalationHook hook)
{
    std::lock_guard<std::mutex> lock(mutex_);
    escalationHook_ = std::move(hook);
}

void DeletionQueue::enqueue(uint64_t fenceValue, DeleteTask&& fn)
{
    if (!fn) return;

    std::vector<std::pair<uint64_t, DeleteTask>>* localBuffer = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_threadIngress.mutex);
        localBuffer = &g_threadIngress.buffers[this];
    }

    localBuffer->emplace_back(fenceValue, std::move(fn));
    if (localBuffer->size() < kLocalFlushThreshold) {
        return;
    }

    std::vector<std::pair<uint64_t, DeleteTask>> flushBatch{};
    flushBatch.swap(*localBuffer);

    std::lock_guard<std::mutex> ingressLock(ingressMutex_);
    for (auto& [fence, task] : flushBatch) {
        ingressItems_.emplace_back(fence, std::move(task));
    }
}

void DeletionQueue::enqueue(uint64_t fenceValue, std::move_only_function<void()>&& fn)
{
    if (!fn) return;

    enqueue(fenceValue, DeleteTask{
        [legacy = std::move(fn)]() mutable -> vkutil::VkExpected<void>
        {
            legacy();
            return {};
        }});
}

vkutil::VkExpected<void> DeletionQueue::executeTask(DeleteTask& fn, uint64_t frameIndex) noexcept
{
    if (!fn) return {};

#if VK_WRAPPER_USE_EXCEPTIONS
    try {
        return fn();
    }
    catch (...) {
        return vkutil::checkResult(VK_ERROR_UNKNOWN, "DeletionQueue::task", "deletion_queue", nullptr, frameIndex);
    }
#else
    (void)frameIndex;
    return fn();
#endif
}

bool DeletionQueue::shouldAttemptNow(const Item& item, uint64_t frameIndex) const noexcept
{
    return frameIndex >= item.nextRetryFrame;
}

bool DeletionQueue::shouldRetainFailed(Item& item, uint64_t frameIndex, const RetryPolicy& policy, FailureEscalationEvent& escalation) noexcept
{
    if (item.firstFailureFrame == 0) {
        item.firstFailureFrame = frameIndex;
    }

    item.retryCount += 1;

    const uint64_t age = frameIndex - item.firstFailureFrame;
    const bool retryExhausted = item.retryCount > policy.maxRetries;
    const bool ageExceeded = age > policy.maxFrameAge;
    if (retryExhausted || ageExceeded) {
        escalation = FailureEscalationEvent{
            .fenceValue = item.fenceValue,
            .retryCount = item.retryCount,
            .firstFailureFrame = item.firstFailureFrame,
            .currentFrame = frameIndex,
            .reason = retryExhausted ? "max_retries_exceeded" : "max_age_exceeded"
        };
        return false;
    }

    const uint64_t backoffShift = (item.retryCount > 16) ? 16 : item.retryCount;
    const uint64_t backoffFrames = policy.baseRetryBackoffFrames == 0 ? 0 : (policy.baseRetryBackoffFrames << backoffShift);
    item.nextRetryFrame = frameIndex + backoffFrames;
    return true;
}

void DeletionQueue::reportEscalation(const FailureEscalationEvent& event) const
{
    FailureEscalationHook hook;
    bool hardFailInDebug = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        hook = escalationHook_;
        hardFailInDebug = retryPolicy.hardFailInDebug;
    }

    if (hook) {
        hook(event);
    }
#ifndef NDEBUG
    if (hardFailInDebug) {
        std::fputs("DeletionQueue escalation: dropping repeatedly failed task\n", stderr);
        std::abort();
    }
#endif
}

void DeletionQueue::drainIngressLocked()
{
    std::vector<Item> incoming{};
    {
        std::lock_guard<std::mutex> ingressLock(ingressMutex_);
        incoming.swap(ingressItems_);
    }

    for (auto& item : incoming) {
        auto& queue = readyByFence_[item.fenceValue];
        queue.emplace_back(std::move(item));
        ++totalItems_;
    }
}

vkutil::VkExpected<void> DeletionQueue::collect(uint64_t completedValue, uint64_t frameIndex) noexcept
{
    // Flush this thread's local ingress.
    {
        std::vector<std::pair<uint64_t, DeleteTask>> local{};
        {
            std::lock_guard<std::mutex> lock(g_threadIngress.mutex);
            auto it = g_threadIngress.buffers.find(this);
            if (it != g_threadIngress.buffers.end()) {
                local.swap(it->second);
            }
        }
        if (!local.empty()) {
            std::lock_guard<std::mutex> ingressLock(ingressMutex_);
            for (auto& [fence, task] : local) {
                ingressItems_.emplace_back(fence, std::move(task));
            }
        }
    }

    CollectStats stats{};
    vkutil::VkExpected<void> firstFailure{};
    bool hasFailure = false;
    std::vector<FailureEscalationEvent> escalations{};
    std::vector<Item> executeItems{};
    std::vector<Item> deferredItems{};
    FailurePolicy policy = FailurePolicy::KeepFailedTasks;
    RetryPolicy retry{};

    {
        std::lock_guard<std::mutex> lock(mutex_);
        lastCollectStats_ = {};

        drainIngressLocked();
        policy = failurePolicy;
        retry = retryPolicy;

        auto it = readyByFence_.begin();
        while (it != readyByFence_.end() && it->first <= completedValue) {
            auto queue = std::move(it->second);
            it = readyByFence_.erase(it);

            if (drainOrder == DrainOrder::LIFO) {
                for (auto rit = queue.rbegin(); rit != queue.rend(); ++rit) {
                    if (shouldAttemptNow(*rit, frameIndex)) {
                        executeItems.emplace_back(std::move(*rit));
                    } else {
                        deferredItems.emplace_back(std::move(*rit));
                    }
                }
            } else {
                for (auto& item : queue) {
                    if (shouldAttemptNow(item, frameIndex)) {
                        executeItems.emplace_back(std::move(item));
                    } else {
                        deferredItems.emplace_back(std::move(item));
                    }
                }
            }
        }
    }

    std::vector<Item> retryItems{};
    retryItems.reserve(deferredItems.size());
    for (auto& deferred : deferredItems) {
        retryItems.emplace_back(std::move(deferred));
    }

    for (auto& item : executeItems) {
        ++stats.executedCount;
        const auto status = executeTask(item.fn, frameIndex);
        if (status.hasValue()) {
            ++stats.successCount;
            continue;
        }

        ++stats.failureCount;
        if (!hasFailure) {
            firstFailure = status;
            hasFailure = true;
        }

        if (policy == FailurePolicy::KeepFailedTasks) {
            FailureEscalationEvent escalation{};
            if (shouldRetainFailed(item, frameIndex, retry, escalation)) {
                ++stats.retainedFailedCount;
                retryItems.emplace_back(std::move(item));
            } else {
                ++stats.droppedFailedCount;
                escalations.push_back(escalation);
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& item : retryItems) {
            readyByFence_[item.fenceValue].emplace_back(std::move(item));
        }

        const std::size_t succeededCount = static_cast<std::size_t>(stats.successCount);
        const std::size_t droppedCount = static_cast<std::size_t>(stats.droppedFailedCount);
        if (succeededCount + droppedCount >= totalItems_) {
            totalItems_ = 0;
        } else {
            totalItems_ -= (succeededCount + droppedCount);
        }

        lastCollectStats_ = stats;
    }

    for (const FailureEscalationEvent& escalation : escalations) {
        reportEscalation(escalation);
    }

    return hasFailure ? firstFailure : vkutil::VkExpected<void>{};
}

vkutil::VkExpected<void> DeletionQueue::flush(uint64_t frameIndex) noexcept
{
    return collect(UINT64_MAX, frameIndex);
}

bool DeletionQueue::runStressSelfTest() noexcept
{
    constexpr uint64_t kFenceReady = 100;
    constexpr std::size_t kTaskCount = 64;
    constexpr std::size_t kFailureIndex = 27;

    auto buildQueue = [](FailurePolicy policy) {
        auto queue = std::make_unique<DeletionQueue>(kTaskCount, DrainOrder::FIFO);
        queue->setFailurePolicy(policy);
        auto executed = std::make_shared<std::size_t>(0);
        for (std::size_t i = 0; i < kTaskCount; ++i) {
            queue->enqueue(kFenceReady, DeleteTask{
                [executed, i]() -> vkutil::VkExpected<void>
                {
                    ++(*executed);
                    if (i == kFailureIndex) {
                        return vkutil::checkResult(VK_ERROR_UNKNOWN, "DeletionQueue::runStressSelfTest", "deletion_queue");
                    }
                    return {};
                }});
        }
        return std::pair<std::unique_ptr<DeletionQueue>, std::shared_ptr<std::size_t>>(std::move(queue), std::move(executed));
    };

    {
        auto [queue, executed] = buildQueue(FailurePolicy::DiscardFailedTasks);
        const auto first = queue->collect(kFenceReady, 1);
        if (!first.hasValue()) {
            return false;
        }
        if (*executed != kTaskCount) {
            return false;
        }
        const auto stats = queue->lastCollectStats();
        if (stats.executedCount != kTaskCount || stats.failureCount != 1 || stats.successCount != kTaskCount - 1 || stats.retainedFailedCount != 0) {
            return false;
        }
        if (!queue->empty()) {
            return false;
        }
    }

    {
        auto [queue, executed] = buildQueue(FailurePolicy::KeepFailedTasks);
        const auto first = queue->collect(kFenceReady, 1);
        if (!first.hasValue()) {
            return false;
        }
        const auto stats = queue->lastCollectStats();
        if (stats.executedCount != kTaskCount || stats.failureCount != 1 || stats.successCount != kTaskCount - 1 || stats.retainedFailedCount != 1) {
            return false;
        }
        if (queue->size() != 1) {
            return false;
        }

        const auto second = queue->flush(2);
        if (!second.hasValue()) {
            return false;
        }
        const auto secondStats = queue->lastCollectStats();
        if (secondStats.executedCount != 1 || secondStats.failureCount != 1 || secondStats.retainedFailedCount != 1) {
            return false;
        }
        if (queue->size() != 1) {
            return false;
        }

        if (*executed != kTaskCount + 1) {
            return false;
        }
    }

    return true;
}
