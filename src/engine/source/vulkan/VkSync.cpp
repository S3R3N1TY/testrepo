
#include "VkSync.h"
 #include "DeferredDeletionService.h"

namespace {

[[nodiscard]] VkPipelineStageFlags2 inferStageMaskForDependencyClass(SyncDependencyClass dependencyClass) noexcept
{
    switch (dependencyClass) {
    case SyncDependencyClass::Graphics:
        return VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT
            | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
            | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT
            | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT
            | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    case SyncDependencyClass::Compute:
        return VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    case SyncDependencyClass::Transfer:
        return VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    case SyncDependencyClass::Host:
        return VK_PIPELINE_STAGE_2_HOST_BIT;
    case SyncDependencyClass::Generic:
    default:
        return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    }
}

[[nodiscard]] vkutil::VkExpected<VkPipelineStageFlags2> resolveStageMask(
    VkPipelineStageFlags2 explicitMask,
    VkPipelineStageFlags2 contextDefaultMask,
    SyncDependencyClass dependencyClass,
    bool allowAllCommandsFallback,
    const char* context)
{
    if (explicitMask != 0) {
        return vkutil::VkExpected<VkPipelineStageFlags2>(explicitMask);
    }
    if (contextDefaultMask != 0) {
        return vkutil::VkExpected<VkPipelineStageFlags2>(contextDefaultMask);
    }

    const VkPipelineStageFlags2 inferred = inferStageMaskForDependencyClass(dependencyClass);
    if (dependencyClass == SyncDependencyClass::Generic && !allowAllCommandsFallback) {
        return vkutil::VkExpected<VkPipelineStageFlags2>(
            vkutil::makeError("SyncContext::submit", VK_ERROR_VALIDATION_FAILED_EXT, "sync", context).context());
    }
    return vkutil::VkExpected<VkPipelineStageFlags2>(inferred);
}

VkPipelineStageFlags mapStage2ToLegacySingleBit(VkPipelineStageFlags2 bit) noexcept
{
    switch (bit) {
    case VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT: return VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    case VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT: return VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
    case VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT: return VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
#ifdef VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT
    case VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT: return VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
#endif
#ifdef VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT
    case VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT: return VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
#endif
    case VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT: return VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
    case VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT: return VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT;
    case VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT: return VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT;
    case VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT: return VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT;
#ifdef VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT
    case VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT:
        return VK_PIPELINE_STAGE_VERTEX_SHADER_BIT
            | VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT
            | VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT
            | VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT;
#endif
    case VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT: return VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    case VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT: return VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    case VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT: return VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    case VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT: return VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    case VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT: return VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    case VK_PIPELINE_STAGE_2_TRANSFER_BIT: return VK_PIPELINE_STAGE_TRANSFER_BIT;
#ifdef VK_PIPELINE_STAGE_2_COPY_BIT
    case VK_PIPELINE_STAGE_2_COPY_BIT: return VK_PIPELINE_STAGE_TRANSFER_BIT;
#endif
#ifdef VK_PIPELINE_STAGE_2_RESOLVE_BIT
    case VK_PIPELINE_STAGE_2_RESOLVE_BIT: return VK_PIPELINE_STAGE_TRANSFER_BIT;
#endif
#ifdef VK_PIPELINE_STAGE_2_BLIT_BIT
    case VK_PIPELINE_STAGE_2_BLIT_BIT: return VK_PIPELINE_STAGE_TRANSFER_BIT;
#endif
#ifdef VK_PIPELINE_STAGE_2_CLEAR_BIT
    case VK_PIPELINE_STAGE_2_CLEAR_BIT: return VK_PIPELINE_STAGE_TRANSFER_BIT;
#endif
    case VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT: return VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    case VK_PIPELINE_STAGE_2_HOST_BIT: return VK_PIPELINE_STAGE_HOST_BIT;
    case VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT: return VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
    case VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT: return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    default: return 0;
    }
}

vkutil::VkExpected<VkPipelineStageFlags> sanitizeLegacyStageMask(VkPipelineStageFlags2 mask2, const char* context)
{
    if (mask2 == 0) {
        return vkutil::VkExpected<VkPipelineStageFlags>(
            vkutil::makeError("SyncContext::submit", VK_ERROR_VALIDATION_FAILED_EXT, "sync", context).context());
    }

    VkPipelineStageFlags mapped = 0;
    VkPipelineStageFlags2 remaining = mask2;

    for (uint32_t bitIndex = 0; bitIndex < 64; ++bitIndex) {
        const VkPipelineStageFlags2 bit = (VkPipelineStageFlags2{ 1 } << bitIndex);
        if ((remaining & bit) == 0) {
            continue;
        }

        const VkPipelineStageFlags legacyBit = mapStage2ToLegacySingleBit(bit);
        if (legacyBit == 0) {
            return vkutil::VkExpected<VkPipelineStageFlags>(
                vkutil::makeError("SyncContext::submit", VK_ERROR_VALIDATION_FAILED_EXT, "sync", context).context());
        }

        mapped |= legacyBit;
        remaining &= ~bit;
    }

    if (mapped == 0) {
        return vkutil::VkExpected<VkPipelineStageFlags>(
            vkutil::makeError("SyncContext::submit", VK_ERROR_VALIDATION_FAILED_EXT, "sync", context).context());
    }
    return vkutil::VkExpected<VkPipelineStageFlags>(mapped);
}

} // namespace

vkutil::VkExpected<VulkanSemaphore> VulkanSemaphore::createResult(VkDevice device, bool timeline)
{
    if (device == VK_NULL_HANDLE) {
        return vkutil::VkExpected<VulkanSemaphore>(
            vkutil::makeError("VulkanSemaphore::createResult", VK_ERROR_INITIALIZATION_FAILED, "sync").context());
    }

    VkSemaphoreTypeCreateInfo typeInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_BINARY;
    typeInfo.initialValue = 0;

    VkSemaphoreCreateInfo ci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    if (timeline) {
        typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        ci.pNext = &typeInfo;
    }

    VkSemaphore sem = VK_NULL_HANDLE;
    const VkResult res = vkCreateSemaphore(device, &ci, nullptr, &sem);
    if (res != VK_SUCCESS) {
        return vkutil::VkExpected<VulkanSemaphore>(
            vkutil::checkResult(res, "vkCreateSemaphore", "sync").context());
    }

    VulkanSemaphore out{};
    out.handle = DeferredDeletionService::instance().makeDeferredHandle<VkSemaphore, PFN_vkDestroySemaphore>(device, sem, vkDestroySemaphore);
    return std::move(out);
}

VulkanSemaphore::VulkanSemaphore(VkDevice device, bool timeline)
    : handle()
{
    auto created = createResult(device, timeline);
    if (!created.hasValue()) {
        vkutil::throwVkError("VulkanSemaphore::VulkanSemaphore", created.error());
    }
    *this = std::move(created.value());
}

vkutil::VkExpected<VulkanFence> VulkanFence::createResult(VkDevice device, VkFenceCreateFlags flags)
{
    if (device == VK_NULL_HANDLE) {
        return vkutil::VkExpected<VulkanFence>(
            vkutil::makeError("VulkanFence::createResult", VK_ERROR_INITIALIZATION_FAILED, "sync").context());
    }

    VkFenceCreateInfo ci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    ci.flags = flags;

    VkFence fence = VK_NULL_HANDLE;
    const VkResult res = vkCreateFence(device, &ci, nullptr, &fence);
    if (res != VK_SUCCESS) {
        return vkutil::VkExpected<VulkanFence>(
            vkutil::checkResult(res, "vkCreateFence", "sync").context());
    }

    VulkanFence out{};
    out.handle = DeferredDeletionService::instance().makeDeferredHandle<VkFence, PFN_vkDestroyFence>(device, fence, vkDestroyFence);
    return std::move(out);
}

VulkanFence::VulkanFence(VkDevice device, VkFenceCreateFlags flags)
    : handle()
{
    auto created = createResult(device, flags);
    if (!created.hasValue()) {
        vkutil::throwVkError("VulkanFence::VulkanFence", created.error());
    }
    *this = std::move(created.value());
}

vkutil::VkExpected<void> VulkanFence::resetResult()
{
    const VkFence f = handle.get();
    if (f == VK_NULL_HANDLE) {
        return {};
    }

    const VkResult res = vkResetFences(handle.getDevice(), 1, &f);
    if (res != VK_SUCCESS) {
        return vkutil::checkResult(res, "vkResetFences", "sync");
    }
    return {};
}

vkutil::VkExpected<bool> VulkanFence::waitResult(uint64_t timeout)
{
    const VkFence f = handle.get();
    if (f == VK_NULL_HANDLE) {
        return vkutil::VkExpected<bool>(true);
    }

    const VkResult res = vkWaitForFences(handle.getDevice(), 1, &f, VK_TRUE, timeout);
    if (res == VK_SUCCESS) {
        return vkutil::VkExpected<bool>(true);
    }
    if (res == VK_TIMEOUT) {
        return vkutil::VkExpected<bool>(false);
    }

    return vkutil::VkExpected<bool>(vkutil::checkResult(res, "vkWaitForFences", "sync").context());
}

void VulkanFence::reset()
{
    auto res = resetResult();
    if (!res.hasValue()) {
        vkutil::throwVkError("VulkanFence::reset", res.error());
    }
}

VkResult VulkanFence::wait(uint64_t timeout)
{
    auto res = waitResult(timeout);
    if (!res.hasValue()) {
        vkutil::throwVkError("VulkanFence::wait", res.error());
    }
    return res.value() ? VK_SUCCESS : VK_TIMEOUT;
}

vkutil::VkExpected<TimelineSemaphore> TimelineSemaphore::createResult(VkDevice device, uint64_t initialValue)
{
    if (device == VK_NULL_HANDLE) {
        return vkutil::VkExpected<TimelineSemaphore>(
            vkutil::makeError("TimelineSemaphore::createResult", VK_ERROR_INITIALIZATION_FAILED, "sync").context());
    }

    VkSemaphoreTypeCreateInfo typeInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    typeInfo.initialValue = initialValue;

    VkSemaphoreCreateInfo ci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    ci.pNext = &typeInfo;

    VkSemaphore sem = VK_NULL_HANDLE;
    const VkResult res = vkCreateSemaphore(device, &ci, nullptr, &sem);
    if (res != VK_SUCCESS) {
        return vkutil::VkExpected<TimelineSemaphore>(
            vkutil::checkResult(res, "vkCreateSemaphore", "sync").context());
    }

    TimelineSemaphore out{};
    out.handle = DeferredDeletionService::instance().makeDeferredHandle<VkSemaphore, PFN_vkDestroySemaphore>(device, sem, vkDestroySemaphore);
    return std::move(out);
}

TimelineSemaphore::TimelineSemaphore(VkDevice device, uint64_t initialValue)
    : handle()
{
    auto created = createResult(device, initialValue);
    if (!created.hasValue()) {
        vkutil::throwVkError("TimelineSemaphore::TimelineSemaphore", created.error());
    }
    *this = std::move(created.value());
}

vkutil::VkExpected<uint64_t> TimelineSemaphore::value() const
{
    if (!valid()) {
        return vkutil::VkExpected<uint64_t>(vkutil::makeError("TimelineSemaphore::value", VK_ERROR_INITIALIZATION_FAILED, "sync").context());
    }
    uint64_t value = 0;
    const VkResult res = vkGetSemaphoreCounterValue(handle.getDevice(), handle.get(), &value);
    if (res != VK_SUCCESS) {
        return vkutil::VkExpected<uint64_t>(vkutil::checkResult(res, "vkGetSemaphoreCounterValue", "sync").context());
    }
    return vkutil::VkExpected<uint64_t>(value);
}

vkutil::VkExpected<void> TimelineSemaphore::signal(uint64_t value)
{
    if (!valid()) {
        return vkutil::makeError("TimelineSemaphore::signal", VK_ERROR_INITIALIZATION_FAILED, "sync");
    }

    VkSemaphoreSignalInfo info{ VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO };
    info.semaphore = handle.get();
    info.value = value;

    const VkResult res = vkSignalSemaphore(handle.getDevice(), &info);
    if (res != VK_SUCCESS) {
        return vkutil::checkResult(res, "vkSignalSemaphore", "sync");
    }
    return {};
}

vkutil::VkExpected<void> TimelineSemaphore::wait(uint64_t value, uint64_t timeout) const
{
    if (!valid()) {
        return vkutil::makeError("TimelineSemaphore::wait", VK_ERROR_INITIALIZATION_FAILED, "sync");
    }

    const VkSemaphore sem = handle.get();
    VkSemaphoreWaitInfo wi{ VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
    wi.semaphoreCount = 1;
    wi.pSemaphores = &sem;
    wi.pValues = &value;

    const VkResult res = vkWaitSemaphores(handle.getDevice(), &wi, timeout);
    if (res == VK_SUCCESS) {
        return {};
    }
    return vkutil::checkResult(res, "vkWaitSemaphores", "sync");
}

SyncContext::SyncContext(VkDevice device,
    uint32_t framesInFlight,
    bool timelineSupported,
    bool synchronization2Enabled,
    VkPipelineStageFlags2 defaultTimelineWaitStage,
    VkPipelineStageFlags2 defaultTimelineSignalStage,
    VkPipelineStageFlags2 defaultExternalSignalStage)
{
    const auto initRes = init(device,
        framesInFlight,
        timelineSupported,
        synchronization2Enabled,
        defaultTimelineWaitStage,
        defaultTimelineSignalStage,
        defaultExternalSignalStage);
    if (!initRes.hasValue()) {
        vkutil::throwVkError("SyncContext::SyncContext", initRes.error());
    }
}

SyncContext::SyncContext(SyncContext&& other) noexcept
{
    std::unique_lock<std::shared_mutex> otherLock(other.stateMutex_);

    device_ = other.device_;
    framesInFlight_.store(other.framesInFlight_.load(std::memory_order_acquire), std::memory_order_release);
    timelineMode_.store(other.timelineMode_.load(std::memory_order_acquire), std::memory_order_release);
    synchronization2Enabled_.store(other.synchronization2Enabled_.load(std::memory_order_acquire), std::memory_order_release);
    submitBackend_.store(other.submitBackend_.load(std::memory_order_acquire), std::memory_order_release);

    timeline_ = std::move(other.timeline_);
    nextTimelineValue_.store(other.nextTimelineValue_.load(std::memory_order_acquire), std::memory_order_release);
    timelineFrameValues_ = std::move(other.timelineFrameValues_);

    frameFences_ = std::move(other.frameFences_);
    frameSubmittedValues_ = std::move(other.frameSubmittedValues_);
    frameCompletedValues_ = std::move(other.frameCompletedValues_);

    defaultTimelineWaitStage_.store(other.defaultTimelineWaitStage_.load(std::memory_order_acquire), std::memory_order_release);
    defaultTimelineSignalStage_.store(other.defaultTimelineSignalStage_.load(std::memory_order_acquire), std::memory_order_release);
    defaultExternalSignalStage_.store(other.defaultExternalSignalStage_.load(std::memory_order_acquire), std::memory_order_release);

    other.device_ = VK_NULL_HANDLE;
    other.framesInFlight_.store(0, std::memory_order_release);
    other.timelineMode_.store(false, std::memory_order_release);
    other.synchronization2Enabled_.store(false, std::memory_order_release);
    other.submitBackend_.store(SubmitBackend::LegacySubmit, std::memory_order_release);
    other.nextTimelineValue_.store(1, std::memory_order_release);
    other.defaultTimelineWaitStage_.store(0, std::memory_order_release);
    other.defaultTimelineSignalStage_.store(0, std::memory_order_release);
    other.defaultExternalSignalStage_.store(0, std::memory_order_release);
}

SyncContext& SyncContext::operator=(SyncContext&& other) noexcept
{
    if (this == &other) {
        return *this;
    }

    std::scoped_lock lock(stateMutex_, other.stateMutex_);

    device_ = other.device_;
    framesInFlight_.store(other.framesInFlight_.load(std::memory_order_acquire), std::memory_order_release);
    timelineMode_.store(other.timelineMode_.load(std::memory_order_acquire), std::memory_order_release);
    synchronization2Enabled_.store(other.synchronization2Enabled_.load(std::memory_order_acquire), std::memory_order_release);
    submitBackend_.store(other.submitBackend_.load(std::memory_order_acquire), std::memory_order_release);

    timeline_ = std::move(other.timeline_);
    nextTimelineValue_.store(other.nextTimelineValue_.load(std::memory_order_acquire), std::memory_order_release);
    timelineFrameValues_ = std::move(other.timelineFrameValues_);

    frameFences_ = std::move(other.frameFences_);
    frameSubmittedValues_ = std::move(other.frameSubmittedValues_);
    frameCompletedValues_ = std::move(other.frameCompletedValues_);

    defaultTimelineWaitStage_.store(other.defaultTimelineWaitStage_.load(std::memory_order_acquire), std::memory_order_release);
    defaultTimelineSignalStage_.store(other.defaultTimelineSignalStage_.load(std::memory_order_acquire), std::memory_order_release);
    defaultExternalSignalStage_.store(other.defaultExternalSignalStage_.load(std::memory_order_acquire), std::memory_order_release);

    other.device_ = VK_NULL_HANDLE;
    other.framesInFlight_.store(0, std::memory_order_release);
    other.timelineMode_.store(false, std::memory_order_release);
    other.synchronization2Enabled_.store(false, std::memory_order_release);
    other.submitBackend_.store(SubmitBackend::LegacySubmit, std::memory_order_release);
    other.nextTimelineValue_.store(1, std::memory_order_release);
    other.defaultTimelineWaitStage_.store(0, std::memory_order_release);
    other.defaultTimelineSignalStage_.store(0, std::memory_order_release);
    other.defaultExternalSignalStage_.store(0, std::memory_order_release);

    return *this;
}

vkutil::VkExpected<SyncContext> SyncContext::createResult(VkDevice device,
    uint32_t framesInFlight,
    bool timelineSupported,
    bool synchronization2Enabled,
    VkPipelineStageFlags2 defaultTimelineWaitStage,
    VkPipelineStageFlags2 defaultTimelineSignalStage,
    VkPipelineStageFlags2 defaultExternalSignalStage)
{
    SyncContext out{};
    const auto initRes = out.init(device,
        framesInFlight,
        timelineSupported,
        synchronization2Enabled,
        defaultTimelineWaitStage,
        defaultTimelineSignalStage,
        defaultExternalSignalStage);
    if (!initRes.hasValue()) {
        return vkutil::VkExpected<SyncContext>(initRes.context());
    }
    return std::move(out);
}


std::vector<std::shared_ptr<std::atomic<uint64_t>>> SyncContext::makeAtomicFrameValues(uint32_t framesInFlight, uint64_t initialValue)
{
    std::vector<std::shared_ptr<std::atomic<uint64_t>>> values;
    values.reserve(framesInFlight);
    for (uint32_t i = 0; i < framesInFlight; ++i) {
        values.push_back(std::make_shared<std::atomic<uint64_t>>(initialValue));
    }
    return values;
}

vkutil::VkExpected<void> SyncContext::init(VkDevice device,
    uint32_t framesInFlight,
    bool timelineSupported,
    bool synchronization2Enabled,
    VkPipelineStageFlags2 defaultTimelineWaitStage,
    VkPipelineStageFlags2 defaultTimelineSignalStage,
    VkPipelineStageFlags2 defaultExternalSignalStage)
{
    std::unique_lock<std::shared_mutex> stateLock(stateMutex_);
    device_ = device;
    framesInFlight_.store(framesInFlight, std::memory_order_release);
    timelineMode_.store(timelineSupported, std::memory_order_release);
    synchronization2Enabled_.store(synchronization2Enabled, std::memory_order_release);
    submitBackend_.store(synchronization2Enabled ? SubmitBackend::Submit2 : SubmitBackend::LegacySubmit, std::memory_order_release);
    defaultTimelineWaitStage_.store(
        (defaultTimelineWaitStage == 0) ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : defaultTimelineWaitStage,
        std::memory_order_release);
    defaultTimelineSignalStage_.store(
        (defaultTimelineSignalStage == 0) ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : defaultTimelineSignalStage,
        std::memory_order_release);
    defaultExternalSignalStage_.store(
        (defaultExternalSignalStage == 0) ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : defaultExternalSignalStage,
        std::memory_order_release);
    nextTimelineValue_.store(1, std::memory_order_release);

    if (device_ == VK_NULL_HANDLE || framesInFlight == 0) {
        return vkutil::makeError("SyncContext::init", VK_ERROR_INITIALIZATION_FAILED, "sync");
    }

    if (timelineSupported) {
        auto timelineRes = TimelineSemaphore::createResult(device_, 0);
        if (!timelineRes.hasValue()) {
            return vkutil::VkExpected<void>(timelineRes.context());
        }
        timeline_ = std::move(timelineRes.value());
        timelineFrameValues_ = makeAtomicFrameValues(framesInFlight, 0);
        frameFences_.clear();
        frameSubmittedValues_.clear();
        frameCompletedValues_.clear();
    } else {
        timeline_ = TimelineSemaphore{};
        timelineFrameValues_.clear();

        frameFences_.clear();
        frameFences_.reserve(framesInFlight);
        frameSubmittedValues_ = makeAtomicFrameValues(framesInFlight, 0);
        frameCompletedValues_ = makeAtomicFrameValues(framesInFlight, 0);

        for (uint32_t i = 0; i < framesInFlight; ++i) {
            auto fenceRes = VulkanFence::createResult(device_, VK_FENCE_CREATE_SIGNALED_BIT);
            if (!fenceRes.hasValue()) {
                return vkutil::VkExpected<void>(fenceRes.context());
            }
            frameFences_.push_back(std::move(fenceRes.value()));
        }
    }

    return {};
}

vkutil::VkExpected<SyncTicket> SyncContext::submit(const VulkanQueue& queue,
    uint32_t frameIndex,
    const SyncSubmitInfo& submitInfo,
    VkFence explicitFence,
    SubmitFrameSyncPolicy frameSyncPolicy)
{
    bool timelineMode = false;
    SubmitBackend backend = SubmitBackend::LegacySubmit;
    VkPipelineStageFlags2 defaultTimelineWaitStage = 0;
    VkPipelineStageFlags2 defaultTimelineSignalStage = 0;
    VkPipelineStageFlags2 defaultExternalSignalStage = 0;
    VkSemaphore timelineSemaphore = VK_NULL_HANDLE;

    {
        std::shared_lock<std::shared_mutex> stateLock(stateMutex_);
        if (frameIndex >= framesInFlight_.load(std::memory_order_acquire)) {
            return vkutil::VkExpected<SyncTicket>(vkutil::makeError("SyncContext::submit", VK_ERROR_INITIALIZATION_FAILED, "sync").context());
        }
        timelineMode = timelineMode_.load(std::memory_order_acquire);
        backend = submitBackend_.load(std::memory_order_acquire);
        defaultTimelineWaitStage = defaultTimelineWaitStage_.load(std::memory_order_acquire);
        defaultTimelineSignalStage = defaultTimelineSignalStage_.load(std::memory_order_acquire);
        defaultExternalSignalStage = defaultExternalSignalStage_.load(std::memory_order_acquire);
        timelineSemaphore = timeline_.get();
    }

    if (submitInfo.commandBuffers.empty()) {
        return vkutil::VkExpected<SyncTicket>(vkutil::makeError("SyncContext::submit", VK_ERROR_INITIALIZATION_FAILED, "sync").context());
    }
    if (!submitInfo.externalWaitStages.empty() && submitInfo.externalWaitSemaphores.size() != submitInfo.externalWaitStages.size()) {
        return vkutil::VkExpected<SyncTicket>(vkutil::makeError("SyncContext::submit", VK_ERROR_INITIALIZATION_FAILED, "sync", "external_wait_stage_count_mismatch").context());
    }
    if (!submitInfo.externalWaitDependencies.empty() && submitInfo.externalWaitSemaphores.size() != submitInfo.externalWaitDependencies.size()) {
        return vkutil::VkExpected<SyncTicket>(vkutil::makeError("SyncContext::submit", VK_ERROR_INITIALIZATION_FAILED, "sync", "external_wait_dependency_count_mismatch").context());
    }
    if (!timelineMode && !submitInfo.waitTickets.empty()) {
        return vkutil::VkExpected<SyncTicket>(vkutil::makeError("SyncContext::submit", VK_ERROR_VALIDATION_FAILED_EXT, "sync", "fallback_mode_disallows_wait_tickets").context());
    }

    std::vector<VkSemaphoreSubmitInfo> waitInfos;
    std::vector<VkCommandBufferSubmitInfo> cmdInfos;
    std::vector<VkSemaphoreSubmitInfo> signalInfos;

    waitInfos.reserve(submitInfo.externalWaitSemaphores.size() + submitInfo.waitTickets.size());
    cmdInfos.reserve(submitInfo.commandBuffers.size());
    signalInfos.reserve(submitInfo.externalSignalSemaphores.size() + 1);

    for (size_t i = 0; i < submitInfo.externalWaitSemaphores.size(); ++i) {
        const VkPipelineStageFlags2 explicitWaitStage = submitInfo.externalWaitStages.empty() ? 0 : submitInfo.externalWaitStages[i];
        const SyncDependencyClass waitDependency = submitInfo.externalWaitDependencies.empty()
            ? SyncDependencyClass::Graphics
            : submitInfo.externalWaitDependencies[i];
        const auto waitStageRes = resolveStageMask(
            explicitWaitStage,
            0,
            waitDependency,
            submitInfo.allowAllCommandsFallback,
            "external_wait_generic_dependency_requires_explicit_stage_mask");
        if (!waitStageRes.hasValue()) {
            return vkutil::VkExpected<SyncTicket>(waitStageRes.context());
        }

        VkSemaphoreSubmitInfo si{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
        si.semaphore = submitInfo.externalWaitSemaphores[i];
        si.value = 0;
        si.stageMask = waitStageRes.value();
        waitInfos.push_back(si);
    }

    if (timelineMode) {
        const auto timelineWaitStageRes = resolveStageMask(
            submitInfo.timelineWaitStageMask,
            defaultTimelineWaitStage,
            submitInfo.timelineWaitDependency,
            submitInfo.allowAllCommandsFallback,
            "timeline_wait_generic_dependency_requires_explicit_stage_mask");
        if (!timelineWaitStageRes.hasValue()) {
            return vkutil::VkExpected<SyncTicket>(timelineWaitStageRes.context());
        }

        for (const SyncTicket& ticket : submitInfo.waitTickets) {
            VkSemaphoreSubmitInfo tsi{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
            tsi.semaphore = timelineSemaphore;
            tsi.value = ticket.value;
            tsi.stageMask = timelineWaitStageRes.value();
            waitInfos.push_back(tsi);
        }
    }

    for (VkCommandBuffer cb : submitInfo.commandBuffers) {
        VkCommandBufferSubmitInfo cbi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
        cbi.commandBuffer = cb;
        cmdInfos.push_back(cbi);
    }

    const auto externalSignalStageRes = resolveStageMask(
        submitInfo.externalSignalStageMask,
        defaultExternalSignalStage,
        submitInfo.externalSignalDependency,
        submitInfo.allowAllCommandsFallback,
        "external_signal_generic_dependency_requires_explicit_stage_mask");
    if (!externalSignalStageRes.hasValue()) {
        return vkutil::VkExpected<SyncTicket>(externalSignalStageRes.context());
    }

    for (VkSemaphore sem : submitInfo.externalSignalSemaphores) {
        VkSemaphoreSubmitInfo ssi{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
        ssi.semaphore = sem;
        ssi.value = 0;
        ssi.stageMask = externalSignalStageRes.value();
        signalInfos.push_back(ssi);
    }

    SyncTicket outTicket{};
    outTicket.frameIndex = frameIndex;

    VkFence fence = explicitFence;
    if (!timelineMode) {
        if (explicitFence != VK_NULL_HANDLE) {
            return vkutil::VkExpected<SyncTicket>(vkutil::makeError("SyncContext::submit", VK_ERROR_VALIDATION_FAILED_EXT, "sync", "fallback_mode_requires_internal_fence").context());
        }
        const auto prepareRes = prepareFrameForSubmit(frameIndex, frameSyncPolicy);
        if (!prepareRes.hasValue()) {
            return vkutil::VkExpected<SyncTicket>(prepareRes.context());
        }
        if (fence == VK_NULL_HANDLE) {
            std::shared_lock<std::shared_mutex> stateLock(stateMutex_);
            fence = frameFences_[frameIndex].get();
        }
    } else {
        {
            std::shared_lock<std::shared_mutex> stateLock(stateMutex_);
            outTicket.value = nextTimelineValue_.fetch_add(1, std::memory_order_acq_rel);
            timelineFrameValues_[frameIndex]->store(outTicket.value, std::memory_order_release);
        }
        const auto timelineSignalStageRes = resolveStageMask(
            submitInfo.timelineSignalStageMask,
            defaultTimelineSignalStage,
            submitInfo.timelineSignalDependency,
            submitInfo.allowAllCommandsFallback,
            "timeline_signal_generic_dependency_requires_explicit_stage_mask");
        if (!timelineSignalStageRes.hasValue()) {
            return vkutil::VkExpected<SyncTicket>(timelineSignalStageRes.context());
        }

        VkSemaphoreSubmitInfo timelineSignal{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
        timelineSignal.semaphore = timelineSemaphore;
        timelineSignal.value = outTicket.value;
        timelineSignal.stageMask = timelineSignalStageRes.value();
        signalInfos.push_back(timelineSignal);
    }

    vkutil::VkExpected<void> submitRes{};
    if (backend == SubmitBackend::Submit2) {
        VkSubmitInfo2 submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
        submit.waitSemaphoreInfoCount = static_cast<uint32_t>(waitInfos.size());
        submit.pWaitSemaphoreInfos = waitInfos.empty() ? nullptr : waitInfos.data();
        submit.commandBufferInfoCount = static_cast<uint32_t>(cmdInfos.size());
        submit.pCommandBufferInfos = cmdInfos.data();
        submit.signalSemaphoreInfoCount = static_cast<uint32_t>(signalInfos.size());
        submit.pSignalSemaphoreInfos = signalInfos.empty() ? nullptr : signalInfos.data();
        submitRes = queue.submit2({ submit }, fence, "sync_context");
    } else {
        std::vector<VkSemaphore> waitSemaphores;
        std::vector<VkPipelineStageFlags> waitStages;
        std::vector<VkCommandBuffer> commandBuffers;
        std::vector<VkSemaphore> signalSemaphores;
        std::vector<uint64_t> waitValues;
        std::vector<uint64_t> signalValues;

        waitSemaphores.reserve(waitInfos.size());
        waitStages.reserve(waitInfos.size());
        signalSemaphores.reserve(signalInfos.size());
        waitValues.reserve(waitInfos.size());
        signalValues.reserve(signalInfos.size());

        for (const VkSemaphoreSubmitInfo& waitInfo : waitInfos) {
            const auto stageRes = sanitizeLegacyStageMask(waitInfo.stageMask, "legacy_wait_stage_mask");
            if (!stageRes.hasValue()) {
                return vkutil::VkExpected<SyncTicket>(stageRes.context());
            }
            waitSemaphores.push_back(waitInfo.semaphore);
            waitStages.push_back(stageRes.value());
            waitValues.push_back(waitInfo.value);
        }

        commandBuffers = submitInfo.commandBuffers;
        for (const VkSemaphoreSubmitInfo& signalInfo : signalInfos) {
            signalSemaphores.push_back(signalInfo.semaphore);
            signalValues.push_back(signalInfo.value);
        }

        VkTimelineSemaphoreSubmitInfo timelineInfo{ VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
        if (timelineMode) {
            timelineInfo.waitSemaphoreValueCount = static_cast<uint32_t>(waitValues.size());
            timelineInfo.pWaitSemaphoreValues = waitValues.empty() ? nullptr : waitValues.data();
            timelineInfo.signalSemaphoreValueCount = static_cast<uint32_t>(signalValues.size());
            timelineInfo.pSignalSemaphoreValues = signalValues.empty() ? nullptr : signalValues.data();
        }

        VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.pNext = timelineMode ? &timelineInfo : nullptr;
        submit.waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.size());
        submit.pWaitSemaphores = waitSemaphores.empty() ? nullptr : waitSemaphores.data();
        submit.pWaitDstStageMask = waitStages.empty() ? nullptr : waitStages.data();
        submit.commandBufferCount = static_cast<uint32_t>(commandBuffers.size());
        submit.pCommandBuffers = commandBuffers.data();
        submit.signalSemaphoreCount = static_cast<uint32_t>(signalSemaphores.size());
        submit.pSignalSemaphores = signalSemaphores.empty() ? nullptr : signalSemaphores.data();

        submitRes = queue.submit({ submit }, fence, "sync_context");
    }
    if (!submitRes.hasValue()) {
        return vkutil::VkExpected<SyncTicket>(submitRes.context());
    }

    if (!timelineMode) {
        std::shared_lock<std::shared_mutex> stateLock(stateMutex_);
        outTicket.value = frameSubmittedValues_[frameIndex]->fetch_add(1, std::memory_order_acq_rel) + 1;
    } else {
        DeferredDeletionService::instance().updateSubmittedTicket(device_, DeferredDeletionService::SubmissionTicket{
            .value = outTicket.value,
            .queueClass = DeferredDeletionService::QueueClass::Generic,
            .queueFamilyIndex = queue.familyIndex() });
    }

    return vkutil::VkExpected<SyncTicket>(outTicket);
}

vkutil::VkExpected<bool> SyncContext::isTicketComplete(const SyncTicket& ticket) const
{
    VkDevice device = VK_NULL_HANDLE;
    bool timelineMode = false;
    VkFence frameFence = VK_NULL_HANDLE;
    uint64_t completedValue = 0;

    {
        std::shared_lock<std::shared_mutex> stateLock(stateMutex_);
        device = device_;
        timelineMode = timelineMode_.load(std::memory_order_acquire);
        if (!timelineMode) {
            if (ticket.frameIndex >= frameFences_.size()) {
                return vkutil::VkExpected<bool>(vkutil::makeError("SyncContext::isFrameComplete", VK_ERROR_INITIALIZATION_FAILED, "sync").context());
            }
            frameFence = frameFences_[ticket.frameIndex].get();
            completedValue = frameCompletedValues_[ticket.frameIndex]->load(std::memory_order_acquire);
        }
    }

    if (timelineMode) {
        const auto valueRes = timeline_.value();
        if (!valueRes.hasValue()) {
            return vkutil::VkExpected<bool>(valueRes.context());
        }
        static_cast<void>(DeferredDeletionService::instance().collect(device, valueRes.value(), ticket.frameIndex));
        return vkutil::VkExpected<bool>(valueRes.value() >= ticket.value);
    }

    const VkResult status = vkGetFenceStatus(device, frameFence);
    if (status == VK_SUCCESS) {
        std::shared_lock<std::shared_mutex> stateLock(stateMutex_);
        frameCompletedValues_[ticket.frameIndex]->store(frameSubmittedValues_[ticket.frameIndex]->load(std::memory_order_acquire), std::memory_order_release);
        return vkutil::VkExpected<bool>(ticket.value <= frameCompletedValues_[ticket.frameIndex]->load(std::memory_order_acquire));
    }
    if (status == VK_NOT_READY) {
        return vkutil::VkExpected<bool>(ticket.value <= completedValue);
    }
    return vkutil::VkExpected<bool>(vkutil::checkResult(status, "vkGetFenceStatus", "sync").context());
}

vkutil::VkExpected<bool> SyncContext::isFrameComplete(uint32_t frameIndex) const
{
    VkDevice device = VK_NULL_HANDLE;
    bool timelineMode = false;
    uint64_t frameValue = 0;
    VkFence frameFence = VK_NULL_HANDLE;

    {
        std::shared_lock<std::shared_mutex> stateLock(stateMutex_);
        if (frameIndex >= framesInFlight_.load(std::memory_order_acquire)) {
            return vkutil::VkExpected<bool>(vkutil::makeError("SyncContext::isFrameComplete", VK_ERROR_INITIALIZATION_FAILED, "sync").context());
        }
        device = device_;
        timelineMode = timelineMode_.load(std::memory_order_acquire);
        if (timelineMode) {
            frameValue = timelineFrameValues_[frameIndex]->load(std::memory_order_acquire);
        }
        else {
            frameFence = frameFences_[frameIndex].get();
        }
    }

    if (timelineMode) {
        if (frameValue == 0) {
            return vkutil::VkExpected<bool>(true);
        }
        return isTicketComplete(SyncTicket{ .value = frameValue, .frameIndex = frameIndex });
    }

    const VkResult status = vkGetFenceStatus(device, frameFence);
    if (status == VK_SUCCESS) {
        std::shared_lock<std::shared_mutex> stateLock(stateMutex_);
        frameCompletedValues_[frameIndex]->store(frameSubmittedValues_[frameIndex]->load(std::memory_order_acquire), std::memory_order_release);
        return vkutil::VkExpected<bool>(true);
    }
    if (status == VK_NOT_READY) {
        return vkutil::VkExpected<bool>(false);
    }
    return vkutil::VkExpected<bool>(vkutil::checkResult(status, "vkGetFenceStatus", "sync").context());
}

vkutil::VkExpected<bool> SyncContext::waitTicket(const SyncTicket& ticket, uint64_t timeout) const
{
    VkDevice device = VK_NULL_HANDLE;
    bool timelineMode = false;
    VkFence frameFence = VK_NULL_HANDLE;
    uint64_t completedValue = 0;

    {
        std::shared_lock<std::shared_mutex> stateLock(stateMutex_);
        device = device_;
        timelineMode = timelineMode_.load(std::memory_order_acquire);
        if (!timelineMode) {
            if (ticket.frameIndex >= frameFences_.size()) {
                return vkutil::VkExpected<bool>(
                    vkutil::makeError("SyncContext::waitTicket", VK_ERROR_INITIALIZATION_FAILED, "sync").context());
            }
            frameFence = frameFences_[ticket.frameIndex].get();
            completedValue = frameCompletedValues_[ticket.frameIndex]->load(std::memory_order_acquire);
        }
    }

    if (timelineMode) {
        const auto waitRes = timeline_.wait(ticket.value, timeout);
        if (!waitRes.hasValue()) {
            if (waitRes.error() == VK_TIMEOUT) {
                return vkutil::VkExpected<bool>(false);
            }
            return vkutil::VkExpected<bool>(waitRes.context());
        }
        static_cast<void>(DeferredDeletionService::instance().collect(device, ticket.value, ticket.frameIndex));
        return vkutil::VkExpected<bool>(true);
    }

    if (ticket.value <= completedValue) {
        return vkutil::VkExpected<bool>(true);
    }

    const VkResult waited = vkWaitForFences(device, 1u, &frameFence, VK_TRUE, timeout);
    if (waited != VK_SUCCESS && waited != VK_TIMEOUT) {
        return vkutil::VkExpected<bool>(vkutil::checkResult(waited, "vkWaitForFences", "sync").context());
    }
    if (waited == VK_TIMEOUT) {
        return vkutil::VkExpected<bool>(false);
    }

    {
        std::shared_lock<std::shared_mutex> stateLock(stateMutex_);
        frameCompletedValues_[ticket.frameIndex]->store(frameSubmittedValues_[ticket.frameIndex]->load(std::memory_order_acquire), std::memory_order_release);
        return vkutil::VkExpected<bool>(ticket.value <= frameCompletedValues_[ticket.frameIndex]->load(std::memory_order_acquire));
    }
}

vkutil::VkExpected<bool> SyncContext::waitFrame(uint32_t frameIndex, uint64_t timeout)
{
    VkDevice device = VK_NULL_HANDLE;
    bool timelineMode = false;
    uint64_t frameValue = 0;
    VkFence frameFence = VK_NULL_HANDLE;

    {
        std::shared_lock<std::shared_mutex> stateLock(stateMutex_);
        if (frameIndex >= framesInFlight_.load(std::memory_order_acquire)) {
            return vkutil::VkExpected<bool>(
                vkutil::makeError("SyncContext::waitFrame", VK_ERROR_INITIALIZATION_FAILED, "sync").context());
        }
        device = device_;
        timelineMode = timelineMode_.load(std::memory_order_acquire);
        if (timelineMode) {
            frameValue = timelineFrameValues_[frameIndex]->load(std::memory_order_acquire);
        }
        else {
            frameFence = frameFences_[frameIndex].get();
        }
    }

    if (timelineMode) {
        if (frameValue == 0) {
            return vkutil::VkExpected<bool>(true);
        }
        const auto waitRes = timeline_.wait(frameValue, timeout);
        if (!waitRes.hasValue()) {
            if (waitRes.error() == VK_TIMEOUT) {
                return vkutil::VkExpected<bool>(false);
            }
            return vkutil::VkExpected<bool>(waitRes.context());
        }
        static_cast<void>(DeferredDeletionService::instance().collect(device, frameValue, frameIndex));
        return vkutil::VkExpected<bool>(true);
    }

    const VkResult waitRes = vkWaitForFences(device, 1u, &frameFence, VK_TRUE, timeout);
    if (waitRes == VK_TIMEOUT) {
        return vkutil::VkExpected<bool>(false);
    }
    if (waitRes != VK_SUCCESS) {
        return vkutil::VkExpected<bool>(vkutil::checkResult(waitRes, "vkWaitForFences", "sync").context());
    }

    std::shared_lock<std::shared_mutex> stateLock(stateMutex_);
    frameCompletedValues_[frameIndex]->store(frameSubmittedValues_[frameIndex]->load(std::memory_order_acquire), std::memory_order_release);
    return vkutil::VkExpected<bool>(true);
}

vkutil::VkExpected<bool> SyncContext::pollFenceComplete(uint32_t frameIndex) const
{
    VkDevice device = VK_NULL_HANDLE;
    bool timelineMode = false;
    VkFence frameFence = VK_NULL_HANDLE;

    {
        std::shared_lock<std::shared_mutex> stateLock(stateMutex_);
        if (timelineMode_.load(std::memory_order_acquire)) {
            return vkutil::VkExpected<bool>(true);
        }
        if (frameIndex >= framesInFlight_.load(std::memory_order_acquire)) {
            return vkutil::VkExpected<bool>(vkutil::makeError("SyncContext::pollFenceComplete", VK_ERROR_INITIALIZATION_FAILED, "sync").context());
        }
        device = device_;
        timelineMode = timelineMode_.load(std::memory_order_acquire);
        frameFence = frameFences_[frameIndex].get();
    }

    if (timelineMode) {
        return vkutil::VkExpected<bool>(true);
    }

    const VkResult status = vkGetFenceStatus(device, frameFence);
    if (status == VK_SUCCESS) {
        std::shared_lock<std::shared_mutex> stateLock(stateMutex_);
        frameCompletedValues_[frameIndex]->store(frameSubmittedValues_[frameIndex]->load(std::memory_order_acquire), std::memory_order_release);
        return vkutil::VkExpected<bool>(true);
    }
    if (status == VK_NOT_READY) {
        return vkutil::VkExpected<bool>(false);
    }
    return vkutil::VkExpected<bool>(vkutil::checkResult(status, "vkGetFenceStatus", "sync").context());
}

vkutil::VkExpected<bool> SyncContext::waitFence(uint32_t frameIndex, uint64_t timeout)
{
    VkDevice device = VK_NULL_HANDLE;
    VkFence frameFence = VK_NULL_HANDLE;

    {
        std::shared_lock<std::shared_mutex> stateLock(stateMutex_);
        if (timelineMode_.load(std::memory_order_acquire)) {
            return vkutil::VkExpected<bool>(true);
        }
        if (frameIndex >= framesInFlight_.load(std::memory_order_acquire)) {
            return vkutil::VkExpected<bool>(vkutil::makeError("SyncContext::waitFence", VK_ERROR_INITIALIZATION_FAILED, "sync").context());
        }
        device = device_;
        frameFence = frameFences_[frameIndex].get();
    }

    const VkResult waitRes = vkWaitForFences(device, 1u, &frameFence, VK_TRUE, timeout);
    if (waitRes == VK_TIMEOUT) {
        return vkutil::VkExpected<bool>(false);
    }
    if (waitRes != VK_SUCCESS) {
        return vkutil::VkExpected<bool>(vkutil::checkResult(waitRes, "vkWaitForFences", "sync").context());
    }

    std::shared_lock<std::shared_mutex> stateLock(stateMutex_);
    frameCompletedValues_[frameIndex]->store(frameSubmittedValues_[frameIndex]->load(std::memory_order_acquire), std::memory_order_release);
    return vkutil::VkExpected<bool>(true);
}

vkutil::VkExpected<void> SyncContext::prepareFrameForSubmit(uint32_t frameIndex, SubmitFrameSyncPolicy policy)
{
    {
        std::shared_lock<std::shared_mutex> stateLock(stateMutex_);
        if (timelineMode_.load(std::memory_order_acquire)) {
            return {};
        }
        if (frameIndex >= framesInFlight_.load(std::memory_order_acquire)) {
            return vkutil::makeError("SyncContext::prepareFrameForSubmit", VK_ERROR_INITIALIZATION_FAILED, "sync");
        }
    }

    auto processWaitResult = [&](const vkutil::VkExpected<bool>& waitRes) -> vkutil::VkExpected<void> {
        if (!waitRes.hasValue()) {
            return vkutil::VkExpected<void>(waitRes.context());
        }
        if (!waitRes.value()) {
            return vkutil::makeError("SyncContext::prepareFrameForSubmit", VK_NOT_READY, "sync", "frame_not_ready", 0, true);
        }
        return {};
    };

    if (policy.fenceWaitPolicy == FenceWaitPolicy::Poll) {
        const auto readyRes = pollFenceComplete(frameIndex);
        const auto processed = processWaitResult(readyRes);
        if (!processed.hasValue()) {
            return processed;
        }
    } else if (policy.fenceWaitPolicy == FenceWaitPolicy::Wait) {
        const auto waitRes = waitFence(frameIndex, policy.waitTimeout);
        const auto processed = processWaitResult(waitRes);
        if (!processed.hasValue()) {
            return processed;
        }
    } else if (policy.fenceWaitPolicy == FenceWaitPolicy::AssertSignaled) {
        const auto readyRes = pollFenceComplete(frameIndex);
        if (!readyRes.hasValue()) {
            return vkutil::VkExpected<void>(readyRes.context());
        }
        if (!readyRes.value()) {
            return vkutil::makeError("SyncContext::prepareFrameForSubmit", VK_ERROR_VALIDATION_FAILED_EXT, "sync", "frame_fence_unsignaled", 0, false);
        }
    }

    VkFence frameFence = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    {
        std::shared_lock<std::shared_mutex> stateLock(stateMutex_);
        frameFence = frameFences_[frameIndex].get();
        device = device_;
    }

    const VkResult resetRes = vkResetFences(device, 1u, &frameFence);
    if (resetRes != VK_SUCCESS) {
        return vkutil::checkResult(resetRes, "vkResetFences", "sync");
    }

    std::shared_lock<std::shared_mutex> stateLock(stateMutex_);
    frameSubmittedValues_[frameIndex]->store(0, std::memory_order_release);
    frameCompletedValues_[frameIndex]->store(0, std::memory_order_release);
    return {};
}

void SyncContext::setStagePolicy(VkPipelineStageFlags2 timelineWaitStage,
    VkPipelineStageFlags2 timelineSignalStage,
    VkPipelineStageFlags2 externalSignalStage) noexcept
{
    std::shared_lock<std::shared_mutex> stateLock(stateMutex_);
    defaultTimelineWaitStage_.store((timelineWaitStage == 0) ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : timelineWaitStage, std::memory_order_release);
    defaultTimelineSignalStage_.store((timelineSignalStage == 0) ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : timelineSignalStage, std::memory_order_release);
    defaultExternalSignalStage_.store((externalSignalStage == 0) ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : externalSignalStage, std::memory_order_release);
}

vkutil::VkExpected<void> SyncContext::resetFrame(uint32_t frameIndex)
{
    VkDevice device = VK_NULL_HANDLE;
    VkFence frameFence = VK_NULL_HANDLE;
    {
        std::shared_lock<std::shared_mutex> stateLock(stateMutex_);
        if (frameIndex >= framesInFlight_.load(std::memory_order_acquire)) {
            return vkutil::makeError("SyncContext::resetFrame", VK_ERROR_INITIALIZATION_FAILED, "sync");
        }

        if (timelineMode_.load(std::memory_order_acquire)) {
            timelineFrameValues_[frameIndex]->store(0, std::memory_order_release);
            return {};
        }

        frameFence = frameFences_[frameIndex].get();
        device = device_;
    }

    const VkResult resetRes = vkResetFences(device, 1u, &frameFence);
    if (resetRes != VK_SUCCESS) {
        return vkutil::checkResult(resetRes, "vkResetFences", "sync");
    }

    std::shared_lock<std::shared_mutex> postResetLock(stateMutex_);
    if (frameIndex >= framesInFlight_.load(std::memory_order_acquire) || timelineMode_.load(std::memory_order_acquire)) {
        return {};
    }
    frameSubmittedValues_[frameIndex]->store(0, std::memory_order_release);
    frameCompletedValues_[frameIndex]->store(0, std::memory_order_release);
    return {};
}

vkutil::VkExpected<void> submitWithTimeline2(const VulkanQueue& queue,
    const VkSubmitInfo2& submitInfo,
    VkFence fence)
{
    return queue.submit2({ submitInfo }, fence, "sync_context");
}
