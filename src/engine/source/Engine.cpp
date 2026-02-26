#include <Engine.h>

#include <vulkan/DeviceContext.h>
#include <vulkan/RenderGraph.h>
#include <vulkan/SubmissionScheduler.h>
#include <vulkan/SwapchainResources.h>
#include <vulkan/VkCommands.h>
#include <vulkan/VkPipeline.h>
#include <vulkan/VkShaderModule.h>
#include <vulkan/VkSync.h>
#include <vulkan/VkUtils.h>

#include <GLFW/glfw3.h>

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {
constexpr uint32_t kFramesInFlight = 2;

struct FrameData {
    VulkanSemaphore imageAvailable{};
    VulkanFence inFlight{};
};


class PersistentWorkerPool {
public:
    explicit PersistentWorkerPool(uint32_t workerCount)
        : workerCount_(std::max<uint32_t>(1u, workerCount))
    {
        workers_.reserve(workerCount_);
        for (uint32_t i = 0; i < workerCount_; ++i) {
            workers_.emplace_back([this, i]() { workerLoop(i); });
        }
    }

    ~PersistentWorkerPool() noexcept
    {
        {
            std::unique_lock lock(mutex_);
            stop_ = true;
            generation_ += 1;
        }
        cvStart_.notify_all();
        for (std::thread& t : workers_) {
            if (t.joinable()) {
                t.join();
            }
        }
    }

    PersistentWorkerPool(const PersistentWorkerPool&) = delete;
    PersistentWorkerPool& operator=(const PersistentWorkerPool&) = delete;

    void run(uint32_t activeWorkerCount, std::function<void(uint32_t)> job)
    {
        if (!job) {
            return;
        }

        const uint32_t clampedActive = std::min(activeWorkerCount, workerCount_);
        if (clampedActive == 0) {
            return;
        }

        {
            std::unique_lock lock(mutex_);
            job_ = std::move(job);
            activeWorkerCount_ = clampedActive;
            completedWorkers_ = 0;
            generation_ += 1;
        }

        cvStart_.notify_all();

        std::unique_lock lock(mutex_);
        cvDone_.wait(lock, [&]() { return completedWorkers_ == workerCount_; });
    }

private:
    void workerLoop(uint32_t workerIndex)
    {
        uint64_t observedGeneration = 0;
        while (true) {
            std::function<void(uint32_t)> job;
            uint32_t activeCount = 0;

            {
                std::unique_lock lock(mutex_);
                cvStart_.wait(lock, [&]() { return stop_ || generation_ != observedGeneration; });
                if (stop_) {
                    return;
                }

                observedGeneration = generation_;
                activeCount = activeWorkerCount_;
                job = job_;
            }

            if (workerIndex < activeCount && job) {
                job(workerIndex);
            }

            {
                std::unique_lock lock(mutex_);
                completedWorkers_ += 1;
                if (completedWorkers_ == workerCount_) {
                    cvDone_.notify_one();
                }
            }
        }
    }

    uint32_t workerCount_{ 1 };
    std::vector<std::thread> workers_{};

    std::mutex mutex_{};
    std::condition_variable cvStart_{};
    std::condition_variable cvDone_{};

    bool stop_{ false };
    uint64_t generation_{ 0 };
    uint32_t activeWorkerCount_{ 0 };
    uint32_t completedWorkers_{ 0 };
    std::function<void(uint32_t)> job_{};
};

std::vector<VulkanSemaphore> createPerImagePresentSemaphores(VkDevice device, uint32_t imageCount)
{
    std::vector<VulkanSemaphore> semaphores{};
    semaphores.reserve(imageCount);
    for (uint32_t i = 0; i < imageCount; ++i) {
        semaphores.emplace_back(device);
    }
    return semaphores;
}

VkDescriptorPool createImGuiDescriptorPool(VkDevice device)
{
    std::array<VkDescriptorPoolSize, 1> poolSizes{
        VkDescriptorPoolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 128 }
    };

    VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 128;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();

    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    const VkResult createResult = vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool);
    if (createResult != VK_SUCCESS) {
        vkutil::throwVkError("vkCreateDescriptorPool(ImGui)", createResult);
    }

    return descriptorPool;
}

void ensure(const vkutil::VkExpected<void>& result, const char* op)
{
    if (!result) {
        throw std::runtime_error(std::string(op) + ": " + vkutil::vkResultToString(result.error()));
    }
}

void ensure(const vkutil::VkExpected<bool>& result, const char* op)
{
    if (!result) {
        throw std::runtime_error(std::string(op) + ": " + vkutil::vkResultToString(result.error()));
    }
    if (!result.value()) {
        throw std::runtime_error(std::string(op) + ": operation returned false");
    }
}

template <typename T>
T unwrap(vkutil::VkExpected<T>&& result, const char* op)
{
    if (!result) {
        throw std::runtime_error(std::string(op) + ": " + vkutil::vkResultToString(result.error()));
    }
    return std::move(result.value());
}

std::vector<char> loadShaderCode(const char* path)
{
    if (path == nullptr || path[0] == '\0') {
        throw std::runtime_error("Shader path is empty");
    }

    std::vector<char> code;
    vkutil::readFile(path, code);
    if (code.empty()) {
        throw std::runtime_error(std::string("Shader file is empty: ") + path);
    }

    return code;
}

const char* fallbackShaderPath(const char* configuredPath, const char* macroPath) noexcept
{
    if (configuredPath != nullptr && configuredPath[0] != '\0') {
        return configuredPath;
    }
    return macroPath;
}

const char* resolveVertexShaderPath(const Engine::RunConfig& config)
{
#ifdef APP_VERT_SHADER_PATH
    return fallbackShaderPath(config.vertexShaderPath, APP_VERT_SHADER_PATH);
#else
    if (config.vertexShaderPath == nullptr || config.vertexShaderPath[0] == '\0') {
        throw std::runtime_error("RunConfig.vertexShaderPath must be set");
    }
    return config.vertexShaderPath;
#endif
}

const char* resolveFragmentShaderPath(const Engine::RunConfig& config)
{
#ifdef APP_FRAG_SHADER_PATH
    return fallbackShaderPath(config.fragmentShaderPath, APP_FRAG_SHADER_PATH);
#else
    if (config.fragmentShaderPath == nullptr || config.fragmentShaderPath[0] == '\0') {
        throw std::runtime_error("RunConfig.fragmentShaderPath must be set");
    }
    return config.fragmentShaderPath;
#endif
}

void validateFrameGraphInput(const FrameGraphInput& frameGraphInput)
{
    std::unordered_set<uint32_t> viewIds{};
    viewIds.reserve(frameGraphInput.views.size());
    for (const RenderViewPacket& view : frameGraphInput.views) {
        if (!viewIds.insert(view.viewId).second) {
            throw std::runtime_error("FrameGraphInput contains duplicate viewId");
        }
    }

    std::unordered_set<uint32_t> materialIds{};
    materialIds.reserve(frameGraphInput.materialBatches.size());
    for (const MaterialBatchPacket& batch : frameGraphInput.materialBatches) {
        if (!materialIds.insert(batch.materialId).second) {
            throw std::runtime_error("FrameGraphInput contains duplicate materialId");
        }
        const uint64_t end = static_cast<uint64_t>(batch.firstDrawPacket) + static_cast<uint64_t>(batch.drawPacketCount);
        if (end > frameGraphInput.drawPackets.size()) {
            throw std::runtime_error("MaterialBatchPacket draw range exceeds drawPackets size");
        }
    }

    for (const DrawPacket& draw : frameGraphInput.drawPackets) {
        if (!viewIds.empty() && !viewIds.contains(draw.viewId)) {
            throw std::runtime_error("DrawPacket references unknown viewId");
        }
        if (!materialIds.empty() && !materialIds.contains(draw.materialId)) {
            throw std::runtime_error("DrawPacket references unknown materialId");
        }
    }
}

VkPipelineStageFlags toLegacyStage(VkPipelineStageFlags2 stageMask) noexcept
{
    const VkPipelineStageFlags legacy = static_cast<VkPipelineStageFlags>(stageMask & 0xFFFFFFFFULL);
    return legacy != 0 ? legacy : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
}

void emitBarrierBatch(VkCommandBuffer commandBuffer, const RenderTaskGraph::BarrierBatch& barriers, bool useSync2)
{
    if (barriers.empty()) {
        return;
    }

    if (useSync2) {
        VkDependencyInfo depInfo{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        depInfo.memoryBarrierCount = static_cast<uint32_t>(barriers.memoryBarriers.size());
        depInfo.pMemoryBarriers = barriers.memoryBarriers.empty() ? nullptr : barriers.memoryBarriers.data();
        depInfo.bufferMemoryBarrierCount = static_cast<uint32_t>(barriers.bufferBarriers.size());
        depInfo.pBufferMemoryBarriers = barriers.bufferBarriers.empty() ? nullptr : barriers.bufferBarriers.data();
        depInfo.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.imageBarriers.size());
        depInfo.pImageMemoryBarriers = barriers.imageBarriers.empty() ? nullptr : barriers.imageBarriers.data();
        vkCmdPipelineBarrier2(commandBuffer, &depInfo);
        return;
    }

    VkPipelineStageFlags srcStages = 0;
    VkPipelineStageFlags dstStages = 0;

    std::vector<VkMemoryBarrier> memoryBarriers{};
    memoryBarriers.reserve(barriers.memoryBarriers.size());
    for (const VkMemoryBarrier2& barrier2 : barriers.memoryBarriers) {
        srcStages |= toLegacyStage(barrier2.srcStageMask);
        dstStages |= toLegacyStage(barrier2.dstStageMask);

        VkMemoryBarrier barrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        barrier.srcAccessMask = static_cast<VkAccessFlags>(barrier2.srcAccessMask & 0xFFFFFFFFULL);
        barrier.dstAccessMask = static_cast<VkAccessFlags>(barrier2.dstAccessMask & 0xFFFFFFFFULL);
        memoryBarriers.push_back(barrier);
    }

    std::vector<VkBufferMemoryBarrier> bufferBarriers{};
    bufferBarriers.reserve(barriers.bufferBarriers.size());
    for (const VkBufferMemoryBarrier2& barrier2 : barriers.bufferBarriers) {
        srcStages |= toLegacyStage(barrier2.srcStageMask);
        dstStages |= toLegacyStage(barrier2.dstStageMask);

        VkBufferMemoryBarrier barrier{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
        barrier.srcAccessMask = static_cast<VkAccessFlags>(barrier2.srcAccessMask & 0xFFFFFFFFULL);
        barrier.dstAccessMask = static_cast<VkAccessFlags>(barrier2.dstAccessMask & 0xFFFFFFFFULL);
        barrier.srcQueueFamilyIndex = barrier2.srcQueueFamilyIndex;
        barrier.dstQueueFamilyIndex = barrier2.dstQueueFamilyIndex;
        barrier.buffer = barrier2.buffer;
        barrier.offset = barrier2.offset;
        barrier.size = barrier2.size;
        bufferBarriers.push_back(barrier);
    }

    std::vector<VkImageMemoryBarrier> imageBarriers{};
    imageBarriers.reserve(barriers.imageBarriers.size());
    for (const VkImageMemoryBarrier2& barrier2 : barriers.imageBarriers) {
        srcStages |= toLegacyStage(barrier2.srcStageMask);
        dstStages |= toLegacyStage(barrier2.dstStageMask);

        VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.srcAccessMask = static_cast<VkAccessFlags>(barrier2.srcAccessMask & 0xFFFFFFFFULL);
        barrier.dstAccessMask = static_cast<VkAccessFlags>(barrier2.dstAccessMask & 0xFFFFFFFFULL);
        barrier.oldLayout = barrier2.oldLayout;
        barrier.newLayout = barrier2.newLayout;
        barrier.srcQueueFamilyIndex = barrier2.srcQueueFamilyIndex;
        barrier.dstQueueFamilyIndex = barrier2.dstQueueFamilyIndex;
        barrier.image = barrier2.image;
        barrier.subresourceRange = barrier2.subresourceRange;
        imageBarriers.push_back(barrier);
    }

    vkCmdPipelineBarrier(
        commandBuffer,
        srcStages != 0 ? srcStages : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        dstStages != 0 ? dstStages : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0,
        static_cast<uint32_t>(memoryBarriers.size()),
        memoryBarriers.empty() ? nullptr : memoryBarriers.data(),
        static_cast<uint32_t>(bufferBarriers.size()),
        bufferBarriers.empty() ? nullptr : bufferBarriers.data(),
        static_cast<uint32_t>(imageBarriers.size()),
        imageBarriers.empty() ? nullptr : imageBarriers.data());
}

struct TransferSubsystem {
    static void record(VkCommandBuffer commandBuffer, const RenderTaskGraph::BarrierBatch& incomingBarriers, const RenderTaskGraph::BarrierBatch& outgoingBarriers, bool useSync2)
    {
        emitBarrierBatch(commandBuffer, incomingBarriers, useSync2);
        emitBarrierBatch(commandBuffer, outgoingBarriers, useSync2);
    }
};

struct ComputeSubsystem {
    static void record(VkCommandBuffer commandBuffer, const RenderTaskGraph::BarrierBatch& incomingBarriers, const RenderTaskGraph::BarrierBatch& outgoingBarriers, bool useSync2)
    {
        emitBarrierBatch(commandBuffer, incomingBarriers, useSync2);
        emitBarrierBatch(commandBuffer, outgoingBarriers, useSync2);
    }
};

struct RenderSubsystem {
    [[nodiscard]] static std::optional<RenderViewPacket> chooseView(const FrameGraphInput& frameGraphInput)
    {
        if (!frameGraphInput.views.empty()) {
            return frameGraphInput.views.front();
        }

        if (!frameGraphInput.drawPackets.empty()) {
            const uint32_t inferredViewId = frameGraphInput.drawPackets.front().viewId;
            return RenderViewPacket{ .viewId = inferredViewId };
        }

        return std::nullopt;
    }

    static void recordSecondary(
        VkCommandBuffer secondary,
        VkPipeline pipeline,
        VkPipelineLayout pipelineLayout,
        VkExtent2D extent,
        const std::vector<DrawPacket>& drawPackets,
        size_t beginIndex,
        size_t endIndex)
    {
        VkViewport viewport{};
        viewport.width = static_cast<float>(extent.width);
        viewport.height = static_cast<float>(extent.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(secondary, 0, 1, &viewport);

        VkRect2D scissor{};
        scissor.extent = extent;
        vkCmdSetScissor(secondary, 0, 1, &scissor);

        vkCmdBindPipeline(secondary, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        for (size_t i = beginIndex; i < endIndex; ++i) {
            const DrawPacket& draw = drawPackets[i];
            vkCmdPushConstants(secondary, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float), &draw.angleRadians);
            vkCmdDraw(secondary, draw.vertexCount, 1, draw.firstVertex, 0);
        }
    }

    static void recordPrimaryWithSecondaries(
        VkCommandBuffer primary,
        SwapchainResources& swapchain,
        uint32_t imageIndex,
        VkRenderPass renderPass,
        const FrameGraphInput& frameGraphInput,
        const RenderTaskGraph::BarrierBatch& incomingBarriers,
        const RenderTaskGraph::BarrierBatch& outgoingBarriers,
        bool useSync2,
        const std::vector<VkCommandBuffer>& secondaryBuffers,
        bool drawImGui)
    {
        emitBarrierBatch(primary, incomingBarriers, useSync2);

        VkExtent2D extent{};
        swapchain.extent(extent);

        VkClearValue clearValues[2]{};
        const std::optional<RenderViewPacket> view = chooseView(frameGraphInput);
        if (view.has_value()) {
            clearValues[0].color = { {
                view->clearColor[0],
                view->clearColor[1],
                view->clearColor[2],
                view->clearColor[3]
            } };
        }
        else {
            clearValues[0].color = { { 0.02f, 0.02f, 0.08f, 1.0f } };
        }
        clearValues[1].depthStencil = { 1.0f, 0 };

        VkRenderPassBeginInfo rpBegin{};
        rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpBegin.renderPass = renderPass;
        rpBegin.framebuffer = swapchain.framebuffer(imageIndex);
        rpBegin.renderArea.offset = { 0, 0 };
        rpBegin.renderArea.extent = extent;
        rpBegin.clearValueCount = 2;
        rpBegin.pClearValues = clearValues;

        VkSubpassContents subpassContents = VK_SUBPASS_CONTENTS_INLINE;
        if (!secondaryBuffers.empty()) {
#if defined(VK_SUBPASS_CONTENTS_INLINE_AND_SECONDARY_COMMAND_BUFFERS)
            subpassContents = drawImGui
                ? VK_SUBPASS_CONTENTS_INLINE_AND_SECONDARY_COMMAND_BUFFERS
                : VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS;
#elif defined(VK_SUBPASS_CONTENTS_INLINE_AND_SECONDARY_COMMAND_BUFFERS_KHR)
            subpassContents = drawImGui
                ? VK_SUBPASS_CONTENTS_INLINE_AND_SECONDARY_COMMAND_BUFFERS_KHR
                : VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS;
#elif defined(VK_SUBPASS_CONTENTS_INLINE_AND_SECONDARY_COMMAND_BUFFERS_EXT)
            subpassContents = drawImGui
                ? VK_SUBPASS_CONTENTS_INLINE_AND_SECONDARY_COMMAND_BUFFERS_EXT
                : VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS;
#else
            if (drawImGui) {
                throw std::runtime_error(
                    "Mixed inline + secondary subpass recording requires Vulkan headers with INLINE_AND_SECONDARY support.");
            }
            subpassContents = VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS;
#endif
        }

        vkCmdBeginRenderPass(primary, &rpBegin, subpassContents);

        if (!secondaryBuffers.empty()) {
            vkCmdExecuteCommands(primary, static_cast<uint32_t>(secondaryBuffers.size()), secondaryBuffers.data());
        }

        if (drawImGui) {
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), primary);
        }

        vkCmdEndRenderPass(primary);
        emitBarrierBatch(primary, outgoingBarriers, useSync2);
    }
};

class EngineRuntime {
public:
    explicit EngineRuntime(const Engine::RunConfig& config)
        : config_(config)
    {
    }

    void run(IGameSimulation& game)
    {
        initWindow();
        try {
            runMainLoop(game);
        }
        catch (...) {
            shutdownWindow();
            throw;
        }
        shutdownWindow();
    }

private:
    void initWindow()
    {
        if (!glfwInit()) {
            throw std::runtime_error("GLFW init failed");
        }

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        window_ = glfwCreateWindow(
            static_cast<int>(config_.windowWidth),
            static_cast<int>(config_.windowHeight),
            config_.windowTitle != nullptr ? config_.windowTitle : "Engine",
            nullptr,
            nullptr);
        if (!window_) {
            glfwTerminate();
            throw std::runtime_error("Window creation failed");
        }
    }

    void shutdownWindow() noexcept
    {
        if (window_ != nullptr) {
            glfwDestroyWindow(window_);
            window_ = nullptr;
        }
        glfwTerminate();
    }

    void runMainLoop(IGameSimulation& game)
    {
        DeviceContext deviceContext(window_, config_.enableValidation);
        SwapchainResources swapchain{};
        swapchain.init(deviceContext, config_.windowWidth, config_.windowHeight);

        VulkanRenderPass renderPass(
            deviceContext.vkDevice(),
            swapchain.colorFormat(),
            swapchain.depthFormat(),
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        swapchain.buildFramebuffers(deviceContext, renderPass.get());

        VkDescriptorPool imguiDescriptorPool = createImGuiDescriptorPool(deviceContext.vkDevice());
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();

        if (!ImGui_ImplGlfw_InitForVulkan(window_, true)) {
            throw std::runtime_error("ImGui_ImplGlfw_InitForVulkan failed");
        }

        ImGui_ImplVulkan_InitInfo imguiInitInfo{};
        imguiInitInfo.Instance = deviceContext.vkInstance();
        imguiInitInfo.PhysicalDevice = deviceContext.vkPhysical();
        imguiInitInfo.Device = deviceContext.vkDevice();
        imguiInitInfo.QueueFamily = deviceContext.graphicsFamilyIndex();
        imguiInitInfo.Queue = deviceContext.graphicsQueue().get();
        imguiInitInfo.DescriptorPool = imguiDescriptorPool;
        imguiInitInfo.RenderPass = renderPass.get();
        imguiInitInfo.MinImageCount = swapchain.imageCount();
        imguiInitInfo.ImageCount = swapchain.imageCount();
        imguiInitInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        imguiInitInfo.CheckVkResultFn = +[](VkResult err) {
            if (err != VK_SUCCESS) {
                vkutil::throwVkError("ImGui Vulkan backend", err);
            }
        };

        if (!ImGui_ImplVulkan_Init(&imguiInitInfo)) {
            throw std::runtime_error("ImGui_ImplVulkan_Init failed");
        }

        ImGui_ImplVulkan_CreateFontsTexture();

        VulkanPipelineLayout pipelineLayout(
            deviceContext.vkDevice(),
            {},
            { VkPushConstantRange{ VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float) } });

        const std::vector<char> vertShaderCode = loadShaderCode(resolveVertexShaderPath(config_));
        const std::vector<char> fragShaderCode = loadShaderCode(resolveFragmentShaderPath(config_));

        VulkanShaderModule vertShader(deviceContext.vkDevice(), vertShaderCode);
        VulkanShaderModule fragShader(deviceContext.vkDevice(), fragShaderCode);

        VkPipelineShaderStageCreateInfo vertexStage{};
        vertexStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vertexStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vertexStage.module = vertShader.get();
        vertexStage.pName = "main";

        VkPipelineShaderStageCreateInfo fragmentStage{};
        fragmentStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        fragmentStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragmentStage.module = fragShader.get();
        fragmentStage.pName = "main";

        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.cullMode = VK_CULL_MODE_NONE;
        rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterizer.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo blendState{};
        blendState.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blendState.attachmentCount = 1;
        blendState.pAttachments = &blendAttachment;

        std::array<VkDynamicState, 2> dynamicStates{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamicState{};
        dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
        dynamicState.pDynamicStates = dynamicStates.data();

        VkGraphicsPipelineCreateInfo pipelineCi{};
        pipelineCi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        std::array<VkPipelineShaderStageCreateInfo, 2> stages{ vertexStage, fragmentStage };
        pipelineCi.stageCount = static_cast<uint32_t>(stages.size());
        pipelineCi.pStages = stages.data();
        pipelineCi.pVertexInputState = &vertexInput;
        pipelineCi.pInputAssemblyState = &inputAssembly;
        pipelineCi.pViewportState = &viewportState;
        pipelineCi.pRasterizationState = &rasterizer;
        pipelineCi.pMultisampleState = &multisample;
        pipelineCi.pDepthStencilState = &depthStencil;
        pipelineCi.pColorBlendState = &blendState;
        pipelineCi.pDynamicState = &dynamicState;

        VulkanPipelineBuildInfo buildInfo{};
        buildInfo.pipelineLayout = pipelineLayout.get();
        buildInfo.renderPass = renderPass.get();

        VulkanPipeline pipeline(deviceContext.vkDevice(), { vertexStage, fragmentStage }, pipelineCi, buildInfo);

        const uint32_t hardwareThreads = std::max<uint32_t>(1u, std::thread::hardware_concurrency());
        const uint32_t graphicsWorkers = std::min<uint32_t>(8u, std::max<uint32_t>(2u, hardwareThreads));

        VulkanCommandArena::Config transferArenaCfg{};
        transferArenaCfg.device = deviceContext.vkDevice();
        transferArenaCfg.queueFamilyIndex = deviceContext.transferFamilyIndex();
        transferArenaCfg.framesInFlight = kFramesInFlight;
        transferArenaCfg.workerThreads = 2;
        transferArenaCfg.preallocatePerFrame = 4;
        VulkanCommandArena transferArena(transferArenaCfg);

        VulkanCommandArena::Config computeArenaCfg{};
        computeArenaCfg.device = deviceContext.vkDevice();
        computeArenaCfg.queueFamilyIndex = deviceContext.computeFamilyIndex();
        computeArenaCfg.framesInFlight = kFramesInFlight;
        computeArenaCfg.workerThreads = 2;
        computeArenaCfg.preallocatePerFrame = 4;
        VulkanCommandArena computeArena(computeArenaCfg);

        VulkanCommandArena::Config graphicsArenaCfg{};
        graphicsArenaCfg.device = deviceContext.vkDevice();
        graphicsArenaCfg.queueFamilyIndex = deviceContext.graphicsFamilyIndex();
        graphicsArenaCfg.framesInFlight = kFramesInFlight;
        graphicsArenaCfg.workerThreads = graphicsWorkers;
        graphicsArenaCfg.preallocatePerFrame = std::max<uint32_t>(8u, graphicsWorkers * 2u);
        VulkanCommandArena graphicsArena(graphicsArenaCfg);
        PersistentWorkerPool graphicsWorkerPool(graphicsWorkers);

        std::array<FrameData, kFramesInFlight> frames{};
        SubmissionScheduler::SchedulerPolicy schedulerPolicy{};
        schedulerPolicy.allowComputeOnGraphicsFallback = false;
        schedulerPolicy.requireDedicatedComputeQueue = false;
        SubmissionScheduler submissionScheduler(deviceContext, schedulerPolicy);
        bool computeFallbackObserved = false;
        for (auto& frame : frames) {
            frame.imageAvailable = VulkanSemaphore(deviceContext.vkDevice());
            frame.inFlight = VulkanFence(deviceContext.vkDevice(), VK_FENCE_CREATE_SIGNALED_BIT);
        }

        std::vector<VulkanSemaphore> presentFinishedByImage =
            createPerImagePresentSemaphores(deviceContext.vkDevice(), swapchain.imageCount());

        uint32_t frameIndex = 0;
        auto previousTick = std::chrono::steady_clock::now();

        while (!glfwWindowShouldClose(window_)) {
            glfwPollEvents();

            const auto now = std::chrono::steady_clock::now();
            const float deltaSeconds = std::chrono::duration<float>(now - previousTick).count();
            previousTick = now;

            ImGui_ImplVulkan_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            game.tick(SimulationFrameInput{
                .deltaSeconds = deltaSeconds,
                .frameIndex = frameIndex
                });
            game.drawMainMenuBar();
            ImGui::Render();

            const FrameGraphInput frameGraphInput = game.buildFrameGraphInput();
            validateFrameGraphInput(frameGraphInput);

            const uint32_t frameSlot = frameIndex % kFramesInFlight;
            FrameData& frame = frames[frameSlot];
            ensure(frame.inFlight.waitResult(), "frameFence.wait");

            const auto transferToken = transferArena.beginFrame(frameSlot, frame.inFlight.get());
            if (!transferToken.hasValue()) {
                vkutil::throwVkError("transferArena.beginFrame", transferToken.error());
            }
            const auto computeToken = computeArena.beginFrame(frameSlot, frame.inFlight.get());
            if (!computeToken.hasValue()) {
                vkutil::throwVkError("computeArena.beginFrame", computeToken.error());
            }
            const auto graphicsToken = graphicsArena.beginFrame(frameSlot, frame.inFlight.get());
            if (!graphicsToken.hasValue()) {
                vkutil::throwVkError("graphicsArena.beginFrame", graphicsToken.error());
            }

            ensure(frame.inFlight.resetResult(), "frameFence.reset");

            uint32_t imageIndex = 0;
            const VkResult acquireResult = vkAcquireNextImageKHR(
                deviceContext.vkDevice(),
                swapchain.swapchain().get(),
                UINT64_MAX,
                frame.imageAvailable.get(),
                VK_NULL_HANDLE,
                &imageIndex);

            if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR || acquireResult == VK_SUBOPTIMAL_KHR) {
                int fbWidth = 0;
                int fbHeight = 0;
                glfwGetFramebufferSize(window_, &fbWidth, &fbHeight);
                if (fbWidth > 0 && fbHeight > 0) {
                    SwapchainGarbage garbage{};
                    swapchain.recreateBase(deviceContext, static_cast<uint32_t>(fbWidth), static_cast<uint32_t>(fbHeight), garbage);
                    swapchain.buildFramebuffers(deviceContext, renderPass.get());
                    presentFinishedByImage = createPerImagePresentSemaphores(deviceContext.vkDevice(), swapchain.imageCount());
                    ImGui_ImplVulkan_SetMinImageCount(swapchain.imageCount());
                }
                continue;
            }
            if (acquireResult != VK_SUCCESS) {
                vkutil::throwVkError("vkAcquireNextImageKHR", acquireResult);
            }

            RenderTaskGraph graph{};
            const RenderTaskGraph::ResourceId transferOutResource = graph.createResource();
            const RenderTaskGraph::ResourceId computeOutResource = graph.createResource();
            const VkImage swapchainImage = swapchain.swapchain().getImages().at(imageIndex);
            VkImageSubresourceRange swapchainColorRange{};
            swapchainColorRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            swapchainColorRange.baseMipLevel = 0;
            swapchainColorRange.levelCount = 1;
            swapchainColorRange.baseArrayLayer = 0;
            swapchainColorRange.layerCount = 1;

            const RenderTaskGraph::ResourceId colorResource = graph.createImageResource(
                swapchainImage,
                swapchainColorRange,
                VK_IMAGE_LAYOUT_UNDEFINED,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                VK_ACCESS_2_NONE,
                deviceContext.graphicsFamilyIndex());
            const bool useSync2 = deviceContext.isFeatureEnabledSynchronization2();

            std::optional<VulkanCommandArena::BorrowedCommandBuffer> transferPrimary{};
            std::optional<VulkanCommandArena::BorrowedCommandBuffer> computePrimary{};
            std::optional<VulkanCommandArena::BorrowedCommandBuffer> graphicsPrimary{};

            if (frameGraphInput.runTransferStage) {
                auto borrowed = transferArena.acquirePrimary(transferToken.value(), 0, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
                if (!borrowed.hasValue()) {
                    vkutil::throwVkError("transferArena.acquirePrimary", borrowed.error());
                }
                transferPrimary = borrowed.value();
            }

            if (frameGraphInput.runComputeStage) {
                auto borrowed = computeArena.acquirePrimary(computeToken.value(), 0, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
                if (!borrowed.hasValue()) {
                    vkutil::throwVkError("computeArena.acquirePrimary", borrowed.error());
                }
                computePrimary = borrowed.value();
            }

            {
                auto borrowed = graphicsArena.acquirePrimary(graphicsToken.value(), 0, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
                if (!borrowed.hasValue()) {
                    vkutil::throwVkError("graphicsArena.acquirePrimary", borrowed.error());
                }
                graphicsPrimary = borrowed.value();
            }

            if (frameGraphInput.runTransferStage) {
                const auto transferPassId = graph.addPass(RenderTaskGraph::PassNode{
                    .job = SubmissionScheduler::JobRequest{
                        .queueClass = SubmissionScheduler::QueueClass::Transfer,
                        .commandBuffers = { transferPrimary->handle },
                        .debugLabel = "transfer.prepare"
                    },
                    .usages = {
                        RenderTaskGraph::ResourceUsage{
                            .resource = transferOutResource,
                            .access = RenderTaskGraph::ResourceAccessType::Write,
                            .stageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                            .accessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                            .queueFamilyIndex = deviceContext.transferFamilyIndex()
                        }
                    },
                    .record = [&](const RenderTaskGraph::BarrierBatch& incomingBarriers, const RenderTaskGraph::BarrierBatch& outgoingBarriers) {
                        TransferSubsystem::record(transferPrimary->handle, incomingBarriers, outgoingBarriers, useSync2);
                        return transferArena.endBorrowed(*transferPrimary);
                    }
                    });
                (void)transferPassId;
            }

            if (frameGraphInput.runComputeStage) {
                std::vector<RenderTaskGraph::ResourceUsage> computeUsages{};
                if (frameGraphInput.runTransferStage) {
                    computeUsages.push_back(RenderTaskGraph::ResourceUsage{
                        .resource = transferOutResource,
                        .access = RenderTaskGraph::ResourceAccessType::Read,
                        .stageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        .accessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                        .queueFamilyIndex = deviceContext.computeFamilyIndex()
                        });
                }
                computeUsages.push_back(RenderTaskGraph::ResourceUsage{
                    .resource = computeOutResource,
                    .access = RenderTaskGraph::ResourceAccessType::Write,
                    .stageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    .accessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    .queueFamilyIndex = deviceContext.computeFamilyIndex()
                    });

                const auto computePassId = graph.addPass(RenderTaskGraph::PassNode{
                    .job = SubmissionScheduler::JobRequest{
                        .queueClass = SubmissionScheduler::QueueClass::Compute,
                        .commandBuffers = { computePrimary->handle },
                        .debugLabel = "compute.simulate"
                    },
                    .usages = std::move(computeUsages),
                    .record = [&](const RenderTaskGraph::BarrierBatch& incomingBarriers, const RenderTaskGraph::BarrierBatch& outgoingBarriers) {
                        ComputeSubsystem::record(computePrimary->handle, incomingBarriers, outgoingBarriers, useSync2);
                        return computeArena.endBorrowed(*computePrimary);
                    }
                    });
                (void)computePassId;
            }

            std::vector<RenderTaskGraph::ResourceUsage> graphicsUsages{};
            if (frameGraphInput.runComputeStage) {
                graphicsUsages.push_back(RenderTaskGraph::ResourceUsage{
                    .resource = computeOutResource,
                    .access = RenderTaskGraph::ResourceAccessType::Read,
                    .stageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                    .accessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                    .queueFamilyIndex = deviceContext.graphicsFamilyIndex()
                    });
            }
            else if (frameGraphInput.runTransferStage) {
                graphicsUsages.push_back(RenderTaskGraph::ResourceUsage{
                    .resource = transferOutResource,
                    .access = RenderTaskGraph::ResourceAccessType::Read,
                    .stageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                    .accessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                    .queueFamilyIndex = deviceContext.graphicsFamilyIndex()
                    });
            }
            graphicsUsages.push_back(RenderTaskGraph::ResourceUsage{
                .resource = colorResource,
                .access = RenderTaskGraph::ResourceAccessType::Write,
                .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                .accessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .queueFamilyIndex = deviceContext.graphicsFamilyIndex()
                });

            const auto graphicsPassId = graph.addPass(RenderTaskGraph::PassNode{
                .job = SubmissionScheduler::JobRequest{
                    .queueClass = SubmissionScheduler::QueueClass::Graphics,
                    .commandBuffers = { graphicsPrimary->handle },
                    .waitSemaphores = { frame.imageAvailable.get() },
                    .waitStages = { VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT },
                    .signalSemaphores = { presentFinishedByImage[imageIndex].get() },
                    .fence = frame.inFlight.get(),
                    .debugLabel = "graphics.render"
                },
                .usages = std::move(graphicsUsages),
                .record = [&](const RenderTaskGraph::BarrierBatch& incomingBarriers, const RenderTaskGraph::BarrierBatch& outgoingBarriers) {
                    const size_t totalDraws = frameGraphInput.drawPackets.size();
                    const size_t requestedWorkers = static_cast<size_t>(std::max<uint32_t>(1u, graphicsWorkers));
                    const size_t workerCountSz = std::min(requestedWorkers, std::max<size_t>(1, totalDraws));
                    const uint32_t workerCount = static_cast<uint32_t>(workerCountSz);

                    std::vector<VkCommandBuffer> secondaries{};
                    secondaries.resize(workerCount);

                    VkExtent2D extent{};
                    swapchain.extent(extent);

                    std::mutex errorMutex{};
                    std::optional<vkutil::VkErrorContext> firstError{};

                    VkCommandBufferInheritanceInfo inheritance{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO };
                    inheritance.renderPass = renderPass.get();
                    inheritance.subpass = 0;
                    inheritance.framebuffer = swapchain.framebuffer(imageIndex);

                    graphicsWorkerPool.run(workerCount, [&](uint32_t w) {
                        const size_t begin = (totalDraws * w) / workerCount;
                        const size_t end = (totalDraws * (w + 1u)) / workerCount;

                        auto borrowed = graphicsArena.acquireSecondary(
                            graphicsToken.value(),
                            inheritance,
                            w,
                            VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                            VulkanCommandArena::SecondaryRecordingMode::LegacyRenderPass);
                        if (!borrowed.hasValue()) {
                            std::scoped_lock lock(errorMutex);
                            if (!firstError.has_value()) {
                                firstError = borrowed.context();
                            }
                            return;
                        }

                        RenderSubsystem::recordSecondary(
                            borrowed.value().handle,
                            pipeline.get(),
                            pipelineLayout.get(),
                            extent,
                            frameGraphInput.drawPackets,
                            begin,
                            end);

                        auto endResult = graphicsArena.endBorrowed(borrowed.value());
                        if (!endResult.hasValue()) {
                            std::scoped_lock lock(errorMutex);
                            if (!firstError.has_value()) {
                                firstError = endResult.context();
                            }
                            return;
                        }

                        secondaries[w] = borrowed.value().handle;
                    });

                    if (firstError.has_value()) {
                        return vkutil::VkExpected<void>(firstError.value());
                    }

                    RenderSubsystem::recordPrimaryWithSecondaries(
                        graphicsPrimary->handle,
                        swapchain,
                        imageIndex,
                        renderPass.get(),
                        frameGraphInput,
                        incomingBarriers,
                        outgoingBarriers,
                        useSync2,
                        secondaries,
                        true);

                    return graphicsArena.endBorrowed(*graphicsPrimary);
                }
                });
            (void)graphicsPassId;

            graph.setPresent(SubmissionScheduler::PresentRequest{
                .swapchain = swapchain.swapchain().get(),
                .imageIndex = imageIndex,
                .waitSemaphores = { presentFinishedByImage[imageIndex].get() }
                });

            const auto frameExecution = graph.execute(submissionScheduler);
            if (!frameExecution.hasValue()) {
                vkutil::throwVkError("RenderTaskGraph::execute", frameExecution.error());
            }

            if (frameExecution.value().usedComputeToGraphicsFallback && !computeFallbackObserved) {
                computeFallbackObserved = true;
                std::cerr << "SubmissionScheduler: compute submissions are using explicit graphics fallback" << std::endl;
            }

            const VkResult presentResult = frameExecution.value().presentResult;

            if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR) {
                int fbWidth = 0;
                int fbHeight = 0;
                glfwGetFramebufferSize(window_, &fbWidth, &fbHeight);
                SwapchainGarbage garbage{};
                if (fbWidth > 0 && fbHeight > 0) {
                    swapchain.recreateBase(deviceContext, static_cast<uint32_t>(fbWidth), static_cast<uint32_t>(fbHeight), garbage);
                    swapchain.buildFramebuffers(deviceContext, renderPass.get());
                    presentFinishedByImage = createPerImagePresentSemaphores(deviceContext.vkDevice(), swapchain.imageCount());
                    ImGui_ImplVulkan_SetMinImageCount(swapchain.imageCount());
                }
            }
            else if (presentResult != VK_SUCCESS) {
                vkutil::throwVkError("vkQueuePresentKHR", presentResult);
            }

            ++frameIndex;
        }

        if (!deviceContext.waitDeviceIdle()) {
            throw std::runtime_error("waitDeviceIdle failed");
        }

        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        vkDestroyDescriptorPool(deviceContext.vkDevice(), imguiDescriptorPool, nullptr);
    }

    Engine::RunConfig config_{};
    GLFWwindow* window_{ nullptr };
};

} // namespace

void Engine::run(IGameSimulation& game, const RunConfig& config)
{
    EngineRuntime runtime(config);
    runtime.run(game);
}
