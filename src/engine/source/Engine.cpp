#include <Engine.h>

#include <vulkan/DeviceContext.h>
#include <vulkan/PersistentResourceService.h>
#include <vulkan/RenderGraph.h>
#include <vulkan/SubmissionScheduler.h>
#include <vulkan/SwapchainResources.h>
#include <vulkan/VkCommands.h>
#include <vulkan/VkBuffer.h>
#include <vulkan/VkPipeline.h>
#include <vulkan/VkShaderModule.h>
#include <vulkan/VkSync.h>
#include <vulkan/VkUtils.h>
#include <renderer/RuntimeAssetService.h>
#include <renderer/SnapshotRing.h>

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <atomic>
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
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {
constexpr uint32_t kFramesInFlight = 2;

struct FrameData {
    VulkanSemaphore imageAvailable{};
    VulkanFence inFlight{};
};



template <typename Fn>
class ScopeExit {
public:
    explicit ScopeExit(Fn fn)
        : fn_(std::move(fn))
    {
    }

    ~ScopeExit()
    {
        if (active_) {
            fn_();
        }
    }

    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

    ScopeExit(ScopeExit&& other) noexcept
        : fn_(std::move(other.fn_))
        , active_(other.active_)
    {
        other.active_ = false;
    }

    void release() noexcept { active_ = false; }

private:
    Fn fn_;
    bool active_{ true };
};

template <typename Fn>
ScopeExit<Fn> makeScopeExit(Fn fn)
{
    return ScopeExit<Fn>(std::move(fn));
}

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


struct RendererIngestionDiagnostics {
    uint32_t invalidInstances{ 0 };
    uint32_t droppedDraws{ 0 };
    uint32_t invalidLights{ 0 };
    uint32_t unresolvedMeshes{ 0 };
    uint32_t unresolvedMaterials{ 0 };
    uint32_t staleMeshHandles{ 0 };
    uint32_t staleMaterialHandles{ 0 };
    uint32_t loadingMeshes{ 0 };
    uint32_t loadingMaterials{ 0 };
    uint32_t failedMeshes{ 0 };
    uint32_t failedMaterials{ 0 };
};

struct RenderAssetRegistry {
    enum class HandleState : uint8_t {
        Valid,
        Missing,
        Stale,
        Loading,
        Failed
    };

    explicit RenderAssetRegistry(RuntimeAssetService& service)
        : service_(service)
    {
    }

    [[nodiscard]] HandleState resolveMesh(uint32_t meshId, uint32_t vertexCount, uint32_t firstVertex) const
    {
        const auto mesh = service_.resolveMesh(meshId);
        if (!mesh.has_value()) {
            return HandleState::Missing;
        }
        if (mesh->residency == RuntimeAssetBackend::Residency::Loading) {
            return HandleState::Loading;
        }
        if (mesh->residency == RuntimeAssetBackend::Residency::Failed) {
            return HandleState::Failed;
        }
        if (mesh->residency == RuntimeAssetBackend::Residency::Evicted) {
            return HandleState::Missing;
        }
        if (mesh->vertexCount != vertexCount || mesh->firstVertex != firstVertex) {
            return HandleState::Stale;
        }
        return HandleState::Valid;
    }

    [[nodiscard]] HandleState resolveMaterial(uint32_t materialId) const
    {
        const auto material = service_.resolveMaterial(materialId);
        if (!material.has_value()) {
            return HandleState::Missing;
        }
        if (material->residency == RuntimeAssetBackend::Residency::Loading) {
            return HandleState::Loading;
        }
        if (material->residency == RuntimeAssetBackend::Residency::Failed) {
            return HandleState::Failed;
        }
        if (material->residency == RuntimeAssetBackend::Residency::Evicted) {
            return HandleState::Missing;
        }
        return HandleState::Valid;
    }

private:
    RuntimeAssetService& service_;
};

struct RendererCapabilityProfile {
    bool dedicatedTransferQueue{ false };
    bool dedicatedComputeQueue{ false };
};

enum class PlanResourceId : uint8_t {
    TransferPayload,
    ComputePayload,
    LightPayload,
    PersistentSceneBuffer,
    ColorTarget
};

enum class ResourceLifetime : uint8_t {
    Transient,
    Persistent
};

struct PlanResourceRequirement {
    PlanResourceId id{ PlanResourceId::ColorTarget };
    ResourceLifetime lifetime{ ResourceLifetime::Transient };
};

struct PlanResourceUsage {
    PlanResourceId resource{ PlanResourceId::ColorTarget };
    RenderTaskGraph::ResourceAccessType access{ RenderTaskGraph::ResourceAccessType::Read };
    VkPipelineStageFlags2 stageMask{ VK_PIPELINE_STAGE_2_NONE };
    VkAccessFlags2 accessMask{ VK_ACCESS_2_NONE };
    VkImageLayout imageLayout{ VK_IMAGE_LAYOUT_UNDEFINED };
};

struct RenderDrawCommand {
    uint32_t vertexCount{ 3 };
    uint32_t firstVertex{ 0 };
    float angleRadians{ 0.0F };
};

struct RendererPlan {
    struct PassPlan {
        enum class Kind : uint8_t {
            Transfer,
            Compute,
            Graphics
        };

        Kind kind{ Kind::Graphics };
        SubmissionScheduler::QueueClass queueClass{ SubmissionScheduler::QueueClass::Graphics };
        const char* name{ "" };
        std::vector<uint32_t> dependsOnPassNodes{};
        uint32_t nodeId{ 0 };
        std::vector<PlanResourceUsage> usages{};
    };

    uint64_t simulationFrameIndex{ 0 };
    RenderViewSnapshot activeView{};
    std::vector<RenderDrawCommand> drawCommands{};
    std::vector<RenderMaterialGroupSnapshot> materialGroups{};
    std::vector<PlanResourceRequirement> resources{};
    std::vector<PassPlan> activePasses{};

    [[nodiscard]] bool hasPass(PassPlan::Kind kind) const
    {
        return std::any_of(activePasses.begin(), activePasses.end(), [kind](const PassPlan& pass) {
            return pass.kind == kind;
        });
    }
};

struct ValidatedRenderSnapshot {
    RenderWorldSnapshot snapshot{};
    RendererIngestionDiagnostics diagnostics{};
};

ValidatedRenderSnapshot validateRenderSnapshot(RenderWorldSnapshot input, const RenderAssetRegistry& assetRegistry)
{
    ValidatedRenderSnapshot validated{};
    validated.snapshot.simulationFrameIndex = input.simulationFrameIndex;

    std::unordered_set<uint32_t> viewIds{};
    for (RenderViewSnapshot view : input.views) {
        for (float& c : view.clearColor) {
            if (!std::isfinite(c)) {
                c = 0.0F;
            }
        }
        if (viewIds.insert(view.viewId).second) {
            validated.snapshot.views.push_back(view);
        }
    }

    std::unordered_set<uint64_t> seenInstances{};
    for (RenderInstanceSnapshot instance : input.instances) {
        bool valid = true;
        if (instance.mesh.id == 0 || instance.material.id == 0 || instance.mesh.vertexCount == 0) {
            valid = false;
        }

        const auto meshState = assetRegistry.resolveMesh(instance.mesh.id, instance.mesh.vertexCount, instance.mesh.firstVertex);
        if (meshState == RenderAssetRegistry::HandleState::Missing) {
            validated.diagnostics.unresolvedMeshes += 1;
            valid = false;
        }
        else if (meshState == RenderAssetRegistry::HandleState::Stale) {
            validated.diagnostics.staleMeshHandles += 1;
            valid = false;
        }
        else if (meshState == RenderAssetRegistry::HandleState::Loading) {
            validated.diagnostics.loadingMeshes += 1;
            valid = false;
        }
        else if (meshState == RenderAssetRegistry::HandleState::Failed) {
            validated.diagnostics.failedMeshes += 1;
            valid = false;
        }

        const auto materialState = assetRegistry.resolveMaterial(instance.material.id);
        if (materialState == RenderAssetRegistry::HandleState::Missing) {
            validated.diagnostics.unresolvedMaterials += 1;
            valid = false;
        }
        else if (materialState == RenderAssetRegistry::HandleState::Stale) {
            validated.diagnostics.staleMaterialHandles += 1;
            valid = false;
        }
        else if (materialState == RenderAssetRegistry::HandleState::Loading) {
            validated.diagnostics.loadingMaterials += 1;
            valid = false;
        }
        else if (materialState == RenderAssetRegistry::HandleState::Failed) {
            validated.diagnostics.failedMaterials += 1;
            valid = false;
        }
        if (!viewIds.empty() && !viewIds.contains(instance.viewId)) {
            valid = false;
        }

        bool allZero = true;
        for (float& value : instance.localToWorld) {
            if (!std::isfinite(value)) {
                valid = false;
                break;
            }
            if (value != 0.0F) {
                allZero = false;
            }
            if (value == -0.0F) {
                value = 0.0F;
            }
        }
        if (allZero) {
            valid = false;
        }

        if (instance.worldBounds.has_value()) {
            bool validBounds = std::isfinite(instance.worldBounds->radius) && instance.worldBounds->radius >= 0.0F;
            for (float& value : instance.worldBounds->center) {
                if (!std::isfinite(value)) {
                    validBounds = false;
                    break;
                }
                if (value == -0.0F) {
                    value = 0.0F;
                }
            }
            if (!validBounds) {
                valid = false;
                instance.worldBounds.reset();
            }
        }

        if (!seenInstances.insert(instance.instanceId).second) {
            valid = false;
        }

        if (!valid) {
            validated.diagnostics.invalidInstances += 1;
            validated.diagnostics.droppedDraws += 1;
            continue;
        }
        validated.snapshot.instances.push_back(instance);
    }

    for (const RenderMaterialGroupSnapshot& group : input.materialGroups) {
        const uint64_t end = static_cast<uint64_t>(group.firstInstance) + static_cast<uint64_t>(group.instanceCount);
        if (group.materialId == 0 || end > validated.snapshot.instances.size()) {
            continue;
        }
        validated.snapshot.materialGroups.push_back(group);
    }

    for (RenderLightSnapshot light : input.lights) {
        bool valid = std::isfinite(light.intensity);
        for (float& p : light.worldPosition) {
            if (!std::isfinite(p)) {
                valid = false;
                break;
            }
        }
        if (!valid) {
            validated.diagnostics.invalidLights += 1;
            continue;
        }
        validated.snapshot.lights.push_back(light);
    }

    return validated;
}

RendererPlan compileRendererPlan(const ValidatedRenderSnapshot& validated, const RendererCapabilityProfile& capabilities)
{
    RendererPlan plan{};
    plan.simulationFrameIndex = validated.snapshot.simulationFrameIndex;
    if (!validated.snapshot.views.empty()) {
        plan.activeView = validated.snapshot.views.front();
    }

    plan.materialGroups = validated.snapshot.materialGroups;
    plan.resources.push_back(PlanResourceRequirement{ .id = PlanResourceId::TransferPayload, .lifetime = ResourceLifetime::Transient });
    plan.resources.push_back(PlanResourceRequirement{ .id = PlanResourceId::ComputePayload, .lifetime = ResourceLifetime::Transient });
    plan.resources.push_back(PlanResourceRequirement{ .id = PlanResourceId::LightPayload, .lifetime = ResourceLifetime::Transient });
    plan.resources.push_back(PlanResourceRequirement{ .id = PlanResourceId::PersistentSceneBuffer, .lifetime = ResourceLifetime::Persistent });
    plan.resources.push_back(PlanResourceRequirement{ .id = PlanResourceId::ColorTarget, .lifetime = ResourceLifetime::Persistent });

    plan.drawCommands.reserve(validated.snapshot.instances.size());
    for (const RenderInstanceSnapshot& instance : validated.snapshot.instances) {
        const float angleRadians = std::atan2(instance.localToWorld[4], instance.localToWorld[0]);
        plan.drawCommands.push_back(RenderDrawCommand{
            .vertexCount = instance.mesh.vertexCount,
            .firstVertex = instance.mesh.firstVertex,
            .angleRadians = angleRadians
        });
    }

    const bool hasDrawWork = !plan.drawCommands.empty();
    const bool hasLightWork = !validated.snapshot.lights.empty();
    const bool preferTransfer = capabilities.dedicatedTransferQueue;
    const bool preferCompute = capabilities.dedicatedComputeQueue;

    uint32_t nextPassNodeId = 1;

    auto addPass = [&](RendererPlan::PassPlan pass) -> uint32_t {
        if (pass.nodeId == 0) {
            pass.nodeId = nextPassNodeId++;
        }
        const uint32_t nodeId = pass.nodeId;
        plan.activePasses.push_back(std::move(pass));
        return nodeId;
    };

    std::optional<uint32_t> transferNodeId{};
    if (hasDrawWork && preferTransfer) {
        RendererPlan::PassPlan transfer{};
        transfer.kind = RendererPlan::PassPlan::Kind::Transfer;
        transfer.queueClass = SubmissionScheduler::QueueClass::Transfer;
        transfer.name = "transfer.upload";
        transfer.usages.push_back(PlanResourceUsage{
            .resource = PlanResourceId::TransferPayload,
            .access = RenderTaskGraph::ResourceAccessType::Write,
            .stageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .accessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT
        });
        transferNodeId = addPass(std::move(transfer));
    }

    std::optional<uint32_t> simulationNodeId{};
    if (hasDrawWork && preferCompute) {
        RendererPlan::PassPlan computeSimulation{};
        computeSimulation.kind = RendererPlan::PassPlan::Kind::Compute;
        computeSimulation.queueClass = SubmissionScheduler::QueueClass::Compute;
        computeSimulation.name = "compute.simulate";
        if (transferNodeId.has_value()) {
            computeSimulation.dependsOnPassNodes.push_back(*transferNodeId);
            computeSimulation.usages.push_back(PlanResourceUsage{
                .resource = PlanResourceId::TransferPayload,
                .access = RenderTaskGraph::ResourceAccessType::Read,
                .stageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                .accessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT
            });
        }
        computeSimulation.usages.push_back(PlanResourceUsage{
            .resource = PlanResourceId::ComputePayload,
            .access = RenderTaskGraph::ResourceAccessType::Write,
            .stageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .accessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
        });
        simulationNodeId = addPass(std::move(computeSimulation));
    }

    std::optional<uint32_t> lightingNodeId{};
    if (hasLightWork) {
        RendererPlan::PassPlan computeLighting{};
        computeLighting.kind = preferCompute ? RendererPlan::PassPlan::Kind::Compute : RendererPlan::PassPlan::Kind::Graphics;
        computeLighting.queueClass = preferCompute ? SubmissionScheduler::QueueClass::Compute : SubmissionScheduler::QueueClass::Graphics;
        computeLighting.name = "lighting.evaluate";
        if (transferNodeId.has_value()) {
            computeLighting.dependsOnPassNodes.push_back(*transferNodeId);
            computeLighting.usages.push_back(PlanResourceUsage{
                .resource = PlanResourceId::TransferPayload,
                .access = RenderTaskGraph::ResourceAccessType::Read,
                .stageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                .accessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT
            });
        }
        computeLighting.usages.push_back(PlanResourceUsage{
            .resource = PlanResourceId::LightPayload,
            .access = RenderTaskGraph::ResourceAccessType::Write,
            .stageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .accessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
        });
        lightingNodeId = addPass(std::move(computeLighting));
    }

    RendererPlan::PassPlan graphicsPass{};
    graphicsPass.kind = RendererPlan::PassPlan::Kind::Graphics;
    graphicsPass.queueClass = SubmissionScheduler::QueueClass::Graphics;
    graphicsPass.name = "graphics.render";
    graphicsPass.usages.push_back(PlanResourceUsage{
        .resource = PlanResourceId::PersistentSceneBuffer,
        .access = RenderTaskGraph::ResourceAccessType::Read,
        .stageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
        .accessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT
    });
    if (simulationNodeId.has_value()) {
        graphicsPass.usages.push_back(PlanResourceUsage{
            .resource = PlanResourceId::ComputePayload,
            .access = RenderTaskGraph::ResourceAccessType::Read,
            .stageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
            .accessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT
        });
    }
    else if (transferNodeId.has_value()) {
        graphicsPass.usages.push_back(PlanResourceUsage{
            .resource = PlanResourceId::TransferPayload,
            .access = RenderTaskGraph::ResourceAccessType::Read,
            .stageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
            .accessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT
        });
    }
    if (lightingNodeId.has_value()) {
        graphicsPass.usages.push_back(PlanResourceUsage{
            .resource = PlanResourceId::LightPayload,
            .access = RenderTaskGraph::ResourceAccessType::Read,
            .stageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .accessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT
        });
    }
    graphicsPass.usages.push_back(PlanResourceUsage{
        .resource = PlanResourceId::ColorTarget,
        .access = RenderTaskGraph::ResourceAccessType::Write,
        .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .accessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
    });
    addPass(std::move(graphicsPass));

    auto isWrite = [](RenderTaskGraph::ResourceAccessType access) {
        return access == RenderTaskGraph::ResourceAccessType::Write || access == RenderTaskGraph::ResourceAccessType::ReadWrite;
    };

    std::unordered_set<uint64_t> dedup{};
    for (RendererPlan::PassPlan& consumer : plan.activePasses) {
        for (const PlanResourceUsage& consumerUsage : consumer.usages) {
            if (isWrite(consumerUsage.access)) {
                continue;
            }
            for (const RendererPlan::PassPlan& producer : plan.activePasses) {
                if (producer.nodeId == consumer.nodeId) {
                    continue;
                }
                const bool producerWrites = std::any_of(producer.usages.begin(), producer.usages.end(), [&](const PlanResourceUsage& producerUsage) {
                    return producerUsage.resource == consumerUsage.resource && isWrite(producerUsage.access);
                });
                if (!producerWrites) {
                    continue;
                }

                const uint64_t edgeKey = (static_cast<uint64_t>(producer.nodeId) << 32u) | static_cast<uint64_t>(consumer.nodeId);
                if (dedup.insert(edgeKey).second) {
                    consumer.dependsOnPassNodes.push_back(producer.nodeId);
                }
            }
        }
    }

    return plan;
}

struct ResourceAllocationRegistry {
    struct FrameStats {
        uint32_t transientAllocated{ 0 };
        uint32_t persistentReused{ 0 };
        uint32_t persistentDiscovered{ 0 };
    };

    using PersistentHandle = PersistentResourceService::Handle;

    explicit ResourceAllocationRegistry(PersistentResourceService& persistentService, GpuAllocator* gpuAllocator)
        : persistentService_(persistentService), gpuAllocator_(gpuAllocator)
    {
    }

    void beginFrame() noexcept
    {
        frameStats_ = {};
        transientSeen_.clear();
    }

    void syncPersistentColorTargets(const SwapchainResources& swapchain)
    {
        std::unordered_set<PersistentHandle> liveHandles{};
        liveHandles.reserve(swapchain.imageCount());

        VkImageSubresourceRange colorRange{};
        colorRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        colorRange.baseMipLevel = 0;
        colorRange.levelCount = 1;
        colorRange.baseArrayLayer = 0;
        colorRange.layerCount = 1;

        const auto& images = swapchain.swapchain().getImages();
        for (uint32_t i = 0; i < images.size(); ++i) {
            const PersistentHandle handle = static_cast<PersistentHandle>(i + 1u);
            liveHandles.insert(handle);
            persistentService_.upsertImage(handle, images[i], colorRange);
        }

        for (const PersistentHandle handle : activePersistentHandles_) {
            if (!liveHandles.contains(handle)) {
                persistentService_.removeImage(handle);
            }
        }

        activePersistentHandles_ = std::move(liveHandles);
    }

    void recreateOwnedResourcesOnSwapchainEvent()
    {
        for (const PersistentHandle handle : ownedPersistentBufferHandles_) {
            if (!persistentService_.recreateOwnedBuffer(handle)) {
                throw std::runtime_error("Failed to recreate owned persistent buffer");
            }
        }
    }

    RenderTaskGraph::ResourceId allocateTransient(
        RenderTaskGraph& graph,
        PlanResourceId id)
    {
        if (transientSeen_.insert(id).second) {
            frameStats_.transientAllocated += 1;
        }
        return graph.createResource();
    }

    RenderTaskGraph::ResourceId allocatePersistentColorTarget(
        RenderTaskGraph& graph,
        uint64_t imageKey,
        uint32_t queueFamily)
    {
        const PersistentHandle handle = static_cast<PersistentHandle>(imageKey + 1u);
        if (knownPersistentHandles_.insert(handle).second) {
            frameStats_.persistentDiscovered += 1;
        }
        else {
            frameStats_.persistentReused += 1;
        }

        const auto binding = persistentService_.resolveImage(handle);
        if (!binding.has_value()) {
            throw std::runtime_error("Persistent color target handle is unresolved");
        }

        return graph.createImageResource(
            binding->image,
            binding->subresourceRange,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
            VK_ACCESS_2_NONE,
            queueFamily);
    }

    RenderTaskGraph::ResourceId allocatePersistentSceneBuffer(
        RenderTaskGraph& graph,
        uint32_t queueFamily)
    {
        constexpr PersistentHandle kSceneBufferHandle = 0x5000u;
        if (knownPersistentHandles_.insert(kSceneBufferHandle).second) {
            frameStats_.persistentDiscovered += 1;
        }
        else {
            frameStats_.persistentReused += 1;
        }

        if (ownedPersistentBufferHandles_.insert(kSceneBufferHandle).second) {
            const bool created = persistentService_.ensureOwnedBuffer(kSceneBufferHandle, PersistentResourceService::OwnedBufferSpec{
                .create = [this, handle = kSceneBufferHandle]() -> std::optional<PersistentResourceService::BufferBinding> {
                    if (gpuAllocator_ == nullptr) {
                        return std::nullopt;
                    }

                    auto buffer = VulkanBuffer(
                        *gpuAllocator_,
                        4096,
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                        false,
                        VulkanBuffer::AllocationPolicy::DeviceLocal,
                        {});

                    if (!buffer.valid()) {
                        return std::nullopt;
                    }

                    const VkBuffer vkBuffer = buffer.get();
                    const VkDeviceSize size = buffer.getSize();
                    persistentBuffers_.insert_or_assign(handle, std::move(buffer));
                    return PersistentResourceService::BufferBinding{
                        .buffer = vkBuffer,
                        .offset = 0,
                        .size = size
                    };
                },
                .destroy = [this, handle = kSceneBufferHandle](const PersistentResourceService::BufferBinding&) {
                    persistentBuffers_.erase(handle);
                }
            });

            if (!created) {
                throw std::runtime_error("Failed to create owned persistent scene buffer");
            }
        }

        const auto binding = persistentService_.resolveBuffer(kSceneBufferHandle);
        if (!binding.has_value()) {
            throw std::runtime_error("Persistent scene buffer handle is unresolved");
        }

        return graph.createBufferResource(
            binding->buffer,
            binding->offset,
            binding->size,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
            VK_ACCESS_2_NONE,
            queueFamily);
    }
    [[nodiscard]] const FrameStats& frameStats() const noexcept { return frameStats_; }

private:
    PersistentResourceService& persistentService_;
    GpuAllocator* gpuAllocator_{ nullptr };
    FrameStats frameStats_{};
    std::unordered_set<PlanResourceId> transientSeen_{};
    std::unordered_set<PersistentHandle> knownPersistentHandles_{};
    std::unordered_set<PersistentHandle> activePersistentHandles_{};
    std::unordered_set<PersistentHandle> ownedPersistentBufferHandles_{};
    std::unordered_map<PersistentHandle, VulkanBuffer> persistentBuffers_{};
};

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
    [[nodiscard]] static RenderViewSnapshot chooseView(const RendererPlan& plan)
    {
        return plan.activeView;
    }

    static void recordSecondary(
        VkCommandBuffer secondary,
        VkPipeline pipeline,
        VkPipelineLayout pipelineLayout,
        VkExtent2D extent,
        const std::vector<RenderDrawCommand>& drawPackets,
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
            const RenderDrawCommand& draw = drawPackets[i];
            vkCmdPushConstants(secondary, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float), &draw.angleRadians);
            vkCmdDraw(secondary, draw.vertexCount, 1, draw.firstVertex, 0);
        }
    }

    static void recordPrimaryWithSecondaries(
        VkCommandBuffer primary,
        SwapchainResources& swapchain,
        uint32_t imageIndex,
        VkRenderPass renderPass,
        const RendererPlan& plan,
        const RenderTaskGraph::BarrierBatch& incomingBarriers,
        const RenderTaskGraph::BarrierBatch& outgoingBarriers,
        bool useSync2,
        const std::vector<VkCommandBuffer>& secondaryBuffers)
    {
        emitBarrierBatch(primary, incomingBarriers, useSync2);

        VkExtent2D extent{};
        swapchain.extent(extent);

        VkClearValue clearValues[2]{};
                const RenderViewSnapshot view = chooseView(plan);
        clearValues[0].color = { {
            view.clearColor[0],
            view.clearColor[1],
            view.clearColor[2],
            view.clearColor[3]
        } };
        clearValues[1].depthStencil = { 1.0f, 0 };

        VkRenderPassBeginInfo rpBegin{};
        rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpBegin.renderPass = renderPass;
        rpBegin.framebuffer = swapchain.framebuffer(imageIndex);
        rpBegin.renderArea.offset = { 0, 0 };
        rpBegin.renderArea.extent = extent;
        rpBegin.clearValueCount = 2;
        rpBegin.pClearValues = clearValues;

        vkCmdBeginRenderPass(primary, &rpBegin, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);

        if (!secondaryBuffers.empty()) {
            vkCmdExecuteCommands(primary, static_cast<uint32_t>(secondaryBuffers.size()), secondaryBuffers.data());
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
        renderer::SnapshotRing<RenderWorldSnapshot, 3> snapshotRing{};
        const RendererCapabilityProfile capabilityProfile{
            .dedicatedTransferQueue = deviceContext.transferFamilyIndex() != deviceContext.graphicsFamilyIndex(),
            .dedicatedComputeQueue = deviceContext.computeFamilyIndex() != deviceContext.graphicsFamilyIndex()
        };
        for (auto& frame : frames) {
            frame.imageAvailable = VulkanSemaphore(deviceContext.vkDevice());
            frame.inFlight = VulkanFence(deviceContext.vkDevice(), VK_FENCE_CREATE_SIGNALED_BIT);
        }

        std::vector<VulkanSemaphore> presentFinishedByImage =
            createPerImagePresentSemaphores(deviceContext.vkDevice(), swapchain.imageCount());

        uint32_t frameIndex = 0;
        bool snapshotWarmupLogged = false;
        PersistentResourceService persistentResourceService{};
        ResourceAllocationRegistry resourceAllocationRegistry{ persistentResourceService, deviceContext.gpuAllocator.get() };
        auto& runtimeAssetService = RuntimeAssetService::instance();
        if (!runtimeAssetService.initializeDefaultBackend() || !runtimeAssetService.refreshFromBackend()) {
            throw std::runtime_error("Failed to initialize runtime asset backend");
        }
        RenderAssetRegistry assetRegistry{ runtimeAssetService };
        resourceAllocationRegistry.syncPersistentColorTargets(swapchain);
        auto previousTick = std::chrono::steady_clock::now();

        while (!glfwWindowShouldClose(window_)) {
            glfwPollEvents();

            const auto now = std::chrono::steady_clock::now();
            const float deltaSeconds = std::chrono::duration<float>(now - previousTick).count();
            previousTick = now;

            game.tick(SimulationFrameInput{
                .deltaSeconds = deltaSeconds,
                .frameIndex = frameIndex
                });
            runtimeAssetService.refreshFromBackend();
            const auto writeTicket = snapshotRing.beginWrite();
            *writeTicket.snapshot = game.buildRenderSnapshot();
            snapshotRing.publish(writeTicket);

            // Deterministic bootstrap contract: render begins only after staged N+1/N snapshots exist.
            if (writeTicket.writeEpoch < 2) {
                if (!snapshotWarmupLogged) {
                    std::cerr << "SnapshotRing warmup: waiting for staged N+1/N handoff" << std::endl;
                    snapshotWarmupLogged = true;
                }
                ++frameIndex;
                continue;
            }

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
                    resourceAllocationRegistry.syncPersistentColorTargets(swapchain);
                    resourceAllocationRegistry.recreateOwnedResourcesOnSwapchainEvent();
                }
                continue;
            }
            if (acquireResult != VK_SUCCESS) {
                vkutil::throwVkError("vkAcquireNextImageKHR", acquireResult);
            }

            const auto readTicket = snapshotRing.beginReadStaged();
            if (!readTicket.has_value()) {
                ++frameIndex;
                continue;
            }
            auto stagedReadGuard = makeScopeExit([&]() {
                snapshotRing.endRead(*readTicket);
            });

            ensure(frame.inFlight.resetResult(), "frameFence.reset");

            const ValidatedRenderSnapshot validatedSnapshot = validateRenderSnapshot(*readTicket->snapshot, assetRegistry);
            const RendererPlan renderPlan = compileRendererPlan(validatedSnapshot, capabilityProfile);
            if (validatedSnapshot.diagnostics.invalidInstances > 0 || validatedSnapshot.diagnostics.invalidLights > 0
                || validatedSnapshot.diagnostics.unresolvedMeshes > 0 || validatedSnapshot.diagnostics.unresolvedMaterials > 0
                || validatedSnapshot.diagnostics.staleMeshHandles > 0 || validatedSnapshot.diagnostics.staleMaterialHandles > 0
                || validatedSnapshot.diagnostics.loadingMeshes > 0 || validatedSnapshot.diagnostics.loadingMaterials > 0
                || validatedSnapshot.diagnostics.failedMeshes > 0 || validatedSnapshot.diagnostics.failedMaterials > 0) {
                std::cerr << "Renderer ingestion: dropped " << validatedSnapshot.diagnostics.droppedDraws
                    << " draws, invalidInstances=" << validatedSnapshot.diagnostics.invalidInstances
                    << ", invalidLights=" << validatedSnapshot.diagnostics.invalidLights
                    << ", unresolvedMeshes=" << validatedSnapshot.diagnostics.unresolvedMeshes
                    << ", unresolvedMaterials=" << validatedSnapshot.diagnostics.unresolvedMaterials
                    << ", staleMeshHandles=" << validatedSnapshot.diagnostics.staleMeshHandles
                    << ", staleMaterialHandles=" << validatedSnapshot.diagnostics.staleMaterialHandles
                    << ", loadingMeshes=" << validatedSnapshot.diagnostics.loadingMeshes
                    << ", loadingMaterials=" << validatedSnapshot.diagnostics.loadingMaterials
                    << ", failedMeshes=" << validatedSnapshot.diagnostics.failedMeshes
                    << ", failedMaterials=" << validatedSnapshot.diagnostics.failedMaterials << std::endl;
            }

            RenderTaskGraph graph{};
            resourceAllocationRegistry.beginFrame();
            std::optional<RenderTaskGraph::ResourceId> transferOutResource{};
            std::optional<RenderTaskGraph::ResourceId> computeOutResource{};
            std::optional<RenderTaskGraph::ResourceId> lightOutResource{};
            std::optional<RenderTaskGraph::ResourceId> colorResource{};
            std::optional<RenderTaskGraph::ResourceId> persistentSceneBufferResource{};

            for (const PlanResourceRequirement& req : renderPlan.resources) {
                if (req.id == PlanResourceId::ColorTarget) {
                    if (req.lifetime != ResourceLifetime::Persistent) {
                        throw std::runtime_error("RendererPlan invalid lifetime: ColorTarget must be persistent");
                    }
                    colorResource = resourceAllocationRegistry.allocatePersistentColorTarget(
                        graph,
                        static_cast<uint64_t>(imageIndex),
                        deviceContext.graphicsFamilyIndex());
                    continue;
                }

                if (req.id == PlanResourceId::PersistentSceneBuffer) {
                    if (req.lifetime != ResourceLifetime::Persistent) {
                        throw std::runtime_error("RendererPlan invalid lifetime: PersistentSceneBuffer must be persistent");
                    }
                    persistentSceneBufferResource = resourceAllocationRegistry.allocatePersistentSceneBuffer(
                        graph,
                        deviceContext.graphicsFamilyIndex());
                    continue;
                }

                if (req.lifetime != ResourceLifetime::Transient) {
                    throw std::runtime_error("RendererPlan invalid lifetime: payload resources must be transient");
                }

                if (req.id == PlanResourceId::TransferPayload) {
                    transferOutResource = resourceAllocationRegistry.allocateTransient(graph, PlanResourceId::TransferPayload);
                }
                else if (req.id == PlanResourceId::ComputePayload) {
                    computeOutResource = resourceAllocationRegistry.allocateTransient(graph, PlanResourceId::ComputePayload);
                }
                else if (req.id == PlanResourceId::LightPayload) {
                    lightOutResource = resourceAllocationRegistry.allocateTransient(graph, PlanResourceId::LightPayload);
                }
            }
            if (!colorResource.has_value()) {
                colorResource = resourceAllocationRegistry.allocatePersistentColorTarget(
                    graph,
                    static_cast<uint64_t>(imageIndex),
                    deviceContext.graphicsFamilyIndex());
            }

            const auto& allocationStats = resourceAllocationRegistry.frameStats();
            if (allocationStats.persistentDiscovered > 0) {
                std::cerr << "Renderer resources: discovered " << allocationStats.persistentDiscovered
                    << " persistent color targets" << std::endl;
            }

            auto resolveResource = [&](PlanResourceId id) -> RenderTaskGraph::ResourceId {
                switch (id) {
                case PlanResourceId::TransferPayload:
                    if (!transferOutResource.has_value()) {
                        transferOutResource = resourceAllocationRegistry.allocateTransient(graph, PlanResourceId::TransferPayload);
                    }
                    return *transferOutResource;
                case PlanResourceId::ComputePayload:
                    if (!computeOutResource.has_value()) {
                        computeOutResource = resourceAllocationRegistry.allocateTransient(graph, PlanResourceId::ComputePayload);
                    }
                    return *computeOutResource;
                case PlanResourceId::LightPayload:
                    if (!lightOutResource.has_value()) {
                        lightOutResource = resourceAllocationRegistry.allocateTransient(graph, PlanResourceId::LightPayload);
                    }
                    return *lightOutResource;
                case PlanResourceId::PersistentSceneBuffer:
                    if (!persistentSceneBufferResource.has_value()) {
                        persistentSceneBufferResource = resourceAllocationRegistry.allocatePersistentSceneBuffer(
                            graph,
                            deviceContext.graphicsFamilyIndex());
                    }
                    return *persistentSceneBufferResource;
                case PlanResourceId::ColorTarget:
                    return *colorResource;
                }
                return *colorResource;
            };

            auto queueFamilyIndexForClass = [&](SubmissionScheduler::QueueClass queueClass) -> uint32_t {
                switch (queueClass) {
                case SubmissionScheduler::QueueClass::Transfer:
                    return deviceContext.transferFamilyIndex();
                case SubmissionScheduler::QueueClass::Compute:
                    return deviceContext.computeFamilyIndex();
                case SubmissionScheduler::QueueClass::Graphics:
                    return deviceContext.graphicsFamilyIndex();
                }
                return deviceContext.graphicsFamilyIndex();
            };

            auto compileUsages = [&](const RendererPlan::PassPlan& pass) {
                std::vector<RenderTaskGraph::ResourceUsage> usages{};
                usages.reserve(pass.usages.size());
                for (const PlanResourceUsage& usage : pass.usages) {
                    usages.push_back(RenderTaskGraph::ResourceUsage{
                        .resource = resolveResource(usage.resource),
                        .access = usage.access,
                        .stageMask = usage.stageMask,
                        .accessMask = usage.accessMask,
                        .imageLayout = usage.imageLayout,
                        .queueFamilyIndex = queueFamilyIndexForClass(pass.queueClass)
                    });
                }
                return usages;
            };

            const bool useSync2 = deviceContext.isFeatureEnabledSynchronization2();

            std::optional<VulkanCommandArena::BorrowedCommandBuffer> transferPrimary{};
            std::optional<VulkanCommandArena::BorrowedCommandBuffer> computePrimary{};
            std::optional<VulkanCommandArena::BorrowedCommandBuffer> graphicsPrimary{};

            if (renderPlan.hasPass(RendererPlan::PassPlan::Kind::Transfer)) {
                auto borrowed = transferArena.acquirePrimary(transferToken.value(), 0, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
                if (!borrowed.hasValue()) {
                    vkutil::throwVkError("transferArena.acquirePrimary", borrowed.error());
                }
                transferPrimary = borrowed.value();
            }

            if (renderPlan.hasPass(RendererPlan::PassPlan::Kind::Compute)) {
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

            std::unordered_map<uint32_t, RenderTaskGraph::PassId> graphPassIdByNode{};
            graphPassIdByNode.reserve(renderPlan.activePasses.size());

            for (const RendererPlan::PassPlan& pass : renderPlan.activePasses) {
                if (pass.kind == RendererPlan::PassPlan::Kind::Transfer) {
                    const auto transferPassId = graph.addPass(RenderTaskGraph::PassNode{
                        .job = SubmissionScheduler::JobRequest{
                            .queueClass = SubmissionScheduler::QueueClass::Transfer,
                            .commandBuffers = { transferPrimary->handle },
                            .debugLabel = pass.name
                        },
                        .usages = compileUsages(pass),
                        .record = [&](const RenderTaskGraph::BarrierBatch& incomingBarriers, const RenderTaskGraph::BarrierBatch& outgoingBarriers) {
                            TransferSubsystem::record(transferPrimary->handle, incomingBarriers, outgoingBarriers, useSync2);
                            return transferArena.endBorrowed(*transferPrimary);
                        }
                    });
                    graphPassIdByNode.insert_or_assign(pass.nodeId, transferPassId);
                    continue;
                }

                if (pass.kind == RendererPlan::PassPlan::Kind::Compute) {
                    const auto computePassId = graph.addPass(RenderTaskGraph::PassNode{
                        .job = SubmissionScheduler::JobRequest{
                            .queueClass = SubmissionScheduler::QueueClass::Compute,
                            .commandBuffers = { computePrimary->handle },
                            .debugLabel = pass.name
                        },
                        .usages = compileUsages(pass),
                        .record = [&](const RenderTaskGraph::BarrierBatch& incomingBarriers, const RenderTaskGraph::BarrierBatch& outgoingBarriers) {
                            ComputeSubsystem::record(computePrimary->handle, incomingBarriers, outgoingBarriers, useSync2);
                            return computeArena.endBorrowed(*computePrimary);
                        }
                    });
                    graphPassIdByNode.insert_or_assign(pass.nodeId, computePassId);
                    continue;
                }

                const auto graphicsPassId = graph.addPass(RenderTaskGraph::PassNode{
                    .job = SubmissionScheduler::JobRequest{
                        .queueClass = SubmissionScheduler::QueueClass::Graphics,
                        .commandBuffers = { graphicsPrimary->handle },
                        .waitSemaphores = { frame.imageAvailable.get() },
                        .waitStages = { VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT },
                        .signalSemaphores = { presentFinishedByImage[imageIndex].get() },
                        .fence = frame.inFlight.get(),
                        .debugLabel = pass.name
                    },
                    .usages = compileUsages(pass),
                    .record = [&](const RenderTaskGraph::BarrierBatch& incomingBarriers, const RenderTaskGraph::BarrierBatch& outgoingBarriers) {
                        const size_t totalDraws = renderPlan.drawCommands.size();
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
                                renderPlan.drawCommands,
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
                            renderPlan,
                            incomingBarriers,
                            outgoingBarriers,
                            useSync2,
                            secondaries);

                        return graphicsArena.endBorrowed(*graphicsPrimary);
                    }
                });
                graphPassIdByNode.insert_or_assign(pass.nodeId, graphicsPassId);
            }

            for (const RendererPlan::PassPlan& pass : renderPlan.activePasses) {
                const auto consumerIt = graphPassIdByNode.find(pass.nodeId);
                if (consumerIt == graphPassIdByNode.end()) {
                    throw std::runtime_error("RendererPlan node mapping missing for consumer");
                }
                for (const uint32_t depNode : pass.dependsOnPassNodes) {
                    const auto producerIt = graphPassIdByNode.find(depNode);
                    if (producerIt == graphPassIdByNode.end()) {
                        throw std::runtime_error("RendererPlan node mapping missing for dependency");
                    }
                    graph.addDependency(producerIt->second, consumerIt->second);
                }
            }

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
                    resourceAllocationRegistry.syncPersistentColorTargets(swapchain);
                    resourceAllocationRegistry.recreateOwnedResourcesOnSwapchainEvent();
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


void Engine::run(IGameSimulation& game)
{
    run(game, RunConfig{});
}
