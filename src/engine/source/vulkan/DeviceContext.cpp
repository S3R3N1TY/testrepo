// DeviceContext.cpp
#include <iostream>
#include <stdexcept>
#include <utility>
#include <cassert>

// parasoft-begin-suppress ALL "suppress all violations"
#include <GLFW/glfw3.h>
// parasoft-end-suppress ALL "suppress all violations"

 #include "DeviceContext.h"
 #include "VkUtils.h"
 #include "DeferredDeletionService.h"
namespace
{
    const std::vector<const char*> kRequiredDeviceExtensions{
        VK_KHR_SWAPCHAIN_EXTENSION_NAME
    };
    constexpr const char kGraphicsQueueNull[] = "DeviceContext: graphicsQueue() called but graphicsQ is null";
    constexpr const char kPresentQueueNull[] = "DeviceContext: presentQueue() called but presentQ is null";
    constexpr const char kTransferQueueNull[] = "DeviceContext: transferQueue() called but transferQ is null";
    constexpr const char kComputeQueueNull[] = "DeviceContext: computeQueue() called but computeQ is null";
}

bool DeviceContext::RuntimeHandle::valid() const noexcept
{
    if (!snapshot_) {
        return false;
    }
    if (owner_ == nullptr) {
        return true;
    }
    std::shared_lock lock(owner_->runtimeMutex_);
    return owner_->generationMatchesLocked(generation_) && owner_->runtimeSnapshot_ == snapshot_;
}

bool DeviceContext::QueueSubmissionToken::valid() const noexcept
{
    if (owner_ == nullptr || owner_->isShuttingDownFast()) {
        return false;
    }

    std::shared_lock lock(owner_->runtimeMutex_);
    if (!owner_->generationMatchesLocked(generation_)) {
        return false;
    }
    const VulkanQueue* queue = owner_->queueForSelectionLocked(queueSelection_);
    return queue != nullptr && queue->valid();
}

VkQueue DeviceContext::QueueSubmissionToken::queue() const noexcept
{
    if (owner_ == nullptr || owner_->isShuttingDownFast()) {
        return VK_NULL_HANDLE;
    }

    std::shared_lock lock(owner_->runtimeMutex_);
    if (!owner_->generationMatchesLocked(generation_)) {
        return VK_NULL_HANDLE;
    }
    const VulkanQueue* selectedQueue = owner_->queueForSelectionLocked(queueSelection_);
    return selectedQueue ? selectedQueue->get() : VK_NULL_HANDLE;
}

vkutil::VkExpected<void> DeviceContext::QueueSubmissionToken::submit(
    const std::vector<VkSubmitInfo>& submitInfos,
    VkFence fence,
    const char* subsystem) const
{
    if (owner_ == nullptr || owner_->isShuttingDownFast()) {
        return vkutil::makeError("DeviceContext::QueueSubmissionToken::submit", VK_ERROR_INITIALIZATION_FAILED, "queue");
    }

    DeviceContext::QueueRuntimeSnapshot queueSnapshot{};
    {
        std::shared_lock lock(owner_->runtimeMutex_);
        queueSnapshot = owner_->snapshotQueueForTokenLocked(queueSelection_, generation_);
    }

    if (queueSnapshot.generation != generation_) {
        return vkutil::makeError("DeviceContext::QueueSubmissionToken::submit", VK_ERROR_DEVICE_LOST, "queue", "stale_token");
    }
    if (!queueSnapshot.valid || !queueSnapshot.queue.valid()) {
        return vkutil::makeError("DeviceContext::QueueSubmissionToken::submit", VK_ERROR_INITIALIZATION_FAILED, "queue", "queue_unavailable");
    }

    return queueSnapshot.queue.submit(submitInfos, fence, subsystem);
}

vkutil::VkExpected<void> DeviceContext::QueueSubmissionToken::submit2(
    const std::vector<VkSubmitInfo2>& submitInfos,
    VkFence fence,
    const char* subsystem) const
{
    if (owner_ == nullptr || owner_->isShuttingDownFast()) {
        return vkutil::makeError("DeviceContext::QueueSubmissionToken::submit2", VK_ERROR_INITIALIZATION_FAILED, "queue");
    }

    DeviceContext::QueueRuntimeSnapshot queueSnapshot{};
    {
        std::shared_lock lock(owner_->runtimeMutex_);
        queueSnapshot = owner_->snapshotQueueForTokenLocked(queueSelection_, generation_);
    }

    if (queueSnapshot.generation != generation_) {
        return vkutil::makeError("DeviceContext::QueueSubmissionToken::submit2", VK_ERROR_DEVICE_LOST, "queue", "stale_token");
    }
    if (!queueSnapshot.valid || !queueSnapshot.queue.valid()) {
        return vkutil::makeError("DeviceContext::QueueSubmissionToken::submit2", VK_ERROR_INITIALIZATION_FAILED, "queue", "queue_unavailable");
    }

    return queueSnapshot.queue.submit2(submitInfos, fence, subsystem);
}

VkResult DeviceContext::QueueSubmissionToken::present(
    VkSwapchainKHR swapchain,
    uint32_t imageIndex,
    VkSemaphore waitSemaphore) const
{
    if (owner_ == nullptr || owner_->isShuttingDownFast()) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    DeviceContext::QueueRuntimeSnapshot queueSnapshot{};
    {
        std::shared_lock lock(owner_->runtimeMutex_);
        queueSnapshot = owner_->snapshotQueueForTokenLocked(queueSelection_, generation_);
    }

    if (queueSnapshot.generation != generation_) {
        return VK_ERROR_DEVICE_LOST;
    }
    if (!queueSnapshot.valid || !queueSnapshot.queue.valid()) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    return queueSnapshot.queue.present(swapchain, imageIndex, waitSemaphore);
}

VkResult DeviceContext::QueueSubmissionToken::present(
    VkSwapchainKHR swapchain,
    uint32_t imageIndex,
    const std::vector<VkSemaphore>& waitSemaphores) const
{
    if (owner_ == nullptr || owner_->isShuttingDownFast()) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    DeviceContext::QueueRuntimeSnapshot queueSnapshot{};
    {
        std::shared_lock lock(owner_->runtimeMutex_);
        queueSnapshot = owner_->snapshotQueueForTokenLocked(queueSelection_, generation_);
    }

    if (queueSnapshot.generation != generation_) {
        return VK_ERROR_DEVICE_LOST;
    }
    if (!queueSnapshot.valid || !queueSnapshot.queue.valid()) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    return queueSnapshot.queue.present(swapchain, imageIndex, waitSemaphores);
}

vkutil::VkExpected<void> DeviceContext::QueueSubmissionToken::waitIdle() const
{
    if (owner_ == nullptr || owner_->isShuttingDownFast()) {
        return vkutil::makeError("DeviceContext::QueueSubmissionToken::waitIdle", VK_ERROR_INITIALIZATION_FAILED, "queue");
    }

    DeviceContext::QueueRuntimeSnapshot queueSnapshot{};
    {
        std::shared_lock lock(owner_->runtimeMutex_);
        queueSnapshot = owner_->snapshotQueueForTokenLocked(queueSelection_, generation_);
    }

    if (queueSnapshot.generation != generation_) {
        return vkutil::makeError("DeviceContext::QueueSubmissionToken::waitIdle", VK_ERROR_DEVICE_LOST, "queue", "stale_token");
    }
    if (!queueSnapshot.valid || !queueSnapshot.queue.valid()) {
        return vkutil::makeError("DeviceContext::QueueSubmissionToken::waitIdle", VK_ERROR_INITIALIZATION_FAILED, "queue", "queue_unavailable");
    }

    return queueSnapshot.queue.waitIdle();
}

DeviceRuntimeView DeviceContext::RuntimeHandle::view() const
{
    if (!valid()) {
        throw std::runtime_error("DeviceContext::RuntimeHandle::view: stale runtime handle");
    }
    return DeviceRuntimeView{ .snapshot = snapshot_ };
}

VkInstance DeviceRuntimeView::instance() const noexcept { return snapshot ? snapshot->instance : VK_NULL_HANDLE; }
VkPhysicalDevice DeviceRuntimeView::physicalDevice() const noexcept { return snapshot ? snapshot->physicalDevice : VK_NULL_HANDLE; }
VkDevice DeviceRuntimeView::device() const noexcept { return snapshot ? snapshot->device : VK_NULL_HANDLE; }
VkSurfaceKHR DeviceRuntimeView::surface() const noexcept { return snapshot ? snapshot->surface : VK_NULL_HANDLE; }
VkQueue DeviceRuntimeView::graphicsQueue() const noexcept { return snapshot ? snapshot->graphicsQueue : VK_NULL_HANDLE; }
VkQueue DeviceRuntimeView::presentQueue() const noexcept { return snapshot ? snapshot->presentQueue : VK_NULL_HANDLE; }
VkQueue DeviceRuntimeView::transferQueue() const noexcept { return snapshot ? snapshot->transferQueue : VK_NULL_HANDLE; }
VkQueue DeviceRuntimeView::computeQueue() const noexcept { return snapshot ? snapshot->computeQueue : VK_NULL_HANDLE; }
const VulkanDeviceCapabilities* DeviceRuntimeView::capabilities() const noexcept { return snapshot ? &snapshot->capabilities : nullptr; }
const VulkanInstanceCapabilityProfile* DeviceRuntimeView::instanceCapabilities() const noexcept { return snapshot ? &snapshot->instanceCapabilities : nullptr; }
uint64_t DeviceRuntimeView::generation() const noexcept { return snapshot ? snapshot->generation : 0; }

DeviceContext::DeviceContext(GLFWwindow* window, bool enableValidation_)
    : instance(nullptr)
    , debugMessenger(nullptr)
    , surface(nullptr)
    , physical(nullptr)
    , device(nullptr)
    , graphicsQ(nullptr)
    , presentQ(nullptr)
    , transferQ(nullptr)
    , computeQ(nullptr)
    , enableValidation(enableValidation_)
    , supportedFeatures{}
    , enabledFeatures{}
    , physicalProperties{}
    , samplerAnisotropyEnabled(false)
    , maxSamplerAnisotropy(1.0f)
    , timelineSemaphoreSupported(false)
    , timelineSemaphoreEnabled(false)
    , gpuAllocator(nullptr)
    , syncContext(nullptr)
{
    shuttingDownFast_.store(false, std::memory_order_release);
    if (!window) {
        throw std::runtime_error("DeviceContext: window is null");
    }

    uint32_t glfwExtCount = 0;
    const char* const* const glfwExts = glfwGetRequiredInstanceExtensions(&glfwExtCount);
    if (!glfwExts || glfwExtCount == 0) {
        throw std::runtime_error("DeviceContext: glfwGetRequiredInstanceExtensions returned none (is GLFW initialised?)");
    }

    std::vector<const char*> instanceExts(glfwExts, glfwExts + glfwExtCount);

    instance = std::make_unique<VulkanInstance>(instanceExts, enableValidation);

    if (enableValidation) {
        debugMessenger = std::make_unique<VulkanDebugUtilsMessenger>(instance->get());
    }

    surface = std::make_unique<VulkanSurface>(instance->get(), window);

    DeviceFeaturePolicy featurePolicy = DeviceFeaturePolicy::engineDefault();
    featurePolicy.requiredExtensions = kRequiredDeviceExtensions;
    featurePolicy.experimentalExtensions = {
        VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME
    };

    physical = std::make_unique<VulkanPhysicalDevice>(
        instance->get(),
        surface->get(),
        kRequiredDeviceExtensions,
        featurePolicy,
        instance->capabilities().negotiatedApiVersion
    );

    physical->getFeatures(supportedFeatures);
    physical->getProperties(physicalProperties);
    capabilities = physical->capabilities();
    enabledFeatures = capabilities.enabledFeatures2.features;

    samplerAnisotropyEnabled = (enabledFeatures.samplerAnisotropy == VK_TRUE);
    maxSamplerAnisotropy = samplerAnisotropyEnabled
        ? physicalProperties.limits.maxSamplerAnisotropy
        : 1.0f;

    timelineSemaphoreSupported = capabilities.timelineSemaphoreSupported;
    timelineSemaphoreEnabled = capabilities.timelineSemaphoreEnabled;

    const auto& qf = physical->queues();
    device = std::make_unique<VulkanDevice>(
        physical->get(),
        qf,
        capabilities,
        featurePolicy
    );

    DeferredDeletionService::instance().registerDevice(device->get());

    const bool synchronization2Enabled = device->synchronization2Enabled();
    const PFN_vkQueueSubmit2 queueSubmit2 = device->dispatch().queueSubmit2;

    graphicsQ = std::make_unique<VulkanQueue>(device->get(), qf.graphicsFamily, 0u, synchronization2Enabled, queueSubmit2);
    presentQ = std::make_unique<VulkanQueue>(device->get(), qf.presentFamily, 0u, synchronization2Enabled, queueSubmit2);
    transferQ = std::make_unique<VulkanQueue>(device->get(), qf.transferFamily, 0u, synchronization2Enabled, queueSubmit2);
    computeQ = std::make_unique<VulkanQueue>(device->get(), qf.computeFamily, 0u, synchronization2Enabled, queueSubmit2);

    if (enableValidation) {
        vkutil::initDebugUtils(instance->get(), device->get());
    }

    gpuAllocator = std::make_unique<GpuAllocator>(device->get(), physical->get(), capabilities.bufferDeviceAddressEnabled);
    syncContext = std::make_unique<SyncContext>(
        device->get(),
        2u,
        timelineSemaphoreEnabled,
        capabilities.synchronization2Enabled);
    runtimeSnapshot_ = buildRuntimeSnapshotLocked();
    state_ = State::Alive;
}

DeviceContext::~DeviceContext() noexcept
{
    cleanup();
}

DeviceContext::DeviceContext(DeviceContext&& other) noexcept
    : instance(std::move(other.instance))
    , debugMessenger(std::move(other.debugMessenger))
    , surface(std::move(other.surface))
    , physical(std::move(other.physical))
    , device(std::move(other.device))
    , graphicsQ(std::move(other.graphicsQ))
    , presentQ(std::move(other.presentQ))
    , transferQ(std::move(other.transferQ))
    , computeQ(std::move(other.computeQ))
    , enableValidation(other.enableValidation)
    , supportedFeatures(other.supportedFeatures)
    , enabledFeatures(other.enabledFeatures)
    , capabilities(other.capabilities)
    , physicalProperties(other.physicalProperties)
    , samplerAnisotropyEnabled(other.samplerAnisotropyEnabled)
    , maxSamplerAnisotropy(other.maxSamplerAnisotropy)
    , timelineSemaphoreSupported(other.timelineSemaphoreSupported)
    , timelineSemaphoreEnabled(other.timelineSemaphoreEnabled)
    , gpuAllocator(std::move(other.gpuAllocator))
    , syncContext(std::move(other.syncContext))
    , state_(other.state_)
    , generation_(other.generation_)
    , shuttingDownFast_(other.shuttingDownFast_.load(std::memory_order_acquire))
{
    {
        std::unique_lock lock(other.runtimeMutex_);
        runtimeSnapshot_ = std::move(other.runtimeSnapshot_);
        state_ = other.state_;
        generation_ = other.generation_;
        shuttingDownFast_.store(other.shuttingDownFast_.load(std::memory_order_acquire), std::memory_order_release);
    }
    {
        std::unique_lock lock(runtimeMutex_);
        refreshRuntimeSnapshotLocked();
    }
    other.becomeMovedFrom();
}

DeviceContext& DeviceContext::operator=(DeviceContext&& other) noexcept
{
    if (this == &other) {
        return *this;
    }

    cleanup();

    instance = std::move(other.instance);
    debugMessenger = std::move(other.debugMessenger);
    surface = std::move(other.surface);
    physical = std::move(other.physical);
    device = std::move(other.device);
    graphicsQ = std::move(other.graphicsQ);
    presentQ = std::move(other.presentQ);
    transferQ = std::move(other.transferQ);
    computeQ = std::move(other.computeQ);

    enableValidation = other.enableValidation;
    supportedFeatures = other.supportedFeatures;
    enabledFeatures = other.enabledFeatures;
    capabilities = other.capabilities;
    physicalProperties = other.physicalProperties;
    samplerAnisotropyEnabled = other.samplerAnisotropyEnabled;
    maxSamplerAnisotropy = other.maxSamplerAnisotropy;
    timelineSemaphoreSupported = other.timelineSemaphoreSupported;
    timelineSemaphoreEnabled = other.timelineSemaphoreEnabled;

    gpuAllocator = std::move(other.gpuAllocator);
    syncContext = std::move(other.syncContext);

    {
        std::scoped_lock lock(runtimeMutex_, other.runtimeMutex_);
        state_ = other.state_;
        generation_ = other.generation_;
        runtimeSnapshot_ = std::move(other.runtimeSnapshot_);
        shuttingDownFast_.store(other.shuttingDownFast_.load(std::memory_order_acquire), std::memory_order_release);
        refreshRuntimeSnapshotLocked();
    }

    other.becomeMovedFrom();
    return *this;
}

void DeviceContext::refreshRuntimeSnapshotLocked() noexcept
{
    if (state_ == State::Alive && hasLiveRuntimeLocked()) {
        if (!runtimeSnapshot_ || runtimeSnapshot_->generation != generation_) {
            runtimeSnapshot_ = buildRuntimeSnapshotLocked();
        }
    }
}

void DeviceContext::becomeMovedFrom() noexcept
{
    std::unique_lock lock(runtimeMutex_);
    shuttingDownFast_.store(true, std::memory_order_release);
    instance.reset();
    debugMessenger.reset();
    surface.reset();
    physical.reset();
    device.reset();
    graphicsQ.reset();
    presentQ.reset();
    transferQ.reset();
    computeQ.reset();
    gpuAllocator.reset();
    syncContext.reset();

    enableValidation = false;
    supportedFeatures = {};
    enabledFeatures = {};
    capabilities = {};
    physicalProperties = {};
    samplerAnisotropyEnabled = false;
    maxSamplerAnisotropy = 1.0f;
    timelineSemaphoreSupported = false;
    timelineSemaphoreEnabled = false;

    ++generation_;
    state_ = State::Destroyed;
    runtimeSnapshot_.reset();
}

bool DeviceContext::hasLiveRuntimeLocked() const noexcept
{
    return instance && surface && physical && device && graphicsQ && presentQ && transferQ && computeQ;
}

std::shared_ptr<const DeviceRuntimeSnapshot> DeviceContext::buildRuntimeSnapshotLocked() const
{
    if (!hasLiveRuntimeLocked()) {
        return {};
    }
    auto snapshot = std::make_shared<DeviceRuntimeSnapshot>();
    snapshot->instance = instance->get();
    snapshot->physicalDevice = physical->get();
    snapshot->device = device->get();
    snapshot->surface = surface->get();
    snapshot->graphicsQueue = graphicsQ->get();
    snapshot->presentQueue = presentQ->get();
    snapshot->transferQueue = transferQ->get();
    snapshot->computeQueue = computeQ->get();
    snapshot->capabilities = capabilities;
    snapshot->instanceCapabilities = instance->capabilities();
    snapshot->generation = generation_;
    return snapshot;
}

void DeviceContext::requireAliveLocked(const char* apiName) const
{
#ifndef NDEBUG
    if (state_ == State::Alive) {
        assert(hasLiveRuntimeLocked() && "DeviceContext: Alive state must have a complete runtime");
    }
#endif

    if (state_ != State::Alive || !hasLiveRuntimeLocked()) {
        throw std::runtime_error(std::string("DeviceContext: ") + apiName + " requires Alive state");
    }
}

bool DeviceContext::waitDeviceIdle() noexcept
{
    if (isShuttingDownFast()) {
        return true;
    }

    VkDevice deviceHandle = VK_NULL_HANDLE;
    {
        std::shared_lock lock(runtimeMutex_);
        if (state_ != State::Alive && state_ != State::ShuttingDown) {
            return true;
        }
        if (!device) {
            return true;
        }
        deviceHandle = device->get();
    }

    const VkResult res = vkDeviceWaitIdle(deviceHandle);
    if (res == VK_SUCCESS) {
        return true;
    }

    std::cerr << "DeviceContext: vkDeviceWaitIdle failed with "
              << vkutil::vkResultToString(res) << "\n";
    return false;
}

void DeviceContext::cleanup() noexcept
{
    std::unique_ptr<VulkanInstance> localInstance;
    std::unique_ptr<VulkanDebugUtilsMessenger> localDebugMessenger;
    std::unique_ptr<VulkanSurface> localSurface;
    std::unique_ptr<VulkanPhysicalDevice> localPhysical;
    std::unique_ptr<VulkanDevice> localDevice;
    std::unique_ptr<VulkanQueue> localGraphicsQ;
    std::unique_ptr<VulkanQueue> localPresentQ;
    std::unique_ptr<VulkanQueue> localTransferQ;
    std::unique_ptr<VulkanQueue> localComputeQ;
    std::unique_ptr<GpuAllocator> localAllocator;
    std::unique_ptr<SyncContext> localSyncContext;
    VkDevice deviceHandle = VK_NULL_HANDLE;

    {
        std::unique_lock lock(runtimeMutex_);
        if (state_ == State::Destroyed) {
            return;
        }

        shuttingDownFast_.store(true, std::memory_order_release);
        state_ = State::ShuttingDown;

        if (device) {
            deviceHandle = device->get();
        }

        localPresentQ = std::move(presentQ);
        localGraphicsQ = std::move(graphicsQ);
        localTransferQ = std::move(transferQ);
        localComputeQ = std::move(computeQ);
        localSyncContext = std::move(syncContext);
        localAllocator = std::move(gpuAllocator);
        localDevice = std::move(device);
        localPhysical = std::move(physical);
        localSurface = std::move(surface);
        localDebugMessenger = std::move(debugMessenger);
        localInstance = std::move(instance);

        enableValidation = false;
        timelineSemaphoreSupported = false;
        timelineSemaphoreEnabled = false;
        capabilities = VulkanDeviceCapabilities{};
        supportedFeatures = {};
        enabledFeatures = {};
        physicalProperties = {};
        samplerAnisotropyEnabled = false;
        maxSamplerAnisotropy = 1.0f;

        ++generation_;
        state_ = State::Destroyed;
        runtimeSnapshot_.reset();
    }

    if (deviceHandle != VK_NULL_HANDLE) {
        const VkResult idleRes = vkDeviceWaitIdle(deviceHandle);
        if (idleRes != VK_SUCCESS) {
            std::cerr << "DeviceContext: vkDeviceWaitIdle failed during cleanup with "
                      << vkutil::vkResultToString(idleRes) << "\n";
        }
    }

    localSyncContext.reset();
    localAllocator.reset();
    localPresentQ.reset();
    localGraphicsQ.reset();
    localTransferQ.reset();
    localComputeQ.reset();

    if (deviceHandle != VK_NULL_HANDLE) {
        static_cast<void>(DeferredDeletionService::instance().flush(deviceHandle));
        vkutil::shutdownDebugUtils(deviceHandle);
        DeferredDeletionService::instance().unregisterDevice(deviceHandle);
    }

    localDevice.reset();
    localPhysical.reset();
    localSurface.reset();
    localDebugMessenger.reset();
    localInstance.reset();

    vkutil::shutdownDebugUtils();
}

bool DeviceContext::valid() const noexcept
{
    std::shared_lock lock(runtimeMutex_);
    return state_ == State::Alive && hasLiveRuntimeLocked() && static_cast<bool>(runtimeSnapshot_);
}

DeviceContext::RuntimeHandle DeviceContext::runtimeHandle() const noexcept
{
    std::shared_lock lock(runtimeMutex_);
    if (state_ != State::Alive || !runtimeSnapshot_) {
        return RuntimeHandle{};
    }
    return RuntimeHandle(this, generation_, runtimeSnapshot_);
}

DeviceContext::QueueRuntimeSnapshot DeviceContext::snapshotQueueForTokenLocked(
    QueueSelection selection,
    uint64_t expectedGeneration) const noexcept
{
    QueueRuntimeSnapshot snapshot{};
    snapshot.generation = generation_;
    if (state_ != State::Alive || generation_ != expectedGeneration || !hasLiveRuntimeLocked()) {
        return snapshot;
    }

    const VulkanQueue* selectedQueue = queueForSelectionLocked(selection);
    if (selectedQueue == nullptr || !selectedQueue->valid()) {
        return snapshot;
    }

    snapshot.queue = *selectedQueue;
    snapshot.valid = true;
    return snapshot;
}

bool DeviceContext::generationMatchesLocked(uint64_t generation) const noexcept
{
    return generation == generation_ && state_ == State::Alive && hasLiveRuntimeLocked();
}

DeviceRuntimeView DeviceContext::runtimeView() const
{
    std::shared_lock lock(runtimeMutex_);
    requireAliveLocked("runtimeView");
    return DeviceRuntimeView{ .snapshot = runtimeSnapshot_ };
}

DeviceContext::QueueSubmissionToken DeviceContext::graphicsQueueToken() const noexcept
{
    std::shared_lock lock(runtimeMutex_);
    if (state_ != State::Alive || !runtimeSnapshot_) {
        return QueueSubmissionToken{};
    }
    return QueueSubmissionToken(this, generation_, QueueSelection::Graphics);
}

DeviceContext::QueueSubmissionToken DeviceContext::presentQueueToken() const noexcept
{
    std::shared_lock lock(runtimeMutex_);
    if (state_ != State::Alive || !runtimeSnapshot_) {
        return QueueSubmissionToken{};
    }
    return QueueSubmissionToken(this, generation_, QueueSelection::Present);
}

DeviceContext::QueueSubmissionToken DeviceContext::transferQueueToken() const noexcept
{
    std::shared_lock lock(runtimeMutex_);
    if (state_ != State::Alive || !runtimeSnapshot_) {
        return QueueSubmissionToken{};
    }
    return QueueSubmissionToken(this, generation_, QueueSelection::Transfer);
}

DeviceContext::QueueSubmissionToken DeviceContext::computeQueueToken() const noexcept
{
    std::shared_lock lock(runtimeMutex_);
    if (state_ != State::Alive || !runtimeSnapshot_) {
        return QueueSubmissionToken{};
    }
    return QueueSubmissionToken(this, generation_, QueueSelection::Compute);
}

const VulkanQueue* DeviceContext::queueForSelectionLocked(QueueSelection selection) const noexcept
{
    switch (selection) {
    case QueueSelection::Graphics:
        return graphicsQ.get();
    case QueueSelection::Present:
        return presentQ.get();
    case QueueSelection::Transfer:
        return transferQ.get();
    case QueueSelection::Compute:
        return computeQ.get();
    default:
        return nullptr;
    }
}


DeviceQueueCapabilityProfile DeviceContext::queueCapabilityProfile() const noexcept
{
    DeviceQueueCapabilityProfile profile{};

    std::shared_lock lock(runtimeMutex_);
    if (state_ != State::Alive || !runtimeSnapshot_) {
        return profile;
    }

    profile.hasGraphicsQueue = graphicsQ != nullptr && graphicsQ->valid();
    profile.hasPresentQueue = presentQ != nullptr && presentQ->valid();
    profile.hasTransferQueue = transferQ != nullptr && transferQ->valid();
    profile.hasComputeQueue = computeQ != nullptr && computeQ->valid();

    profile.graphicsFamilyIndex = profile.hasGraphicsQueue ? device->graphicsFamilyIndex() : UINT32_MAX;
    profile.presentFamilyIndex = profile.hasPresentQueue ? device->presentFamilyIndex() : UINT32_MAX;
    profile.transferFamilyIndex = profile.hasTransferQueue ? device->transferFamilyIndex() : UINT32_MAX;
    profile.computeFamilyIndex = profile.hasComputeQueue ? device->computeFamilyIndex() : UINT32_MAX;

    profile.transferQueueDedicated = profile.hasTransferQueue
        && profile.transferFamilyIndex != profile.graphicsFamilyIndex
        && profile.transferFamilyIndex != profile.computeFamilyIndex;

    profile.computeQueueDedicated = profile.hasComputeQueue
        && profile.computeFamilyIndex != profile.graphicsFamilyIndex;

    return profile;
}

const VulkanDeviceCapabilities& DeviceContext::deviceCapabilities() const
{
    std::shared_lock lock(runtimeMutex_);
    requireAliveLocked("deviceCapabilities");
    return capabilities;
}

bool DeviceContext::isFeatureSupportedBufferDeviceAddress() const noexcept { return capabilities.bufferDeviceAddressSupported; }
bool DeviceContext::isFeatureEnabledBufferDeviceAddress() const noexcept { return capabilities.bufferDeviceAddressEnabled; }
bool DeviceContext::isFeatureSupportedTimelineSemaphore() const noexcept { return capabilities.timelineSemaphoreSupported; }
bool DeviceContext::isFeatureEnabledTimelineSemaphore() const noexcept { return capabilities.timelineSemaphoreEnabled; }
bool DeviceContext::isFeatureSupportedSynchronization2() const noexcept { return capabilities.synchronization2Supported; }
bool DeviceContext::isFeatureEnabledSynchronization2() const noexcept { return capabilities.synchronization2Enabled; }
bool DeviceContext::isFeatureSupportedDynamicRendering() const noexcept { return capabilities.dynamicRenderingSupported; }
bool DeviceContext::isFeatureEnabledDynamicRendering() const noexcept { return capabilities.dynamicRenderingEnabled; }
bool DeviceContext::isFeatureSupportedDescriptorIndexing() const noexcept { return capabilities.descriptorIndexingSupported; }
bool DeviceContext::isFeatureEnabledDescriptorIndexing() const noexcept { return capabilities.descriptorIndexingEnabled; }

VkDevice DeviceContext::vkDevice() const
{
    std::shared_lock lock(runtimeMutex_);
    requireAliveLocked("vkDevice");
    return device->get();
}

VkPhysicalDevice DeviceContext::vkPhysical() const
{
    std::shared_lock lock(runtimeMutex_);
    requireAliveLocked("vkPhysical");
    return physical->get();
}

VkInstance DeviceContext::vkInstance() const
{
    std::shared_lock lock(runtimeMutex_);
    requireAliveLocked("vkInstance");
    return instance->get();
}

VkSurfaceKHR DeviceContext::vkSurface() const
{
    std::shared_lock lock(runtimeMutex_);
    requireAliveLocked("vkSurface");
    return surface->get();
}

uint32_t DeviceContext::graphicsFamilyIndex() const
{
    std::shared_lock lock(runtimeMutex_);
    requireAliveLocked("graphicsFamilyIndex");
    return device->graphicsFamilyIndex();
}

uint32_t DeviceContext::presentFamilyIndex() const
{
    std::shared_lock lock(runtimeMutex_);
    requireAliveLocked("presentFamilyIndex");
    return device->presentFamilyIndex();
}

uint32_t DeviceContext::transferFamilyIndex() const
{
    std::shared_lock lock(runtimeMutex_);
    requireAliveLocked("transferFamilyIndex");
    return device->transferFamilyIndex();
}

uint32_t DeviceContext::computeFamilyIndex() const
{
    std::shared_lock lock(runtimeMutex_);
    requireAliveLocked("computeFamilyIndex");
    return device->computeFamilyIndex();
}

VulkanQueue DeviceContext::graphicsQueue() const
{
    std::shared_lock lock(runtimeMutex_);
    requireAliveLocked("graphicsQueue");
    if (!graphicsQ) throw std::runtime_error(kGraphicsQueueNull);
    return *graphicsQ;
}

VulkanQueue DeviceContext::presentQueue() const
{
    std::shared_lock lock(runtimeMutex_);
    requireAliveLocked("presentQueue");
    if (!presentQ) throw std::runtime_error(kPresentQueueNull);
    return *presentQ;
}

VulkanQueue DeviceContext::transferQueue() const
{
    std::shared_lock lock(runtimeMutex_);
    requireAliveLocked("transferQueue");
    if (!transferQ) throw std::runtime_error(kTransferQueueNull);
    return *transferQ;
}

VulkanQueue DeviceContext::computeQueue() const
{
    std::shared_lock lock(runtimeMutex_);
    requireAliveLocked("computeQueue");
    if (!computeQ) throw std::runtime_error(kComputeQueueNull);
    return *computeQ;
}
