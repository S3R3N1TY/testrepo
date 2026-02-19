// DeletionQueue.h
#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>
#include <deque>
#include <unordered_map>
#include <map>

 #include "VkUtils.h"

class DeletionQueue
{
public:
    using DeleteTask = std::move_only_function<vkutil::VkExpected<void>()>;

    enum class DrainOrder : uint8_t { FIFO, LIFO };
    enum class FailurePolicy : uint8_t {
        KeepFailedTasks,
        DiscardFailedTasks
    };

    struct RetryPolicy {
        uint32_t maxRetries{ 8 };
        uint64_t maxFrameAge{ 512 };
        uint64_t baseRetryBackoffFrames{ 1 };
        bool hardFailInDebug{ false };
    };

    struct CollectStats {
        uint32_t executedCount{ 0 };
        uint32_t successCount{ 0 };
        uint32_t failureCount{ 0 };
        uint32_t retainedFailedCount{ 0 };
        uint32_t droppedFailedCount{ 0 };
    };

    struct FailureEscalationEvent {
        uint64_t fenceValue{ 0 };
        uint32_t retryCount{ 0 };
        uint64_t firstFailureFrame{ 0 };
        uint64_t currentFrame{ 0 };
        const char* reason{ "unknown" };
    };

    using FailureEscalationHook = std::function<void(const FailureEscalationEvent&)>;

    DeletionQueue() = default;
    explicit DeletionQueue(std::size_t reserveCount, DrainOrder order = DrainOrder::FIFO);

    void reserve(std::size_t count);

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

    void clear();

    void enqueue(uint64_t fenceValue, DeleteTask&& fn);
    void enqueue(uint64_t fenceValue, std::move_only_function<void()>&& fn);

    [[nodiscard]] vkutil::VkExpected<void> collect(uint64_t completedValue, uint64_t frameIndex = 0) noexcept;

    [[nodiscard]] vkutil::VkExpected<void> flush(uint64_t frameIndex = 0) noexcept;

    void setDrainOrder(DrainOrder order) noexcept;
    [[nodiscard]] DrainOrder getDrainOrder() const noexcept;

    void setFailurePolicy(FailurePolicy policy) noexcept;
    [[nodiscard]] FailurePolicy getFailurePolicy() const noexcept;
    [[nodiscard]] CollectStats lastCollectStats() const noexcept;

    void setRetryPolicy(const RetryPolicy& policy) noexcept;
    [[nodiscard]] RetryPolicy getRetryPolicy() const noexcept;
    void setFailureEscalationHook(FailureEscalationHook hook);

    [[nodiscard]] static bool runStressSelfTest() noexcept;

private:
    struct Item
    {
        uint64_t fenceValue = 0;
        DeleteTask fn;
        uint32_t retryCount{ 0 };
        uint64_t firstFailureFrame{ 0 };
        uint64_t nextRetryFrame{ 0 };

        Item(uint64_t value, DeleteTask&& f)
            : fenceValue(value), fn(std::move(f))
        {
        }
    };

    struct IngressBatch {
        std::vector<Item> items{};
    };

    static vkutil::VkExpected<void> executeTask(DeleteTask& fn, uint64_t frameIndex) noexcept;
    [[nodiscard]] bool shouldAttemptNow(const Item& item, uint64_t frameIndex) const noexcept;
    [[nodiscard]] static bool shouldRetainFailed(Item& item, uint64_t frameIndex, const RetryPolicy& policy, FailureEscalationEvent& escalation) noexcept;
    void reportEscalation(const FailureEscalationEvent& event) const;
    void drainIngressLocked();
    [[nodiscard]] std::size_t sizeLocked() const noexcept;

    mutable std::mutex mutex_{};
    std::map<uint64_t, std::deque<Item>> readyByFence_{};
    std::size_t totalItems_{ 0 };

    mutable std::mutex ingressMutex_{};
    std::vector<Item> ingressItems_{};

    DrainOrder drainOrder = DrainOrder::FIFO;
    FailurePolicy failurePolicy = FailurePolicy::KeepFailedTasks;
    RetryPolicy retryPolicy{};
    FailureEscalationHook escalationHook_{};
    CollectStats lastCollectStats_{};
};
