#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <memory>
#include <optional>
#include <atomic>
#include <array>
#include <cstdio>
#include <cstdlib>

 #include "DeletionQueue.h"
 #include "UniqueHandle.h"

class DeferredDeletionService {
public:
    struct AdapterBase {
        virtual ~AdapterBase() = default;
    };

    enum class QueueClass : uint8_t {
        Graphics = 0,
        Present,
        Transfer,
        Compute,
        Generic
    };

    struct SubmissionTicket {
        uint64_t value{ 0 };
        QueueClass queueClass{ QueueClass::Generic };
        uint32_t queueFamilyIndex{ UINT32_MAX };

        [[nodiscard]] bool valid() const noexcept { return value > 0; }
    };

    static DeferredDeletionService& instance();

    void registerDevice(VkDevice device);
    void unregisterDevice(VkDevice device);

    void updateSubmittedValue(VkDevice device, uint64_t submittedValue);
    void updateSubmittedTicket(VkDevice device, SubmissionTicket ticket);
    void enqueueAfter(VkDevice device, uint64_t retireAfter, DeletionQueue::DeleteTask&& task);
    vkutil::VkExpected<void> collect(VkDevice device, uint64_t completedValue, uint64_t frameIndex = 0);
    vkutil::VkExpected<void> flush(VkDevice device, uint64_t frameIndex = 0);

    template<typename Handle, typename DestroyFn>
    class DeviceQueue final : public AdapterBase, public vkhandle::IDeletionQueue<VkDevice, Handle, DestroyFn> {
    public:
        DeviceQueue(VkDevice device, uint64_t generation)
            : device_(device)
            , generation_(generation)
        {
        }

        [[nodiscard]] bool requiresRetireValue() const noexcept override { return true; }

        void enqueue(VkDevice parent,
            Handle handle,
            DestroyFn destroyFn,
            std::optional<VkAllocationCallbacks> allocator,
            std::optional<uint64_t> retireAfterValue) noexcept override
        {
            if (parent == VK_NULL_HANDLE || handle == Handle{}) {
                return;
            }
            const VkAllocationCallbacks* allocatorPtr = allocator.has_value() ? &allocator.value() : nullptr;
            if (!DeferredDeletionService::instance().isCurrentGeneration(parent, generation_)) {
                destroyFn(parent, handle, allocatorPtr);
                return;
            }
            if (!retireAfterValue.has_value()) {
                reportMissingRetireValue();
            }
            DeferredDeletionService::instance().enqueueAfter(parent, retireAfterValue.value(), DeletionQueue::DeleteTask{
                [parent, handle, destroyFn, allocator = std::move(allocator)]() -> vkutil::VkExpected<void>
                {
                    const VkAllocationCallbacks* deferredAllocator = allocator.has_value() ? &allocator.value() : nullptr;
                    destroyFn(parent, handle, deferredAllocator);
                    return {};
                }});
        }

    private:
        [[noreturn]] static void reportMissingRetireValue() noexcept
        {
            std::fputs("DeferredDeletionService invariant violation: missing retire value for deferred destruction\n", stderr);
            std::abort();
        }

        VkDevice device_{ VK_NULL_HANDLE };
        uint64_t generation_{ 0 };
    };

    template<typename Handle, typename DestroyFn>
    std::shared_ptr<vkhandle::IDeletionQueue<VkDevice, Handle, DestroyFn>> getDeviceQueue(VkDevice device)
    {
        auto state = findRegisteredDeviceState(device);
        if (!state) {
            return {};
        }

        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->lifecycle != DeviceLifecycle::Registered) {
            return {};
        }

        const void* key = adapterKey<Handle, DestroyFn>();
        auto it = state->adapters.find(key);
        if (it == state->adapters.end()) {
            auto adapter = std::make_shared<DeviceQueue<Handle, DestroyFn>>(device, state->generation);
            state->adapters.emplace(key, adapter);
            return adapter;
        }

        auto typedAdapter = std::static_pointer_cast<DeviceQueue<Handle, DestroyFn>>(it->second);
        return typedAdapter;
    }

    template<typename Handle, typename DestroyFn>
    [[nodiscard]] vkhandle::DeviceUniqueHandle<Handle, DestroyFn> makeDeferredHandle(VkDevice device, Handle handle, DestroyFn destroyFn)
    {
        return makeDeferredHandleAfter<Handle, DestroyFn>(device, handle, destroyFn, currentRetireValue(device));
    }

    template<typename Handle, typename DestroyFn>
    [[nodiscard]] vkhandle::DeviceUniqueHandle<Handle, DestroyFn> makeDeferredHandleAfter(
        VkDevice device,
        Handle handle,
        DestroyFn destroyFn,
        std::optional<uint64_t> retireAfterValue)
    {
        auto queue = getDeviceQueue<Handle, DestroyFn>(device);
        auto deferred = vkhandle::DeviceUniqueHandle<Handle, DestroyFn>(
            device,
            handle,
            destroyFn,
            nullptr,
            queue,
            vkhandle::DeleteMode::Deferred,
            vkhandle::DeferredFallbackPolicy::StrictRequireQueue);
        deferred.setDeferredRetireAfterValue(retireAfterValue);
        return deferred;
    }

private:
    enum class DeviceLifecycle : uint8_t {
        Dead = 0,
        Registered,
        Unregistering
    };

    struct DeviceState {
        std::shared_ptr<DeletionQueue> queue{};
        std::unordered_map<const void*, std::shared_ptr<AdapterBase>> adapters{};
        uint64_t submittedValue{ 0 };
        std::array<uint64_t, 5> submittedByQueueClass{};
        std::unordered_map<uint32_t, uint64_t> submittedByQueueFamily{};
        uint64_t generation{ 0 };
        DeviceLifecycle lifecycle{ DeviceLifecycle::Dead };
        mutable std::mutex mutex{};
    };

    DeferredDeletionService() = default;

    template<typename Handle, typename DestroyFn>
    static const void* adapterKey() noexcept
    {
        static const int kTypeKey = 0;
        return &kTypeKey;
    }

    [[nodiscard]] std::shared_ptr<DeviceState> ensureRegisteredDeviceStateLocked(VkDevice device);
    [[nodiscard]] std::shared_ptr<DeviceState> findRegisteredDeviceState(VkDevice device) const;
    void enqueue(VkDevice device, uint64_t retireAfter, DeletionQueue::DeleteTask&& task);
    [[nodiscard]] uint64_t submittedValue(VkDevice device) const;
    [[nodiscard]] uint64_t currentRetireValue(VkDevice device) const;
    [[nodiscard]] static size_t queueClassIndex(QueueClass queueClass) noexcept;
    [[nodiscard]] bool isCurrentGeneration(VkDevice device, uint64_t generation) const;

    mutable std::mutex devicesMutex_{};
    std::unordered_map<VkDevice, std::shared_ptr<DeviceState>> devices_{};
    std::atomic<uint64_t> nextGeneration_{ 1 };
};
