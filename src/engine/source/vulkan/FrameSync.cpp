// ==============================
// FrameSync.cpp
// ==============================
#include <stdexcept>
#include <string>
#include <chrono>

 #include "FrameSync.h"
 #include "VkUtils.h"
namespace
{
    constexpr const char kPresentSemaphorePrefix[] = "PresentSem[";
    constexpr const char kCmdBufferPrefix[] = "CmdBuffer[";
    constexpr const char kImageAvailablePrefix[] = "ImageAvailable[";
}

void FrameSync::init(VkDevice dev,
    uint32_t graphicsFamilyIndex,
    size_t swapchainImageCount_,
    bool enableValidation,
    RuntimeConfig config)
{
    const std::lock_guard<std::mutex> lock(stateMutex_);

    if (dev == VK_NULL_HANDLE) {
        throw std::runtime_error("FrameSync::init: device is VK_NULL_HANDLE");
    }
    if (swapchainImageCount_ == 0) {
        throw std::runtime_error("FrameSync::init: swapchainImageCount must be > 0");
    }

    // If re-init is called, clean up first (idempotent safety).
    cleanupUnlocked();

    device = dev;
    validation = enableValidation;
    swapImageCount = swapchainImageCount_;
    config_ = config;
    if (config_.framesInFlight == 0) {
        config_.framesInFlight = 2;
    }
    framesInFlight_ = config_.framesInFlight;

    // ------------------------------------------------------------
    // Command pool + per-frame command buffers
    // ------------------------------------------------------------
    VulkanCommandArena::Config cmdArenaConfig{};
    cmdArenaConfig.device = device;
    cmdArenaConfig.queueFamilyIndex = graphicsFamilyIndex;
    cmdArenaConfig.framesInFlight = framesInFlight_;
    cmdArenaConfig.workerThreads = 1;
    cmdArenaConfig.preallocatePerFrame = 2;
    cmdArena = std::make_unique<VulkanCommandArena>(cmdArenaConfig);

    cmdBuffers.assign(framesInFlight_, VK_NULL_HANDLE);

    // ------------------------------------------------------------
    // Image-available semaphores (per frame-in-flight)
    // ------------------------------------------------------------
    imageAvailableSems.clear();
    imageAvailableSems.reserve(framesInFlight_);
    for (uint32_t i = 0; i < framesInFlight_; ++i) {
        imageAvailableSems.emplace_back(device);
    }

    // ------------------------------------------------------------
    // Timeline semaphores (persistent across swapchain recreations)
    // ------------------------------------------------------------
    timelineSem = std::make_unique<VulkanSemaphore>(device, true);
    uploadTimeline = std::make_unique<TimelineSemaphore>(device, 0);
    currentValue = 0;
    uploadValue = 0;
    frameValues.assign(framesInFlight_, 0);

    // ------------------------------------------------------------
    // Present semaphores (per swapchain image)
    // ------------------------------------------------------------
    presentSems.clear();
    presentSems.reserve(swapImageCount);
    for (size_t i = 0; i < swapImageCount; ++i) {
        presentSems.emplace_back(device);
    }

    currentFrame = 0;

    if (validation) {
        nameObjects();
    }
}

void FrameSync::cleanup() noexcept
{
    const std::lock_guard<std::mutex> lock(stateMutex_);
    cleanupUnlocked();
}

void FrameSync::cleanupUnlocked() noexcept
{
    // Order: command buffers freed before pool, semaphores freed anytime, etc.
    cmdBuffers.clear();
    cmdArena.reset();

    imageAvailableSems.clear();
    presentSems.clear();

    timelineSem.reset();
    uploadTimeline.reset();
    frameValues.clear();

    device = VK_NULL_HANDLE;
    currentValue = 0;
    uploadValue = 0;
    currentFrame = 0;
    swapImageCount = 0;
    validation = false;
    framesInFlight_ = 2;
    config_ = RuntimeConfig{};
    diagnostics_ = Diagnostics{};
}

void FrameSync::recreateForSwapchain(size_t newSwapchainImageCount, Garbage& outGarbage)
{
    const std::lock_guard<std::mutex> lock(stateMutex_);

    if (!valid()) {
        throw std::runtime_error("FrameSync::recreateForSwapchain called before init()");
    }
    if (newSwapchainImageCount == 0) {
        throw std::runtime_error("FrameSync::recreateForSwapchain: newSwapchainImageCount must be > 0");
    }

    outGarbage = Garbage{};
    outGarbage.oldPresentSems = std::move(presentSems);

    presentSems.clear();
    presentSems.reserve(newSwapchainImageCount);
    for (size_t i = 0; i < newSwapchainImageCount; ++i) {
        presentSems.emplace_back(device);
    }

    swapImageCount = newSwapchainImageCount;

    // Safe reset: avoids any weird mismatch assumptions across recreation.
    currentFrame = 0;

    if (validation) {
        for (size_t i = 0; i < presentSems.size(); ++i) {
            const std::string n = kPresentSemaphorePrefix + std::to_string(i) + string_constants::kCloseBracket;
            vkutil::setObjectName(device, VK_OBJECT_TYPE_SEMAPHORE, presentSems[i].get(), n.c_str());
        }
    }
    return;
}

VkSemaphore FrameSync::imageAvailable(uint32_t frameIndex) const
{
    const std::lock_guard<std::mutex> lock(stateMutex_);

    if (frameIndex >= framesInFlight_) {
        throw std::runtime_error("FrameSync::imageAvailable: frameIndex out of range");
    }
    return imageAvailableSems[frameIndex].get();
}

vkutil::VkExpected<uint64_t> FrameSync::completedValue() const
{
    const std::lock_guard<std::mutex> lock(stateMutex_);

    if (!valid()) return vkutil::VkExpected<uint64_t>(VK_ERROR_INITIALIZATION_FAILED);

    uint64_t value = 0;
    const VkResult res = vkGetSemaphoreCounterValue(device, timelineSem->get(), &value);
    if (res != VK_SUCCESS) {
        return vkutil::VkExpected<uint64_t>(res);
    }
    return vkutil::VkExpected<uint64_t>(value);
}

uint64_t FrameSync::nextUploadValue()
{
    const std::lock_guard<std::mutex> lock(stateMutex_);

    if (!valid()) {
        throw std::runtime_error("FrameSync::nextUploadValue called before init()");
    }
    return ++uploadValue;
}

vkutil::VkExpected<uint64_t> FrameSync::uploadCompletedValue() const
{
    const std::lock_guard<std::mutex> lock(stateMutex_);

    if (!valid()) {
        return vkutil::VkExpected<uint64_t>(VK_ERROR_INITIALIZATION_FAILED);
    }
    return uploadTimeline->value();
}

VkSemaphore FrameSync::uploadSemaphore() const
{
    const std::lock_guard<std::mutex> lock(stateMutex_);

    if (!valid()) {
        return VK_NULL_HANDLE;
    }
    return uploadTimeline->get();
}

uint64_t FrameSync::maxInFlightValue() const noexcept
{
    const std::lock_guard<std::mutex> lock(stateMutex_);

    uint64_t m = 0;
    for (const uint64_t v : frameValues) {
        m = std::max(m, v);
    }
    return m;
}

VkResult FrameSync::acquireFrame(VkSwapchainKHR swapchain, Frame& outFrame)
{
    const std::lock_guard<std::mutex> lock(stateMutex_);

    if (!valid()) {
        throw std::runtime_error("FrameSync::acquireFrame called before init()");
    }
    if (swapchain == VK_NULL_HANDLE) {
        throw std::runtime_error("FrameSync::acquireFrame: swapchain is VK_NULL_HANDLE");
    }

    // Wait CPU-side for this frame index if used before.
    const uint64_t waitValue = frameValues[currentFrame];
    if (waitValue != 0) {
        const VkSemaphore sem = timelineSem->get();

        VkSemaphoreWaitInfo wi{ VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
        wi.flags = 0;
        wi.semaphoreCount = 1;
        wi.pSemaphores = &sem;
        wi.pValues = &waitValue;

        VkResult w = VK_SUCCESS;
        const auto waitStart = std::chrono::steady_clock::now();
        if (config_.frameReuseWaitPolicy == WaitPolicy::Poll) {
            uint64_t completed = 0;
            w = vkGetSemaphoreCounterValue(device, sem, &completed);
            if (w == VK_SUCCESS && completed < waitValue) {
                diagnostics_.framesSkipped++;
                diagnostics_.frameReuseTimeoutCount++;
                return VK_NOT_READY;
            }
        } else {
            const uint64_t waitTimeout = (config_.frameReuseWaitPolicy == WaitPolicy::Timed)
                ? config_.frameReuseWaitTimeoutNs
                : UINT64_MAX;
            w = vkWaitSemaphores(device, &wi, waitTimeout);
        }

        diagnostics_.frameReuseWaitCount++;
        diagnostics_.frameReuseWaitedNs += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - waitStart).count());
        if (w == VK_TIMEOUT) {
            diagnostics_.framesSkipped++;
            diagnostics_.frameReuseTimeoutCount++;
            return w;
        }
        if (w != VK_SUCCESS) {
            return w;
        }
        cmdArena->markFrameComplete(currentFrame);
    }

    uint32_t imageIndex = 0;
    const VkResult res = vkAcquireNextImageKHR(
        device,
        swapchain,
        config_.acquireTimeoutNs,
        imageAvailableSems[currentFrame].get(),
        VK_NULL_HANDLE,
        &imageIndex
    );

    if (res == VK_TIMEOUT || res == VK_NOT_READY) {
        diagnostics_.acquireTimeoutCount++;
        diagnostics_.framesSkipped++;
        return res;
    }

    if (res != VK_SUCCESS &&
        res != VK_SUBOPTIMAL_KHR &&
        res != VK_ERROR_OUT_OF_DATE_KHR)
    {
        return res;
    }

    if (swapImageCount > 0 && imageIndex >= swapImageCount) {
        throw std::runtime_error("FrameSync::acquireFrame: acquired imageIndex out of expected range");
    }

    const auto beginFrameRes = cmdArena->beginFrame(currentFrame, waitValue);
    if (!beginFrameRes.hasValue()) {
        return beginFrameRes.error();
    }

    const auto cmdRes = cmdArena->acquirePrimary(beginFrameRes.value(), 0, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
    if (!cmdRes.hasValue()) {
        return cmdRes.error();
    }
    cmdBuffers[currentFrame] = cmdRes.value().handle;

    outFrame.frameIndex = currentFrame;
    outFrame.imageIndex = imageIndex;
    outFrame.cmdBuffer = cmdBuffers[currentFrame];
    return res;
}

VkResult FrameSync::submitAndPresent(const VulkanQueue& graphicsQ,
    const VulkanQueue& presentQ,
    VkSwapchainKHR swapchain,
    const Frame& frame,
    uint64_t uploadWaitValue)
{
    const std::lock_guard<std::mutex> lock(stateMutex_);

    if (!valid()) {
        throw std::runtime_error("FrameSync::submitAndPresent called before init()");
    }
    if (swapchain == VK_NULL_HANDLE) {
        throw std::runtime_error("FrameSync::submitAndPresent: swapchain is VK_NULL_HANDLE");
    }
    if (frame.cmdBuffer == VK_NULL_HANDLE) {
        throw std::runtime_error("FrameSync::submitAndPresent: frame.cmdBuffer is null");
    }
    if (frame.frameIndex >= framesInFlight_) {
        throw std::runtime_error("FrameSync::submitAndPresent: frameIndex out of range");
    }
    if (swapImageCount == 0 || frame.imageIndex >= swapImageCount) {
        throw std::runtime_error("FrameSync::submitAndPresent: imageIndex out of range");
    }

    // Reserve candidate timeline value locally; commit only after successful submit.
    const uint64_t signalValue = currentValue + 1;

    VkSemaphoreSubmitInfo waitInfos[2]{};
    waitInfos[0] = VkSemaphoreSubmitInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
    waitInfos[0].semaphore = imageAvailableSems[frame.frameIndex].get();
    waitInfos[0].value = 0;
    waitInfos[0].stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    uint32_t waitCount = 1;
    if (uploadWaitValue > 0) {
        waitInfos[1] = VkSemaphoreSubmitInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
        waitInfos[1].semaphore = uploadTimeline->get();
        waitInfos[1].value = uploadWaitValue;
        waitInfos[1].stageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        waitCount = 2;
    }

    VkSemaphoreSubmitInfo signalInfos[2]{};
    signalInfos[0] = VkSemaphoreSubmitInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
    signalInfos[0].semaphore = presentSems[frame.imageIndex].get();
    signalInfos[0].value = 0;
    signalInfos[0].stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;

    signalInfos[1] = VkSemaphoreSubmitInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
    signalInfos[1].semaphore = timelineSem->get();
    signalInfos[1].value = signalValue;
    signalInfos[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkCommandBufferSubmitInfo cmdInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
    cmdInfo.commandBuffer = frame.cmdBuffer;

    VkSubmitInfo2 submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    submitInfo.waitSemaphoreInfoCount = waitCount;
    submitInfo.pWaitSemaphoreInfos = waitInfos;
    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos = &cmdInfo;
    submitInfo.signalSemaphoreInfoCount = 2;
    submitInfo.pSignalSemaphoreInfos = signalInfos;

    const auto submitRes = submitWithTimeline2(graphicsQ, submitInfo, VK_NULL_HANDLE);
    if (!submitRes) {
        return submitRes.error();
    }

    currentValue = signalValue;
    frameValues[frame.frameIndex] = signalValue;
    cmdArena->markFrameSubmitted(frame.frameIndex, signalValue);

    const VkResult pres = presentQ.present(
        swapchain,
        frame.imageIndex,
        presentSems[frame.imageIndex].get()
    );

    currentFrame = (currentFrame + 1) % framesInFlight_;
    return pres;
}

void FrameSync::makeRenderPassBegin(const Frame& /*frame*/,
    VkRenderPass renderPass,
    VkFramebuffer fb,
    const VkExtent2D& extent,
    const VkClearValue* clearValues,
    uint32_t clearCount,
    VkRenderPassBeginInfo& outInfo) const noexcept
{
    outInfo = VkRenderPassBeginInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    outInfo.renderPass = renderPass;
    outInfo.framebuffer = fb;
    outInfo.renderArea.offset = { 0, 0 };
    outInfo.renderArea.extent = extent;
    outInfo.clearValueCount = clearCount;
    outInfo.pClearValues = clearValues;
    return;
}

void FrameSync::nameObjects() const
{
    if (!validation || device == VK_NULL_HANDLE) return;

    for (size_t i = 0; i < cmdBuffers.size(); ++i) {
        const std::string n = kCmdBufferPrefix + std::to_string(i) + string_constants::kCloseBracket;
        if (cmdBuffers[i] != VK_NULL_HANDLE) {
            vkutil::setObjectName(device, VK_OBJECT_TYPE_COMMAND_BUFFER, cmdBuffers[i], n.c_str());
        }
    }

    for (size_t i = 0; i < imageAvailableSems.size(); ++i) {
        const std::string n = kImageAvailablePrefix + std::to_string(i) + string_constants::kCloseBracket;
        vkutil::setObjectName(device, VK_OBJECT_TYPE_SEMAPHORE, imageAvailableSems[i].get(), n.c_str());
    }

    for (size_t i = 0; i < presentSems.size(); ++i) {
        const std::string n = kPresentSemaphorePrefix + std::to_string(i) + string_constants::kCloseBracket;
        vkutil::setObjectName(device, VK_OBJECT_TYPE_SEMAPHORE, presentSems[i].get(), n.c_str());
    }

    if (timelineSem) {
        vkutil::setObjectName(device, VK_OBJECT_TYPE_SEMAPHORE, timelineSem->get(), "TimelineSemaphore");
    }
    if (uploadTimeline) {
        vkutil::setObjectName(device, VK_OBJECT_TYPE_SEMAPHORE, uploadTimeline->get(), "UploadTimelineSemaphore");
    }
}
