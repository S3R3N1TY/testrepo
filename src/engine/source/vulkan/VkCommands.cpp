#include "VkCommands.h"

#include <utility>
#include <stdexcept>
#include <limits>
#include <cassert>


namespace {
constexpr uint8_t toLifecycleByte(VulkanCommandArena::FrameLifecycleState lifecycle) noexcept
{
    return static_cast<uint8_t>(lifecycle);
}

VulkanCommandArena::FrameLifecycleState fromLifecycleByte(uint8_t lifecycle) noexcept
{
    return static_cast<VulkanCommandArena::FrameLifecycleState>(lifecycle);
}
}

vkutil::VkExpected<VulkanCommandPool> VulkanCommandPool::create(VkDevice device, uint32_t queueFamilyIndex)
{
    if (device == VK_NULL_HANDLE) {
        return vkutil::VkExpected<VulkanCommandPool>(
            vkutil::makeError("VulkanCommandPool::create", VK_ERROR_INITIALIZATION_FAILED, "command_pool").context());
    }

    VkCommandPoolCreateInfo ci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    ci.queueFamilyIndex = queueFamilyIndex;
    ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    VkCommandPool created = VK_NULL_HANDLE;
    const VkResult res = vkCreateCommandPool(device, &ci, nullptr, &created);
    if (res != VK_SUCCESS) {
        return vkutil::VkExpected<VulkanCommandPool>(
            vkutil::checkResult(res, "vkCreateCommandPool", "command_pool").context());
    }

    return VulkanCommandPool(device, created);
}

VulkanCommandPool::VulkanCommandPool(VulkanCommandPool&& other) noexcept
    : device(std::exchange(other.device, VK_NULL_HANDLE))
    , commandPool(std::exchange(other.commandPool, VK_NULL_HANDLE))
{
}

VulkanCommandPool& VulkanCommandPool::operator=(VulkanCommandPool&& other) noexcept
{
    if (this != &other) {
        reset();
        device = std::exchange(other.device, VK_NULL_HANDLE);
        commandPool = std::exchange(other.commandPool, VK_NULL_HANDLE);
    }
    return *this;
}

VulkanCommandPool::~VulkanCommandPool() noexcept
{
    reset();
}

void VulkanCommandPool::reset() noexcept
{
    if (commandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device, commandPool, nullptr);
        commandPool = VK_NULL_HANDLE;
    }
    device = VK_NULL_HANDLE;
}

vkutil::VkExpected<VulkanCommandBuffer> VulkanCommandBuffer::create(VkDevice device, VkCommandPool commandPool)
{
    if (device == VK_NULL_HANDLE) {
        return vkutil::VkExpected<VulkanCommandBuffer>(
            vkutil::makeError("VulkanCommandBuffer::create", VK_ERROR_INITIALIZATION_FAILED, "command_buffer").context());
    }
    if (commandPool == VK_NULL_HANDLE) {
        return vkutil::VkExpected<VulkanCommandBuffer>(
            vkutil::makeError("VulkanCommandBuffer::create", VK_ERROR_INITIALIZATION_FAILED, "command_buffer").context());
    }

    VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = commandPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;

    VkCommandBuffer created = VK_NULL_HANDLE;
    const VkResult res = vkAllocateCommandBuffers(device, &ai, &created);
    if (res != VK_SUCCESS) {
        return vkutil::VkExpected<VulkanCommandBuffer>(
            vkutil::checkResult(res, "vkAllocateCommandBuffers", "command_buffer").context());
    }

    return VulkanCommandBuffer(device, commandPool, created);
}

VulkanCommandBuffer::VulkanCommandBuffer(VulkanCommandBuffer&& other) noexcept
    : device(std::exchange(other.device, VK_NULL_HANDLE))
    , commandPool(std::exchange(other.commandPool, VK_NULL_HANDLE))
    , commandBuffer(std::exchange(other.commandBuffer, VK_NULL_HANDLE))
{
}

VulkanCommandBuffer& VulkanCommandBuffer::operator=(VulkanCommandBuffer&& other) noexcept
{
    if (this != &other) {
        reset();
        device = std::exchange(other.device, VK_NULL_HANDLE);
        commandPool = std::exchange(other.commandPool, VK_NULL_HANDLE);
        commandBuffer = std::exchange(other.commandBuffer, VK_NULL_HANDLE);
    }
    return *this;
}

VulkanCommandBuffer::~VulkanCommandBuffer() noexcept
{
    reset();
}

void VulkanCommandBuffer::reset() noexcept
{
    if (commandBuffer != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
        commandBuffer = VK_NULL_HANDLE;
    }

    device = VK_NULL_HANDLE;
    commandPool = VK_NULL_HANDLE;
}

vkutil::VkExpected<void> VulkanCommandBuffer::resetRecording(VkCommandBufferResetFlags flags)
{
    if (commandBuffer == VK_NULL_HANDLE) {
        return vkutil::makeError("VulkanCommandBuffer::resetRecording", VK_ERROR_INITIALIZATION_FAILED, "command_buffer");
    }

    VKUTIL_RETURN_IF_FAILED(vkResetCommandBuffer(commandBuffer, flags), "vkResetCommandBuffer", "command_buffer");
    return {};
}

vkutil::VkExpected<void> VulkanCommandBuffer::begin(VkCommandBufferUsageFlags flags)
{
    if (commandBuffer == VK_NULL_HANDLE) {
        return vkutil::makeError("VulkanCommandBuffer::begin", VK_ERROR_INITIALIZATION_FAILED, "command_buffer");
    }

    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = flags;
    bi.pInheritanceInfo = nullptr;

    VKUTIL_RETURN_IF_FAILED(vkBeginCommandBuffer(commandBuffer, &bi), "vkBeginCommandBuffer", "command_buffer");
    return {};
}

vkutil::VkExpected<void> VulkanCommandBuffer::beginSecondary(const VkCommandBufferInheritanceInfo& inheritance, VkCommandBufferUsageFlags flags)
{
    if (commandBuffer == VK_NULL_HANDLE) {
        return vkutil::makeError("VulkanCommandBuffer::beginSecondary", VK_ERROR_INITIALIZATION_FAILED, "command_buffer");
    }

    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = flags | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
    bi.pInheritanceInfo = &inheritance;

    VKUTIL_RETURN_IF_FAILED(vkBeginCommandBuffer(commandBuffer, &bi), "vkBeginCommandBuffer", "command_buffer");
    return {};
}

vkutil::VkExpected<void> VulkanCommandBuffer::end()
{
    if (commandBuffer == VK_NULL_HANDLE) {
        return vkutil::makeError("VulkanCommandBuffer::end", VK_ERROR_INITIALIZATION_FAILED, "command_buffer");
    }

    VKUTIL_RETURN_IF_FAILED(vkEndCommandBuffer(commandBuffer), "vkEndCommandBuffer", "command_buffer");
    return {};
}

const char* VulkanCommandArena::BorrowedValidation::reason() const noexcept
{
    if (valid) return "ok";
    if (invalidHandle) return "invalid_handle";
    if (invalidWorkerIndex) return "invalid_worker_index";
    if (invalidFrameIndex) return "invalid_frame_index";
    if (staleGeneration) return "stale_generation";
    if (staleEpoch) return "stale_epoch";
    return "unknown";
}

VulkanCommandArena::CommandRecorder::~CommandRecorder() noexcept
{
    if (!finished_) {
        static_cast<void>(finish());
#ifndef NDEBUG
        assert(finished_ && "CommandRecorder destroyed without successful finish()");
#endif
    }
}

vkutil::VkExpected<void> VulkanCommandArena::CommandRecorder::finish() noexcept
{
    if (finished_) {
        return {};
    }
    if (arena_ == nullptr || !borrowed_.valid()) {
        finished_ = true;
        return vkutil::makeError("VulkanCommandArena::CommandRecorder::finish", VK_ERROR_INITIALIZATION_FAILED, "command_arena", "invalid_recorder");
    }

    auto result = arena_->endBorrowed(borrowed_);
    finished_ = true;
    arena_ = nullptr;
    borrowed_ = BorrowedCommandBuffer{};
    return result;
}

VulkanCommandArena::VulkanCommandArena(const Config& config)
{
    const auto initResult = init(config);
    if (!initResult.hasValue()) {
        vkutil::throwVkError("VulkanCommandArena::VulkanCommandArena", initResult.error());
    }
}

vkutil::VkExpected<VulkanCommandArena> VulkanCommandArena::createResult(const Config& config)
{
    VulkanCommandArena out{};
    const auto initResult = out.init(config);
    if (!initResult.hasValue()) {
        return vkutil::VkExpected<VulkanCommandArena>(initResult.context());
    }
    return out;
}

vkutil::VkExpected<void> VulkanCommandArena::init(const Config& config)
{
    if (config.device == VK_NULL_HANDLE || config.framesInFlight == 0 || config.workerThreads == 0) {
        return vkutil::makeError("VulkanCommandArena::init", VK_ERROR_INITIALIZATION_FAILED, "command_arena");
    }

    device_ = config.device;
    framesInFlight_ = config.framesInFlight;
    waitForIdleOnDestroy_ = config.waitForIdleOnDestroy;

    frameSync_.resize(framesInFlight_);
    frameTransitionMutexes_.resize(framesInFlight_);
    for (uint32_t i = 0; i < framesInFlight_; ++i) {
        frameTransitionMutexes_[i] = std::make_shared<std::mutex>();
        storeFrameSyncStateLocked(i, FrameSyncState{ .lifecycle = FrameLifecycleState::Available, .signaled = true, .ticket = {} });
    }

    workers_.resize(config.workerThreads);
    for (auto& worker : workers_) {
        worker.resize(framesInFlight_);
        for (FrameState& frame : worker) {
            VkCommandPoolCreateInfo info{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
            info.queueFamilyIndex = config.queueFamilyIndex;
            info.flags = config.poolFlags;
            VkResult res = vkCreateCommandPool(device_, &info, nullptr, &frame.pool);
            if (res != VK_SUCCESS) {
                return vkutil::checkResult(res, "vkCreateCommandPool", "command_arena");
            }

            if (config.preallocatePerFrame > 0) {
                frame.primaryBuffers.resize(config.preallocatePerFrame, VK_NULL_HANDLE);
                VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
                ai.commandPool = frame.pool;
                ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                ai.commandBufferCount = config.preallocatePerFrame;
                res = vkAllocateCommandBuffers(device_, &ai, frame.primaryBuffers.data());
                if (res != VK_SUCCESS) {
                    return vkutil::checkResult(res, "vkAllocateCommandBuffers(primary)", "command_arena");
                }

                frame.secondaryBuffers.resize(config.preallocatePerFrame, VK_NULL_HANDLE);
                ai.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
                res = vkAllocateCommandBuffers(device_, &ai, frame.secondaryBuffers.data());
                if (res != VK_SUCCESS) {
                    return vkutil::checkResult(res, "vkAllocateCommandBuffers(secondary)", "command_arena");
                }
            }
        }
    }

    return {};
}

std::unique_lock<std::mutex> VulkanCommandArena::lockFrameTransition(uint32_t frameIndex)
{
    assert(frameIndex < frameTransitionMutexes_.size());
    assert(frameTransitionMutexes_[frameIndex] != nullptr);
    return std::unique_lock<std::mutex>(*frameTransitionMutexes_[frameIndex]);
}

VulkanCommandArena::FrameSyncState VulkanCommandArena::loadFrameSyncStateLocked(uint32_t frameIndex) const noexcept
{
    const AtomicFrameSyncState& state = frameSync_[frameIndex];
    FrameSyncState out{};
    out.lifecycle = fromLifecycleByte(state.lifecycle.load(std::memory_order_acquire));
    out.signaled = state.signaled.load(std::memory_order_acquire);
    out.ticket.value = state.ticketValue.load(std::memory_order_acquire);
    out.ticket.frameIndex = state.ticketFrameIndex.load(std::memory_order_acquire);
    return out;
}

void VulkanCommandArena::storeFrameSyncStateLocked(uint32_t frameIndex, const FrameSyncState& state) noexcept
{
    AtomicFrameSyncState& dst = frameSync_[frameIndex];
    dst.ticketFrameIndex.store(state.ticket.frameIndex, std::memory_order_release);
    dst.ticketValue.store(state.ticket.value, std::memory_order_release);
    dst.signaled.store(state.signaled, std::memory_order_release);
    dst.lifecycle.store(toLifecycleByte(state.lifecycle), std::memory_order_release);
}

vkutil::VkExpected<bool> VulkanCommandArena::updateFrameSyncState(uint32_t frameIndex, FrameWaitPolicy waitPolicy)
{
    if (frameIndex >= framesInFlight_) {
        return vkutil::VkExpected<bool>(vkutil::makeError("VulkanCommandArena::updateFrameSyncState", VK_ERROR_INITIALIZATION_FAILED, "command_arena").context());
    }

    const auto frameLock = lockFrameTransition(frameIndex);
    FrameSyncState state = loadFrameSyncStateLocked(frameIndex);
    if (state.signaled) {
        state.lifecycle = FrameLifecycleState::Available;
        storeFrameSyncStateLocked(frameIndex, state);
        return vkutil::VkExpected<bool>(true);
    }

    if (syncContext_ == nullptr) {
        return vkutil::VkExpected<bool>(false);
    }

    if (waitPolicy == FrameWaitPolicy::Wait) {
        const auto waitRes = syncContext_->waitTicket(state.ticket, UINT64_MAX);
        if (!waitRes.hasValue()) {
            return vkutil::VkExpected<bool>(waitRes.context());
        }
        if (!waitRes.value()) {
            state.lifecycle = FrameLifecycleState::InFlight;
            storeFrameSyncStateLocked(frameIndex, state);
            return vkutil::VkExpected<bool>(false);
        }
        state.signaled = true;
        state.lifecycle = FrameLifecycleState::Available;
        storeFrameSyncStateLocked(frameIndex, state);
        return vkutil::VkExpected<bool>(true);
    }

    const auto completeRes = syncContext_->isTicketComplete(state.ticket);
    if (!completeRes.hasValue()) {
        return vkutil::VkExpected<bool>(completeRes.context());
    }

    if (completeRes.value()) {
        state.signaled = true;
        state.lifecycle = FrameLifecycleState::Available;
        storeFrameSyncStateLocked(frameIndex, state);
        return vkutil::VkExpected<bool>(true);
    }

    state.lifecycle = FrameLifecycleState::InFlight;
    storeFrameSyncStateLocked(frameIndex, state);
    return vkutil::VkExpected<bool>(false);
}

vkutil::VkExpected<VulkanCommandArena::FrameToken> VulkanCommandArena::beginFrameInternal(uint32_t frameIndex, std::optional<FrameSyncState> observedCompletion)
{
    if (frameIndex >= framesInFlight_) {
        return vkutil::VkExpected<VulkanCommandArena::FrameToken>(vkutil::makeError("VulkanCommandArena::beginFrame", VK_ERROR_INITIALIZATION_FAILED, "command_arena").context());
    }

    const auto frameLock = lockFrameTransition(frameIndex);
    return beginFrameInternalLocked(frameIndex, std::move(observedCompletion));
}

vkutil::VkExpected<VulkanCommandArena::FrameToken> VulkanCommandArena::beginFrameInternalLocked(uint32_t frameIndex, std::optional<FrameSyncState> observedCompletion)
{
    if (observedCompletion.has_value()) {
        storeFrameSyncStateLocked(frameIndex, *observedCompletion);
    }

    const FrameSyncState state = loadFrameSyncStateLocked(frameIndex);
    if (!state.signaled) {
        return vkutil::VkExpected<VulkanCommandArena::FrameToken>(vkutil::makeError("VulkanCommandArena::beginFrame", VK_NOT_READY, "command_arena", nullptr, 0, true).context());
    }

    uint64_t epoch = 0;
    for (auto& worker : workers_) {
        FrameState& frame = worker[frameIndex];
        std::lock_guard<std::mutex> lock(*frame.mutex);
        const VkResult res = vkResetCommandPool(device_, frame.pool, 0);
        if (res != VK_SUCCESS) {
            return vkutil::VkExpected<FrameToken>(
                vkutil::checkResult(res, "vkResetCommandPool", "command_arena").context());
        }
        frame.nextPrimary = 0;
        frame.nextSecondary = 0;
        const uint64_t frameEpoch = frame.generation->fetch_add(1, std::memory_order_acq_rel) + 1;
        if (epoch == 0 || frameEpoch < epoch) {
            epoch = frameEpoch;
        }
    }

    AtomicFrameSyncState& syncState = frameSync_[frameIndex];
    syncState.frameEpoch.store(epoch, std::memory_order_release);
    storeFrameSyncStateLocked(frameIndex, FrameSyncState{ .lifecycle = FrameLifecycleState::Retired, .signaled = false, .ticket = {} });
    return VulkanCommandArena::FrameToken{ .frameIndex = frameIndex, .epoch = epoch };
}

vkutil::VkExpected<VulkanCommandArena::FrameToken> VulkanCommandArena::beginFrame(uint32_t frameIndex, FrameWaitPolicy waitPolicy)
{
    const auto availableRes = updateFrameSyncState(frameIndex, waitPolicy);
    if (!availableRes.hasValue()) {
        return vkutil::VkExpected<VulkanCommandArena::FrameToken>(availableRes.context());
    }
    if (!availableRes.value()) {
        return vkutil::VkExpected<VulkanCommandArena::FrameToken>(vkutil::makeError("VulkanCommandArena::beginFrame", VK_NOT_READY, "command_arena", nullptr, 0, true).context());
    }
    return beginFrameInternal(frameIndex, std::nullopt);
}

vkutil::VkExpected<VulkanCommandArena::FrameToken> VulkanCommandArena::beginFrame(uint32_t frameIndex, VkFence frameFence)
{
    if (frameFence == VK_NULL_HANDLE) {
        return vkutil::VkExpected<VulkanCommandArena::FrameToken>(vkutil::makeError("VulkanCommandArena::beginFrame(fence)", VK_ERROR_INITIALIZATION_FAILED, "command_arena").context());
    }

    const VkResult fenceStatus = vkGetFenceStatus(device_, frameFence);
    if (fenceStatus == VK_NOT_READY) {
        return vkutil::VkExpected<VulkanCommandArena::FrameToken>(vkutil::makeError("VulkanCommandArena::beginFrame(fence)", VK_NOT_READY, "command_arena", nullptr, 0, true).context());
    }
    if (fenceStatus != VK_SUCCESS) {
        return vkutil::VkExpected<VulkanCommandArena::FrameToken>(vkutil::checkResult(fenceStatus, "vkGetFenceStatus", "command_arena").context());
    }

    const auto frameLock = lockFrameTransition(frameIndex);
    const FrameSyncState currentState = loadFrameSyncStateLocked(frameIndex);
    const FrameSyncState observed{ .lifecycle = FrameLifecycleState::Available, .signaled = true,
        .ticket = SyncTicket{ .value = currentState.ticket.value, .frameIndex = frameIndex } };
    return beginFrameInternalLocked(frameIndex, observed);
}

vkutil::VkExpected<VulkanCommandArena::FrameToken> VulkanCommandArena::beginFrame(uint32_t frameIndex, uint64_t completedValue)
{
    if (frameIndex >= framesInFlight_) {
        return vkutil::VkExpected<VulkanCommandArena::FrameToken>(
            vkutil::makeError("VulkanCommandArena::beginFrame(timeline)", VK_ERROR_INITIALIZATION_FAILED, "command_arena", "invalid_frame_index").context());
    }

    const auto frameLock = lockFrameTransition(frameIndex);
    const FrameSyncState currentState = loadFrameSyncStateLocked(frameIndex);
    const bool complete = currentState.signaled || completedValue >= currentState.ticket.value;
    if (!complete) {
        return vkutil::VkExpected<VulkanCommandArena::FrameToken>(vkutil::makeError("VulkanCommandArena::beginFrame(timeline)", VK_NOT_READY, "command_arena", nullptr, 0, true).context());
    }

    const FrameSyncState observed{ .lifecycle = FrameLifecycleState::Available, .signaled = true,
        .ticket = SyncTicket{ .value = currentState.ticket.value, .frameIndex = frameIndex } };
    return beginFrameInternalLocked(frameIndex, observed);
}

void VulkanCommandArena::markFrameSubmitted(uint32_t frameIndex, uint64_t submissionValue) noexcept
{
    markFrameSubmitted(frameIndex, SyncTicket{ .value = submissionValue, .frameIndex = frameIndex });
}

void VulkanCommandArena::markFrameSubmitted(uint32_t frameIndex, const SyncTicket& ticket) noexcept
{
    if (frameIndex >= frameSync_.size()) {
        return;
    }

    const auto frameLock = lockFrameTransition(frameIndex);
    storeFrameSyncStateLocked(frameIndex, FrameSyncState{ .lifecycle = FrameLifecycleState::InFlight, .signaled = false, .ticket = ticket });
}

void VulkanCommandArena::markFrameComplete(uint32_t frameIndex) noexcept
{
    if (frameIndex >= frameSync_.size()) {
        return;
    }

    const auto frameLock = lockFrameTransition(frameIndex);
    storeFrameSyncStateLocked(frameIndex, FrameSyncState{ .lifecycle = FrameLifecycleState::Available, .signaled = true, .ticket = {} });
}

VulkanCommandArena::BorrowedValidation VulkanCommandArena::validateBorrowed(const BorrowedCommandBuffer& borrowed) const noexcept
{
    BorrowedValidation out{};
    if (!borrowed.valid()) {
        out.invalidHandle = true;
        return out;
    }
    if (borrowed.workerIndex >= workers_.size()) {
        out.invalidWorkerIndex = true;
        return out;
    }
    if (borrowed.frameIndex >= framesInFlight_) {
        out.invalidFrameIndex = true;
        return out;
    }

    const FrameState& frame = workers_[borrowed.workerIndex][borrowed.frameIndex];
    const uint64_t generation = frame.generation->load(std::memory_order_acquire);
    const uint64_t epoch = frameSync_[borrowed.frameIndex].frameEpoch.load(std::memory_order_acquire);
    out.staleGeneration = generation != borrowed.generation;
    out.staleEpoch = (epoch == 0) || (borrowed.epoch != epoch);
    out.valid = !out.staleGeneration && !out.staleEpoch;
    return out;
}

bool VulkanCommandArena::isBorrowedValid(const BorrowedCommandBuffer& borrowed) const noexcept
{
    return validateBorrowed(borrowed).valid;
}

vkutil::VkExpected<VulkanCommandArena::BorrowedCommandBuffer> VulkanCommandArena::acquire(
    const VulkanCommandArena::FrameToken& token,
    CommandBufferLevel level,
    uint32_t workerIndex,
    VkCommandBufferUsageFlags usage,
    const VkCommandBufferInheritanceInfo* inheritance,
    SecondaryRecordingMode secondaryMode)
{
    if (!token.valid()) {
        return vkutil::VkExpected<BorrowedCommandBuffer>(
            vkutil::makeError("VulkanCommandArena::acquire", VK_ERROR_INITIALIZATION_FAILED, "command_arena", "stale_token").context());
    }
    if (workerIndex >= workers_.size() || token.frameIndex >= framesInFlight_) {
        return vkutil::VkExpected<BorrowedCommandBuffer>(
            vkutil::makeError("VulkanCommandArena::acquire", VK_ERROR_INITIALIZATION_FAILED, "command_arena", "invalid_indices").context());
    }

    FrameState& frame = workers_[workerIndex][token.frameIndex];
    const uint64_t expectedEpoch = frameSync_[token.frameIndex].frameEpoch.load(std::memory_order_acquire);
    if (expectedEpoch == 0 || token.epoch != expectedEpoch) {
        return vkutil::VkExpected<BorrowedCommandBuffer>(
            vkutil::makeError("VulkanCommandArena::acquire", VK_ERROR_INITIALIZATION_FAILED, "command_arena", "stale_token").context());
    }

    std::lock_guard<std::mutex> lock(*frame.mutex);

    VkCommandBuffer cb = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer>& buffers = (level == CommandBufferLevel::Primary) ? frame.primaryBuffers : frame.secondaryBuffers;
    uint32_t& next = (level == CommandBufferLevel::Primary) ? frame.nextPrimary : frame.nextSecondary;

    if (next < buffers.size()) {
        cb = buffers[next++];
    } else {
        VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = frame.pool;
        ai.level = (level == CommandBufferLevel::Primary) ? VK_COMMAND_BUFFER_LEVEL_PRIMARY : VK_COMMAND_BUFFER_LEVEL_SECONDARY;
        ai.commandBufferCount = 1;
        const VkResult allocRes = vkAllocateCommandBuffers(device_, &ai, &cb);
        if (allocRes != VK_SUCCESS) {
            return vkutil::VkExpected<BorrowedCommandBuffer>(
                vkutil::checkResult(allocRes, "vkAllocateCommandBuffers", "command_arena").context());
        }
        buffers.push_back(cb);
        ++next;
    }

    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = usage;
    if (level == CommandBufferLevel::Secondary) {
        if (inheritance == nullptr) {
            return vkutil::VkExpected<BorrowedCommandBuffer>(
                vkutil::makeError("VulkanCommandArena::acquireSecondary", VK_ERROR_INITIALIZATION_FAILED, "command_arena", "missing_inheritance").context());
        }
        if (secondaryMode == SecondaryRecordingMode::LegacyRenderPass) {
            bi.flags |= VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
        }
        bi.pInheritanceInfo = inheritance;
    }

    const VkResult beginRes = vkBeginCommandBuffer(cb, &bi);
    if (beginRes != VK_SUCCESS) {
        return vkutil::VkExpected<BorrowedCommandBuffer>(
            vkutil::checkResult(beginRes, "vkBeginCommandBuffer", "command_arena").context());
    }

    return BorrowedCommandBuffer{
        .handle = cb,
        .workerIndex = workerIndex,
        .frameIndex = token.frameIndex,
        .generation = frame.generation->load(std::memory_order_acquire),
        .epoch = token.epoch,
        .level = level
    };
}

vkutil::VkExpected<VulkanCommandArena::BorrowedCommandBuffer> VulkanCommandArena::acquirePrimary(
    const VulkanCommandArena::FrameToken& token,
    uint32_t workerIndex,
    VkCommandBufferUsageFlags usage)
{
    return acquire(token, CommandBufferLevel::Primary, workerIndex, usage, nullptr, SecondaryRecordingMode::LegacyRenderPass);
}

vkutil::VkExpected<VulkanCommandArena::BorrowedCommandBuffer> VulkanCommandArena::acquireSecondary(
    const VulkanCommandArena::FrameToken& token,
    const VkCommandBufferInheritanceInfo& inheritance,
    uint32_t workerIndex,
    VkCommandBufferUsageFlags usage,
    SecondaryRecordingMode mode)
{
    return acquire(token, CommandBufferLevel::Secondary, workerIndex, usage, &inheritance, mode);
}

vkutil::VkExpected<VulkanCommandArena::CommandRecorder> VulkanCommandArena::acquireRecorderPrimary(
    const FrameToken& token,
    uint32_t workerIndex,
    VkCommandBufferUsageFlags usage)
{
    auto borrowed = acquirePrimary(token, workerIndex, usage);
    if (!borrowed.hasValue()) {
        return vkutil::VkExpected<CommandRecorder>(borrowed.context());
    }
    return CommandRecorder(this, borrowed.value());
}

vkutil::VkExpected<VulkanCommandArena::CommandRecorder> VulkanCommandArena::acquireRecorderSecondary(
    const FrameToken& token,
    const VkCommandBufferInheritanceInfo& inheritance,
    uint32_t workerIndex,
    VkCommandBufferUsageFlags usage,
    SecondaryRecordingMode mode)
{
    auto borrowed = acquireSecondary(token, inheritance, workerIndex, usage, mode);
    if (!borrowed.hasValue()) {
        return vkutil::VkExpected<CommandRecorder>(borrowed.context());
    }
    return CommandRecorder(this, borrowed.value());
}

vkutil::VkExpected<void> VulkanCommandArena::endBorrowed(const BorrowedCommandBuffer& borrowed) const
{
    const BorrowedValidation validation = validateBorrowed(borrowed);
    if (!validation.valid) {
        return vkutil::makeError("VulkanCommandArena::endBorrowed", VK_ERROR_INITIALIZATION_FAILED, "command_arena", validation.reason());
    }

    const VkResult endRes = vkEndCommandBuffer(borrowed.handle);
    if (endRes != VK_SUCCESS) {
        return vkutil::checkResult(endRes, "vkEndCommandBuffer", "command_arena");
    }

    return {};
}

VulkanCommandArena::~VulkanCommandArena() noexcept
{
#ifndef NDEBUG
    if (!waitForIdleOnDestroy_) {
        for (const AtomicFrameSyncState& frame : frameSync_) {
            if (!frame.signaled.load(std::memory_order_acquire) && frame.ticketValue.load(std::memory_order_acquire) != 0) {
                std::terminate();
            }
        }
    }
#endif

    if (waitForIdleOnDestroy_ && device_ != VK_NULL_HANDLE) {
        static_cast<void>(vkDeviceWaitIdle(device_));
    }

    for (auto& worker : workers_) {
        for (FrameState& frame : worker) {
            if (frame.pool != VK_NULL_HANDLE) {
                vkDestroyCommandPool(device_, frame.pool, nullptr);
                frame.pool = VK_NULL_HANDLE;
            }
            frame.primaryBuffers.clear();
            frame.secondaryBuffers.clear();
            frame.nextPrimary = 0;
            frame.nextSecondary = 0;
        }
    }
    device_ = VK_NULL_HANDLE;
    framesInFlight_ = 0;
    frameSync_.clear();
    frameTransitionMutexes_.clear();
}
