#include <algorithm>
 #include "DeferredDeletionService.h"

size_t DeferredDeletionService::queueClassIndex(QueueClass queueClass) noexcept
{
    return static_cast<size_t>(queueClass);
}

DeferredDeletionService& DeferredDeletionService::instance()
{
    static DeferredDeletionService service;
    return service;
}

std::shared_ptr<DeferredDeletionService::DeviceState> DeferredDeletionService::ensureRegisteredDeviceStateLocked(VkDevice device)
{
    if (device == VK_NULL_HANDLE) {
        return {};
    }

    std::lock_guard<std::mutex> lock(devicesMutex_);
    auto [it, inserted] = devices_.try_emplace(device, std::make_shared<DeviceState>());
    std::shared_ptr<DeviceState> state = it->second;
    if (!state) {
        return {};
    }

    std::lock_guard<std::mutex> stateLock(state->mutex);
    if (!state->queue) {
        state->queue = std::make_shared<DeletionQueue>();
    }

    state->adapters.clear();
    state->submittedValue = 0;
    state->submittedByQueueClass.fill(0);
    state->submittedByQueueFamily.clear();
    state->generation = nextGeneration_.fetch_add(1, std::memory_order_relaxed);
    if (state->generation == 0) {
        state->generation = nextGeneration_.fetch_add(1, std::memory_order_relaxed);
    }
    state->lifecycle = DeviceLifecycle::Registered;
    return state;
}

std::shared_ptr<DeferredDeletionService::DeviceState> DeferredDeletionService::findRegisteredDeviceState(VkDevice device) const
{
    if (device == VK_NULL_HANDLE) {
        return {};
    }

    std::lock_guard<std::mutex> lock(devicesMutex_);
    const auto it = devices_.find(device);
    if (it == devices_.end()) {
        return {};
    }
    return it->second;
}

void DeferredDeletionService::registerDevice(VkDevice device)
{
    static_cast<void>(ensureRegisteredDeviceStateLocked(device));
}

void DeferredDeletionService::unregisterDevice(VkDevice device)
{
    if (device == VK_NULL_HANDLE) {
        return;
    }

    std::shared_ptr<DeviceState> state;
    {
        std::lock_guard<std::mutex> lock(devicesMutex_);
        auto it = devices_.find(device);
        if (it == devices_.end()) {
            return;
        }
        state = it->second;
        devices_.erase(it);
    }

    if (!state) {
        return;
    }

    std::shared_ptr<DeletionQueue> queue;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->lifecycle = DeviceLifecycle::Unregistering;
        queue = state->queue;
        state->adapters.clear();
    }

    if (queue) {
        static_cast<void>(queue->flush());
    }

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->lifecycle = DeviceLifecycle::Dead;
    }
}

void DeferredDeletionService::updateSubmittedValue(VkDevice device, uint64_t submittedValue)
{
    updateSubmittedTicket(device, SubmissionTicket{ .value = submittedValue, .queueClass = QueueClass::Generic });
}

void DeferredDeletionService::updateSubmittedTicket(VkDevice device, SubmissionTicket ticket)
{
    if (device == VK_NULL_HANDLE || !ticket.valid()) {
        return;
    }

    auto state = findRegisteredDeviceState(device);
    if (!state) {
        return;
    }

    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->lifecycle != DeviceLifecycle::Registered) {
        return;
    }

    state->submittedValue = std::max(state->submittedValue, ticket.value);
    const size_t idx = queueClassIndex(ticket.queueClass);
    if (idx < state->submittedByQueueClass.size()) {
        state->submittedByQueueClass[idx] = std::max(state->submittedByQueueClass[idx], ticket.value);
    }
    if (ticket.queueFamilyIndex != UINT32_MAX) {
        auto [it, inserted] = state->submittedByQueueFamily.emplace(ticket.queueFamilyIndex, ticket.value);
        if (!inserted) {
            it->second = std::max(it->second, ticket.value);
        }
    }
}

void DeferredDeletionService::enqueueAfter(VkDevice device, uint64_t retireAfter, DeletionQueue::DeleteTask&& task)
{
    enqueue(device, retireAfter, std::move(task));
}

void DeferredDeletionService::enqueue(VkDevice device, uint64_t retireAfter, DeletionQueue::DeleteTask&& task)
{
    auto state = findRegisteredDeviceState(device);
    if (!state) {
        return;
    }

    std::shared_ptr<DeletionQueue> queue;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->lifecycle != DeviceLifecycle::Registered) {
            return;
        }
        queue = state->queue;
    }
    if (queue) {
        queue->enqueue(retireAfter, std::move(task));
    }
}

uint64_t DeferredDeletionService::submittedValue(VkDevice device) const
{
    auto state = findRegisteredDeviceState(device);
    if (!state) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->lifecycle != DeviceLifecycle::Registered) {
        return 0;
    }
    return state->submittedValue;
}

uint64_t DeferredDeletionService::currentRetireValue(VkDevice device) const
{
    auto state = findRegisteredDeviceState(device);
    if (!state) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->lifecycle != DeviceLifecycle::Registered) {
        return 0;
    }

    uint64_t retireValue = state->submittedValue;
    for (uint64_t trackedValue : state->submittedByQueueClass) {
        retireValue = std::max(retireValue, trackedValue);
    }
    for (const auto& [_, trackedValue] : state->submittedByQueueFamily) {
        retireValue = std::max(retireValue, trackedValue);
    }
    return retireValue;
}

bool DeferredDeletionService::isCurrentGeneration(VkDevice device, uint64_t generation) const
{
    auto state = findRegisteredDeviceState(device);
    if (!state) {
        return false;
    }

    std::lock_guard<std::mutex> lock(state->mutex);
    return state->lifecycle == DeviceLifecycle::Registered && state->generation == generation;
}

vkutil::VkExpected<void> DeferredDeletionService::collect(VkDevice device, uint64_t completedValue, uint64_t frameIndex)
{
    auto state = findRegisteredDeviceState(device);
    if (!state) {
        return {};
    }

    std::shared_ptr<DeletionQueue> queue;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->lifecycle != DeviceLifecycle::Registered) {
            return {};
        }
        queue = state->queue;
    }

    if (!queue) {
        return {};
    }
    return queue->collect(completedValue, frameIndex);
}

vkutil::VkExpected<void> DeferredDeletionService::flush(VkDevice device, uint64_t frameIndex)
{
    auto state = findRegisteredDeviceState(device);
    if (!state) {
        return {};
    }

    std::shared_ptr<DeletionQueue> queue;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->lifecycle != DeviceLifecycle::Registered && state->lifecycle != DeviceLifecycle::Unregistering) {
            return {};
        }
        queue = state->queue;
    }

    if (!queue) {
        return {};
    }
    return queue->flush(frameIndex);
}
