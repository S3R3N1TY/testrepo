#include "RenderGraph.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace {
struct EdgeHash {
    size_t operator()(const std::pair<RenderTaskGraph::PassId, RenderTaskGraph::PassId>& e) const noexcept
    {
        return std::hash<size_t>{}(e.first) ^ (std::hash<size_t>{}(e.second) << 1U);
    }
};

void appendBarrierBatch(RenderTaskGraph::BarrierBatch& dst, const RenderTaskGraph::BarrierBatch& src)
{
    dst.memoryBarriers.insert(dst.memoryBarriers.end(), src.memoryBarriers.begin(), src.memoryBarriers.end());
    dst.bufferBarriers.insert(dst.bufferBarriers.end(), src.bufferBarriers.begin(), src.bufferBarriers.end());
    dst.imageBarriers.insert(dst.imageBarriers.end(), src.imageBarriers.begin(), src.imageBarriers.end());
}

class PersistentRenderGraphTaskSystem {
public:
    using TaskFn = std::function<void(uint32_t workerLane, size_t taskIndex)>;

    static PersistentRenderGraphTaskSystem& instance()
    {
        static PersistentRenderGraphTaskSystem pool;
        return pool;
    }

    void run(size_t taskCount, TaskFn task)
    {
        if (!task || taskCount == 0) {
            return;
        }

        if (workerCount_ == 0) {
            for (size_t i = 0; i < taskCount; ++i) {
                task(0, i);
            }
            return;
        }

        {
            std::unique_lock<std::mutex> lock(mutex_);
            task_ = std::move(task);
            taskCount_ = taskCount;
            nextIndex_.store(0, std::memory_order_release);
            activeWorkers_.store(workerCount_, std::memory_order_release);
            generation_ += 1;
        }

        cvStart_.notify_all();

        std::unique_lock<std::mutex> lock(mutex_);
        cvDone_.wait(lock, [&]() { return activeWorkers_.load(std::memory_order_acquire) == 0; });
        task_ = {};
        taskCount_ = 0;
    }

    ~PersistentRenderGraphTaskSystem() noexcept
    {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            stop_ = true;
            generation_ += 1;
        }
        cvStart_.notify_all();

        for (std::thread& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

private:
    PersistentRenderGraphTaskSystem()
    {
        const uint32_t hardwareThreads = std::max(1u, std::thread::hardware_concurrency());
        workerCount_ = hardwareThreads > 1 ? hardwareThreads - 1 : 0;
        workers_.reserve(workerCount_);
        for (uint32_t i = 0; i < workerCount_; ++i) {
            workers_.emplace_back([this, i]() { workerLoop(i); });
        }
    }

    void workerLoop(uint32_t workerLane)
    {
        uint64_t observedGeneration = 0;
        while (true) {
            TaskFn task;
            size_t taskCount = 0;

            {
                std::unique_lock<std::mutex> lock(mutex_);
                cvStart_.wait(lock, [&]() { return stop_ || generation_ != observedGeneration; });
                if (stop_) {
                    return;
                }

                observedGeneration = generation_;
                task = task_;
                taskCount = taskCount_;
            }

            while (true) {
                const size_t index = nextIndex_.fetch_add(1, std::memory_order_relaxed);
                if (index >= taskCount) {
                    break;
                }
                task(workerLane, index);
            }

            if (activeWorkers_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                std::unique_lock<std::mutex> lock(mutex_);
                cvDone_.notify_one();
            }
        }
    }

    std::mutex mutex_{};
    std::condition_variable cvStart_{};
    std::condition_variable cvDone_{};
    std::vector<std::thread> workers_{};

    TaskFn task_{};
    size_t taskCount_{ 0 };
    uint32_t workerCount_{ 0 };
    uint64_t generation_{ 0 };
    bool stop_{ false };

    std::atomic<size_t> nextIndex_{ 0 };
    std::atomic<uint32_t> activeWorkers_{ 0 };
};
}

void RenderTaskGraph::clear()
{
    resources_.clear();
    passes_.clear();
    presentRequest_.reset();
    nextResourceId_ = 1;
}

RenderTaskGraph::ResourceId RenderTaskGraph::createResource()
{
    const ResourceId id = nextResourceId_++;
    resources_.insert_or_assign(id, ResourceDescriptor{});
    return id;
}

RenderTaskGraph::ResourceId RenderTaskGraph::createBufferResource(
    VkBuffer buffer,
    VkDeviceSize offset,
    VkDeviceSize size,
    VkPipelineStageFlags2 initialStageMask,
    VkAccessFlags2 initialAccessMask,
    uint32_t initialQueueFamilyIndex)
{
    const ResourceId id = nextResourceId_++;
    ResourceDescriptor descriptor{};
    descriptor.type = ResourceType::Buffer;
    descriptor.buffer = buffer;
    descriptor.bufferOffset = offset;
    descriptor.bufferSize = size;
    descriptor.initialStageMask = initialStageMask;
    descriptor.initialAccessMask = initialAccessMask;
    descriptor.initialQueueFamilyIndex = initialQueueFamilyIndex;
    resources_.insert_or_assign(id, descriptor);
    return id;
}

RenderTaskGraph::ResourceId RenderTaskGraph::createImageResource(
    VkImage image,
    const VkImageSubresourceRange& subresourceRange,
    VkImageLayout initialLayout,
    VkPipelineStageFlags2 initialStageMask,
    VkAccessFlags2 initialAccessMask,
    uint32_t initialQueueFamilyIndex)
{
    const ResourceId id = nextResourceId_++;
    ResourceDescriptor descriptor{};
    descriptor.type = ResourceType::Image;
    descriptor.image = image;
    descriptor.imageSubresourceRange = subresourceRange;
    descriptor.initialImageLayout = initialLayout;
    descriptor.initialStageMask = initialStageMask;
    descriptor.initialAccessMask = initialAccessMask;
    descriptor.initialQueueFamilyIndex = initialQueueFamilyIndex;
    resources_.insert_or_assign(id, descriptor);
    return id;
}

RenderTaskGraph::ResourceId RenderTaskGraph::createTransientBufferResource(
    VkDeviceSize size,
    VkDeviceSize alignment,
    uint64_t aliasClass,
    VkPipelineStageFlags2 initialStageMask,
    VkAccessFlags2 initialAccessMask,
    uint32_t initialQueueFamilyIndex)
{
    const ResourceId id = nextResourceId_++;
    ResourceDescriptor descriptor{};
    descriptor.type = ResourceType::Buffer;
    descriptor.transient = true;
    descriptor.aliasClass = aliasClass;
    descriptor.buffer = VK_NULL_HANDLE;
    descriptor.bufferOffset = 0;
    descriptor.bufferSize = VK_WHOLE_SIZE;
    descriptor.transientBufferSize = size;
    descriptor.transientBufferAlignment = std::max<VkDeviceSize>(1, alignment);
    descriptor.initialStageMask = initialStageMask;
    descriptor.initialAccessMask = initialAccessMask;
    descriptor.initialQueueFamilyIndex = initialQueueFamilyIndex;
    resources_.insert_or_assign(id, descriptor);
    return id;
}

RenderTaskGraph::ResourceId RenderTaskGraph::createTransientImageResource(
    VkExtent3D extent,
    VkFormat format,
    VkImageUsageFlags usage,
    VkImageLayout initialLayout,
    uint64_t aliasClass,
    VkImageType imageType,
    uint32_t mipLevels,
    uint32_t arrayLayers,
    VkSampleCountFlagBits samples,
    VkPipelineStageFlags2 initialStageMask,
    VkAccessFlags2 initialAccessMask,
    uint32_t initialQueueFamilyIndex)
{
    const ResourceId id = nextResourceId_++;
    ResourceDescriptor descriptor{};
    descriptor.type = ResourceType::Image;
    descriptor.transient = true;
    descriptor.aliasClass = aliasClass;
    descriptor.image = VK_NULL_HANDLE;
    descriptor.imageSubresourceRange = VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, arrayLayers };
    descriptor.transientImageExtent = extent;
    descriptor.transientImageFormat = format;
    descriptor.transientImageUsage = usage;
    descriptor.transientImageType = imageType;
    descriptor.transientImageMipLevels = mipLevels;
    descriptor.transientImageArrayLayers = arrayLayers;
    descriptor.transientImageSamples = samples;
    descriptor.initialImageLayout = initialLayout;
    descriptor.initialStageMask = initialStageMask;
    descriptor.initialAccessMask = initialAccessMask;
    descriptor.initialQueueFamilyIndex = initialQueueFamilyIndex;
    resources_.insert_or_assign(id, descriptor);
    return id;
}

RenderTaskGraph::PassId RenderTaskGraph::addPass(PassNode pass)
{
    const PassId id = passes_.size();
    passes_.push_back(std::move(pass));
    return id;
}

void RenderTaskGraph::setPresent(const SubmissionScheduler::PresentRequest& request)
{
    presentRequest_ = request;
}

bool RenderTaskGraph::isWriteAccess(ResourceAccessType access) noexcept
{
    return access == ResourceAccessType::Write || access == ResourceAccessType::ReadWrite;
}

bool RenderTaskGraph::imageRangesOverlap(const VkImageSubresourceRange& lhs, const VkImageSubresourceRange& rhs) noexcept
{
    if ((lhs.aspectMask & rhs.aspectMask) == 0) {
        return false;
    }

    const uint32_t lhsMipEnd = lhs.levelCount == VK_REMAINING_MIP_LEVELS ? UINT32_MAX : lhs.baseMipLevel + lhs.levelCount;
    const uint32_t rhsMipEnd = rhs.levelCount == VK_REMAINING_MIP_LEVELS ? UINT32_MAX : rhs.baseMipLevel + rhs.levelCount;
    const uint32_t lhsLayerEnd = lhs.layerCount == VK_REMAINING_ARRAY_LAYERS ? UINT32_MAX : lhs.baseArrayLayer + lhs.layerCount;
    const uint32_t rhsLayerEnd = rhs.layerCount == VK_REMAINING_ARRAY_LAYERS ? UINT32_MAX : rhs.baseArrayLayer + rhs.layerCount;

    const bool mipOverlaps = lhs.baseMipLevel < rhsMipEnd && rhs.baseMipLevel < lhsMipEnd;
    const bool layerOverlaps = lhs.baseArrayLayer < rhsLayerEnd && rhs.baseArrayLayer < lhsLayerEnd;
    return mipOverlaps && layerOverlaps;
}

bool RenderTaskGraph::usagesOverlap(const ResourceDescriptor& descriptor, const ResourceUsage& lhs, const ResourceUsage& rhs) noexcept
{
    if (descriptor.type == ResourceType::Image) {
        return imageRangesOverlap(lhs.imageSubresourceRange, rhs.imageSubresourceRange);
    }

    if (descriptor.type == ResourceType::Buffer) {
        const VkDeviceSize lhsStart = lhs.bufferOffset;
        const VkDeviceSize rhsStart = rhs.bufferOffset;
        const VkDeviceSize lhsEnd = lhs.bufferSize == VK_WHOLE_SIZE ? UINT64_MAX : lhsStart + lhs.bufferSize;
        const VkDeviceSize rhsEnd = rhs.bufferSize == VK_WHOLE_SIZE ? UINT64_MAX : rhsStart + rhs.bufferSize;
        return lhsStart < rhsEnd && rhsStart < lhsEnd;
    }

    return true;
}

vkutil::VkExpected<void> RenderTaskGraph::validateUsageContract(const ResourceDescriptor& descriptor, const ResourceUsage& usage) noexcept
{
    if (usage.resource == 0) {
        return vkutil::makeError("RenderTaskGraph::validateUsageContract", VK_ERROR_INITIALIZATION_FAILED, "render_graph", "resource_id_zero");
    }

    if (usage.stageMask == VK_PIPELINE_STAGE_2_NONE && usage.accessMask != VK_ACCESS_2_NONE) {
        return vkutil::makeError("RenderTaskGraph::validateUsageContract", VK_ERROR_INITIALIZATION_FAILED, "render_graph", "access_mask_requires_stage_mask");
    }

    if (descriptor.type == ResourceType::Image) {
        const bool writesImage = usage.access == ResourceAccessType::Write || usage.access == ResourceAccessType::ReadWrite;
        if (writesImage && usage.imageLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
            return vkutil::makeError("RenderTaskGraph::validateUsageContract", VK_ERROR_INITIALIZATION_FAILED, "render_graph", "image_write_requires_explicit_layout");
        }
    }

    return {};
}

vkutil::VkExpected<RenderTaskGraph::SyncContractDecision> RenderTaskGraph::buildSyncContractDecision(
    const ResourceDescriptor& descriptor,
    const ResourceUsage& src,
    const ResourceUsage& dst) noexcept
{
    SyncContractDecision decision{};

    const bool srcWrite = isWriteAccess(src.access);
    const bool dstWrite = isWriteAccess(dst.access);
    decision.requiresMemoryBarrier = srcWrite || dstWrite;

    decision.requiresQueueOwnershipTransfer = src.queueFamilyIndex != VK_QUEUE_FAMILY_IGNORED
        && dst.queueFamilyIndex != VK_QUEUE_FAMILY_IGNORED
        && src.queueFamilyIndex != dst.queueFamilyIndex;

    decision.srcQueueFamilyIndex = decision.requiresQueueOwnershipTransfer ? src.queueFamilyIndex : VK_QUEUE_FAMILY_IGNORED;
    decision.dstQueueFamilyIndex = decision.requiresQueueOwnershipTransfer ? dst.queueFamilyIndex : VK_QUEUE_FAMILY_IGNORED;

    decision.srcStageMask = src.stageMask != 0 ? src.stageMask : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    decision.srcAccessMask = src.accessMask;
    decision.dstStageMask = dst.stageMask != 0 ? dst.stageMask : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    decision.dstAccessMask = dst.accessMask;

    if (descriptor.type == ResourceType::Image) {
        decision.oldLayout = src.imageLayout;
        decision.newLayout = dst.imageLayout != VK_IMAGE_LAYOUT_UNDEFINED ? dst.imageLayout : src.imageLayout;

        if (decision.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && decision.newLayout != VK_IMAGE_LAYOUT_UNDEFINED) {
            decision.oldLayout = decision.newLayout;
        }
        if (decision.newLayout == VK_IMAGE_LAYOUT_UNDEFINED && decision.oldLayout != VK_IMAGE_LAYOUT_UNDEFINED) {
            decision.newLayout = decision.oldLayout;
        }

        decision.requiresLayoutTransition = decision.oldLayout != VK_IMAGE_LAYOUT_UNDEFINED
            && decision.newLayout != VK_IMAGE_LAYOUT_UNDEFINED
            && decision.oldLayout != decision.newLayout;
    }

    decision.requiresExecutionDependency = decision.requiresMemoryBarrier
        || decision.requiresQueueOwnershipTransfer
        || decision.requiresLayoutTransition;

    if (decision.requiresExecutionDependency
        && decision.srcStageMask == VK_PIPELINE_STAGE_2_NONE
        && decision.dstStageMask == VK_PIPELINE_STAGE_2_NONE)
    {
        return vkutil::VkExpected<SyncContractDecision>(
            vkutil::makeError("RenderTaskGraph::buildSyncContractDecision", VK_ERROR_INITIALIZATION_FAILED, "render_graph", "execution_dependency_requires_stage_masks").context());
    }

    return decision;
}

RenderTaskGraph::ResourceUsage RenderTaskGraph::makeInitialUsage(const ResourceDescriptor& descriptor) noexcept
{
    ResourceUsage usage{};
    usage.resource = 0;
    usage.access = ResourceAccessType::Read;
    usage.stageMask = descriptor.initialStageMask;
    usage.accessMask = descriptor.initialAccessMask;
    usage.imageLayout = descriptor.initialImageLayout;
    usage.queueFamilyIndex = descriptor.initialQueueFamilyIndex;
    return usage;
}

bool RenderTaskGraph::requiresBarrier(const ResourceDescriptor& descriptor, const ResourceUsage& src, const ResourceUsage& dst) noexcept
{
    const auto contract = buildSyncContractDecision(descriptor, src, dst);
    return contract.hasValue() && contract.value().requiresExecutionDependency;
}

RenderTaskGraph::BarrierBatch RenderTaskGraph::makeBarrierBatch(const ResourceDescriptor& descriptor, const ResourceUsage& src, const ResourceUsage& dst) noexcept
{
    BarrierBatch batch{};
    const auto contractResult = buildSyncContractDecision(descriptor, src, dst);
    if (!contractResult.hasValue()) {
        return batch;
    }
    const SyncContractDecision& contract = contractResult.value();

    if (!contract.requiresExecutionDependency) {
        return batch;
    }

    if (descriptor.type == ResourceType::Buffer && descriptor.buffer != VK_NULL_HANDLE) {
        VkBufferMemoryBarrier2 barrier{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
        barrier.srcStageMask = contract.srcStageMask;
        barrier.srcAccessMask = contract.srcAccessMask;
        barrier.dstStageMask = contract.dstStageMask;
        barrier.dstAccessMask = contract.dstAccessMask;
        barrier.srcQueueFamilyIndex = contract.srcQueueFamilyIndex;
        barrier.dstQueueFamilyIndex = contract.dstQueueFamilyIndex;
        barrier.buffer = descriptor.buffer;
        barrier.offset = dst.bufferOffset;
        barrier.size = dst.bufferSize;

        batch.bufferBarriers.push_back(barrier);
        return batch;
    }

    if (descriptor.type == ResourceType::Image && descriptor.image != VK_NULL_HANDLE) {
        VkImageMemoryBarrier2 barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        barrier.srcStageMask = contract.srcStageMask;
        barrier.srcAccessMask = contract.srcAccessMask;
        barrier.dstStageMask = contract.dstStageMask;
        barrier.dstAccessMask = contract.dstAccessMask;
        barrier.srcQueueFamilyIndex = contract.srcQueueFamilyIndex;
        barrier.dstQueueFamilyIndex = contract.dstQueueFamilyIndex;
        barrier.oldLayout = contract.oldLayout;
        barrier.newLayout = contract.newLayout;
        barrier.image = descriptor.image;
        barrier.subresourceRange = dst.imageSubresourceRange;

        batch.imageBarriers.push_back(barrier);
        return batch;
    }

    VkMemoryBarrier2 barrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    barrier.srcStageMask = contract.srcStageMask;
    barrier.srcAccessMask = contract.srcAccessMask;
    barrier.dstStageMask = contract.dstStageMask;
    barrier.dstAccessMask = contract.dstAccessMask;
    batch.memoryBarriers.push_back(barrier);
    return batch;
}

RenderTaskGraph::BarrierBatch RenderTaskGraph::makeReleaseBarrierBatch(const ResourceDescriptor& descriptor, const ResourceUsage& src, const ResourceUsage& dst) noexcept
{
    BarrierBatch batch = makeBarrierBatch(descriptor, src, dst);
    for (VkMemoryBarrier2& barrier : batch.memoryBarriers) {
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
        barrier.dstAccessMask = VK_ACCESS_2_NONE;
    }
    for (VkBufferMemoryBarrier2& barrier : batch.bufferBarriers) {
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
        barrier.dstAccessMask = VK_ACCESS_2_NONE;
    }
    for (VkImageMemoryBarrier2& barrier : batch.imageBarriers) {
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
        barrier.dstAccessMask = VK_ACCESS_2_NONE;
    }
    return batch;
}

RenderTaskGraph::BarrierBatch RenderTaskGraph::makeAcquireBarrierBatch(const ResourceDescriptor& descriptor, const ResourceUsage& src, const ResourceUsage& dst) noexcept
{
    BarrierBatch batch = makeBarrierBatch(descriptor, src, dst);
    for (VkMemoryBarrier2& barrier : batch.memoryBarriers) {
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        barrier.srcAccessMask = VK_ACCESS_2_NONE;
    }
    for (VkBufferMemoryBarrier2& barrier : batch.bufferBarriers) {
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        barrier.srcAccessMask = VK_ACCESS_2_NONE;
    }
    for (VkImageMemoryBarrier2& barrier : batch.imageBarriers) {
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        barrier.srcAccessMask = VK_ACCESS_2_NONE;
    }
    return batch;
}

vkutil::VkExpected<void> RenderTaskGraph::buildDependenciesAndBarriers(
    std::vector<Edge>& outEdges,
    std::vector<BarrierBatch>& outIncomingBarriers,
    std::vector<BarrierBatch>& outOutgoingBarriers) const
{
    outEdges.clear();
    outIncomingBarriers.clear();
    outOutgoingBarriers.clear();
    outIncomingBarriers.resize(passes_.size());
    outOutgoingBarriers.resize(passes_.size());

    std::unordered_map<ResourceId, ResourceState> resourceStates{};
    resourceStates.reserve(resources_.size());
    for (const auto& [id, descriptor] : resources_) {
        resourceStates.insert_or_assign(id, ResourceState{ .descriptor = descriptor });
    }

    std::unordered_set<std::pair<PassId, PassId>, EdgeHash> edgeDedup{};

    auto addEdge = [&](PassId producer, PassId consumer, VkPipelineStageFlags2 consumerStage) {
        if (producer == consumer) {
            return;
        }

        const std::pair<PassId, PassId> key{ producer, consumer };
        if (!edgeDedup.insert(key).second) {
            return;
        }

        outEdges.push_back(Edge{
            .producer = producer,
            .consumer = consumer,
            .consumerWaitStage = consumerStage != 0 ? consumerStage : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
            });
    };

    for (PassId passId = 0; passId < passes_.size(); ++passId) {
        const PassNode& pass = passes_[passId];

        for (const ResourceUsage& usage : pass.usages) {
            auto stateIt = resourceStates.find(usage.resource);
            if (stateIt == resourceStates.end()) {
                return vkutil::makeError("RenderTaskGraph::buildDependenciesAndBarriers", VK_ERROR_INITIALIZATION_FAILED, "render_graph", "resource_not_registered");
            }

            ResourceState& state = stateIt->second;
            const auto usageValidation = validateUsageContract(state.descriptor, usage);
            if (!usageValidation.hasValue()) {
                return vkutil::VkExpected<void>(usageValidation.context());
            }

            const bool writes = isWriteAccess(usage.access);

            if (state.lastWriter.has_value() && usagesOverlap(state.descriptor, state.lastWriter->usage, usage)) {
                const ResourceUsage& srcUsage = state.lastWriter->usage;
                const auto syncContract = buildSyncContractDecision(state.descriptor, srcUsage, usage);
                if (!syncContract.hasValue()) {
                    return vkutil::VkExpected<void>(syncContract.context());
                }
                if (syncContract.value().requiresExecutionDependency) {
                    addEdge(state.lastWriter->pass, passId, usage.stageMask);
                    if (syncContract.value().requiresQueueOwnershipTransfer && state.lastWriter->pass != passId) {
                        appendBarrierBatch(outOutgoingBarriers[state.lastWriter->pass], makeReleaseBarrierBatch(state.descriptor, srcUsage, usage));
                        appendBarrierBatch(outIncomingBarriers[passId], makeAcquireBarrierBatch(state.descriptor, srcUsage, usage));
                    } else {
                        appendBarrierBatch(outIncomingBarriers[passId], makeBarrierBatch(state.descriptor, srcUsage, usage));
                    }
                }
            }
            else {
                const ResourceUsage initialUsage = makeInitialUsage(state.descriptor);
                const auto syncContract = buildSyncContractDecision(state.descriptor, initialUsage, usage);
                if (!syncContract.hasValue()) {
                    return vkutil::VkExpected<void>(syncContract.context());
                }
                if (syncContract.value().requiresExecutionDependency) {
                    appendBarrierBatch(outIncomingBarriers[passId], makeBarrierBatch(state.descriptor, initialUsage, usage));
                }
            }

            if (writes) {
                for (const UsageRef& reader : state.readers) {
                    if (!usagesOverlap(state.descriptor, reader.usage, usage)) {
                        continue;
                    }
                    const auto syncContract = buildSyncContractDecision(state.descriptor, reader.usage, usage);
                    if (!syncContract.hasValue()) {
                        return vkutil::VkExpected<void>(syncContract.context());
                    }
                    if (syncContract.value().requiresExecutionDependency) {
                        addEdge(reader.pass, passId, usage.stageMask);
                        if (syncContract.value().requiresQueueOwnershipTransfer && reader.pass != passId) {
                            appendBarrierBatch(outOutgoingBarriers[reader.pass], makeReleaseBarrierBatch(state.descriptor, reader.usage, usage));
                            appendBarrierBatch(outIncomingBarriers[passId], makeAcquireBarrierBatch(state.descriptor, reader.usage, usage));
                        } else {
                            appendBarrierBatch(outIncomingBarriers[passId], makeBarrierBatch(state.descriptor, reader.usage, usage));
                        }
                    }
                }
                state.readers.clear();
                state.lastWriter = UsageRef{ .pass = passId, .usage = usage };
            }
            else {
                for (const UsageRef& reader : state.readers) {
                    if (!usagesOverlap(state.descriptor, reader.usage, usage)) {
                        continue;
                    }
                    const auto syncContract = buildSyncContractDecision(state.descriptor, reader.usage, usage);
                    if (!syncContract.hasValue()) {
                        return vkutil::VkExpected<void>(syncContract.context());
                    }
                    if (syncContract.value().requiresExecutionDependency) {
                        addEdge(reader.pass, passId, usage.stageMask);
                        if (syncContract.value().requiresQueueOwnershipTransfer && reader.pass != passId) {
                            appendBarrierBatch(outOutgoingBarriers[reader.pass], makeReleaseBarrierBatch(state.descriptor, reader.usage, usage));
                            appendBarrierBatch(outIncomingBarriers[passId], makeAcquireBarrierBatch(state.descriptor, reader.usage, usage));
                        } else {
                            appendBarrierBatch(outIncomingBarriers[passId], makeBarrierBatch(state.descriptor, reader.usage, usage));
                        }
                    }
                }
                state.readers.push_back(UsageRef{ .pass = passId, .usage = usage });
            }
        }
    }

    return {};
}

vkutil::VkExpected<RenderTaskGraph::ExecutionSchedule> RenderTaskGraph::buildExecutionSchedule(const std::vector<Edge>& edges) const
{
    ExecutionSchedule schedule{};
    schedule.levelByPass.resize(passes_.size(), 0);

    if (passes_.empty()) {
        return schedule;
    }

    std::vector<uint32_t> indegree(passes_.size(), 0);
    std::vector<size_t> nextLevelByPass(passes_.size(), 0);
    std::vector<std::vector<PassId>> adjacency(passes_.size());

    for (const Edge& edge : edges) {
        if (edge.producer >= passes_.size() || edge.consumer >= passes_.size()) {
            return vkutil::VkExpected<ExecutionSchedule>(
                vkutil::makeError("RenderTaskGraph::buildExecutionSchedule", VK_ERROR_INITIALIZATION_FAILED, "render_graph", "invalid_dependency_edge").context());
        }

        adjacency[edge.producer].push_back(edge.consumer);
        indegree[edge.consumer] += 1;
    }

    std::vector<PassId> ready{};
    ready.reserve(passes_.size());
    for (PassId passId = 0; passId < passes_.size(); ++passId) {
        if (indegree[passId] == 0) {
            ready.push_back(passId);
        }
    }

    std::sort(ready.begin(), ready.end());

    while (!ready.empty()) {
        std::vector<PassId> level{};
        level.swap(ready);
        std::sort(level.begin(), level.end());

        for (const PassId passId : level) {
            schedule.topologicalOrder.push_back(passId);
        }

        std::vector<PassId> nextReady{};
        for (const PassId passId : level) {
            for (const PassId child : adjacency[passId]) {
                nextLevelByPass[child] = std::max(nextLevelByPass[child], schedule.levelByPass[passId] + 1);
                uint32_t& childIndegree = indegree[child];
                childIndegree -= 1;
                if (childIndegree == 0) {
                    schedule.levelByPass[child] = nextLevelByPass[child];
                    nextReady.push_back(child);
                }
            }
        }

        schedule.levels.push_back(std::move(level));
        ready = std::move(nextReady);
    }

    if (schedule.topologicalOrder.size() != passes_.size()) {
        return vkutil::VkExpected<ExecutionSchedule>(
            vkutil::makeError("RenderTaskGraph::buildExecutionSchedule", VK_ERROR_INITIALIZATION_FAILED, "render_graph", "dependency_cycle_detected").context());
    }

    return schedule;
}

bool RenderTaskGraph::transientResourcesCompatible(const ResourceDescriptor& lhs, const ResourceDescriptor& rhs) noexcept
{
    if (lhs.type != rhs.type || !lhs.transient || !rhs.transient) {
        return false;
    }

    if (lhs.aliasClass != 0 && rhs.aliasClass != 0 && lhs.aliasClass != rhs.aliasClass) {
        return false;
    }

    if (lhs.type == ResourceType::Buffer) {
        return true;
    }

    if (lhs.type == ResourceType::Image) {
        return lhs.transientImageFormat == rhs.transientImageFormat
            && lhs.transientImageUsage == rhs.transientImageUsage
            && lhs.transientImageType == rhs.transientImageType
            && lhs.transientImageMipLevels == rhs.transientImageMipLevels
            && lhs.transientImageArrayLayers == rhs.transientImageArrayLayers
            && lhs.transientImageSamples == rhs.transientImageSamples;
    }

    return false;
}

vkutil::VkExpected<RenderTaskGraph::CompiledTransientPlan> RenderTaskGraph::buildTransientPlan(const ExecutionSchedule& schedule) const
{
    CompiledTransientPlan plan{};

    if (passes_.empty()) {
        return plan;
    }

    std::unordered_map<PassId, size_t> orderByPass{};
    orderByPass.reserve(schedule.topologicalOrder.size());
    for (size_t order = 0; order < schedule.topologicalOrder.size(); ++order) {
        orderByPass.insert_or_assign(schedule.topologicalOrder[order], order);
    }

    for (const auto& [resourceId, descriptor] : resources_) {
        if (!descriptor.transient) {
            continue;
        }

        bool used = false;
        size_t firstUse = 0;
        size_t lastUse = 0;
        for (PassId passId = 0; passId < passes_.size(); ++passId) {
            const PassNode& pass = passes_[passId];
            const bool usedByPass = std::any_of(pass.usages.begin(), pass.usages.end(), [&](const ResourceUsage& usage) {
                return usage.resource == resourceId;
                });
            if (!usedByPass) {
                continue;
            }

            auto orderIt = orderByPass.find(passId);
            if (orderIt == orderByPass.end()) {
                return vkutil::VkExpected<CompiledTransientPlan>(
                    vkutil::makeError("RenderTaskGraph::buildTransientPlan", VK_ERROR_INITIALIZATION_FAILED, "render_graph", "missing_pass_schedule_order").context());
            }

            const size_t order = orderIt->second;
            if (!used) {
                firstUse = order;
                lastUse = order;
                used = true;
            }
            else {
                firstUse = std::min(firstUse, order);
                lastUse = std::max(lastUse, order);
            }
        }

        if (!used) {
            continue;
        }

        plan.lifetimes.push_back(TransientResourceLifetime{
            .resource = resourceId,
            .firstUseOrder = firstUse,
            .lastUseOrder = lastUse,
            .type = descriptor.type
            });
    }

    std::sort(plan.lifetimes.begin(), plan.lifetimes.end(), [](const TransientResourceLifetime& lhs, const TransientResourceLifetime& rhs) {
        if (lhs.firstUseOrder == rhs.firstUseOrder) {
            return lhs.resource < rhs.resource;
        }
        return lhs.firstUseOrder < rhs.firstUseOrder;
        });

    struct AliasSlotState {
        uint32_t slotId{ 0 };
        ResourceType type{ ResourceType::Global };
        uint64_t aliasClass{ 0 };
        size_t lastUseOrder{ 0 };
        ResourceDescriptor descriptor{};
    };

    std::vector<AliasSlotState> slots{};
    uint32_t nextSlotId = 1;

    for (const TransientResourceLifetime& lifetime : plan.lifetimes) {
        auto descriptorIt = resources_.find(lifetime.resource);
        if (descriptorIt == resources_.end()) {
            return vkutil::VkExpected<CompiledTransientPlan>(
                vkutil::makeError("RenderTaskGraph::buildTransientPlan", VK_ERROR_INITIALIZATION_FAILED, "render_graph", "transient_descriptor_missing").context());
        }

        const ResourceDescriptor& descriptor = descriptorIt->second;
        if (!descriptor.transient) {
            continue;
        }

        AliasSlotState* chosenSlot = nullptr;
        for (AliasSlotState& slot : slots) {
            if (slot.type != descriptor.type) {
                continue;
            }
            if (lifetime.firstUseOrder <= slot.lastUseOrder) {
                continue;
            }
            if (!transientResourcesCompatible(slot.descriptor, descriptor)) {
                continue;
            }
            chosenSlot = &slot;
            break;
        }

        if (chosenSlot == nullptr) {
            AliasSlotState slot{};
            slot.slotId = nextSlotId++;
            slot.type = descriptor.type;
            slot.aliasClass = descriptor.aliasClass;
            slot.lastUseOrder = lifetime.lastUseOrder;
            slot.descriptor = descriptor;
            slots.push_back(slot);
            chosenSlot = &slots.back();

            plan.aliasAllocations.push_back(TransientAliasAllocation{
                .aliasSlot = chosenSlot->slotId,
                .type = descriptor.type,
                .aliasClass = descriptor.aliasClass,
                .requiredBufferSize = descriptor.transientBufferSize,
                .requiredBufferAlignment = std::max<VkDeviceSize>(1, descriptor.transientBufferAlignment),
                .requiredImageExtent = descriptor.transientImageExtent,
                .imageFormat = descriptor.transientImageFormat,
                .imageUsage = descriptor.transientImageUsage,
                .imageType = descriptor.transientImageType,
                .imageMipLevels = descriptor.transientImageMipLevels,
                .imageArrayLayers = descriptor.transientImageArrayLayers,
                .imageSamples = descriptor.transientImageSamples
                });
        }
        else {
            chosenSlot->lastUseOrder = lifetime.lastUseOrder;
        }

        plan.aliasSlotByResource.insert_or_assign(lifetime.resource, chosenSlot->slotId);

        auto allocIt = std::find_if(plan.aliasAllocations.begin(), plan.aliasAllocations.end(), [&](const TransientAliasAllocation& alloc) {
            return alloc.aliasSlot == chosenSlot->slotId;
            });
        if (allocIt == plan.aliasAllocations.end()) {
            return vkutil::VkExpected<CompiledTransientPlan>(
                vkutil::makeError("RenderTaskGraph::buildTransientPlan", VK_ERROR_INITIALIZATION_FAILED, "render_graph", "alias_slot_allocation_missing").context());
        }

        allocIt->resources.push_back(lifetime.resource);
        allocIt->requiredBufferSize = std::max(allocIt->requiredBufferSize, descriptor.transientBufferSize);
        allocIt->requiredBufferAlignment = std::max(allocIt->requiredBufferAlignment, std::max<VkDeviceSize>(1, descriptor.transientBufferAlignment));
        allocIt->requiredImageExtent.width = std::max(allocIt->requiredImageExtent.width, descriptor.transientImageExtent.width);
        allocIt->requiredImageExtent.height = std::max(allocIt->requiredImageExtent.height, descriptor.transientImageExtent.height);
        allocIt->requiredImageExtent.depth = std::max(allocIt->requiredImageExtent.depth, descriptor.transientImageExtent.depth);
    }

    return plan;
}

vkutil::VkExpected<std::vector<RenderTaskGraph::CompiledPass>> RenderTaskGraph::compile() const
{
    std::vector<Edge> edges{};
    std::vector<BarrierBatch> incomingBarriers{};
    std::vector<BarrierBatch> outgoingBarriers{};
    const auto build = buildDependenciesAndBarriers(edges, incomingBarriers, outgoingBarriers);
    if (!build.hasValue()) {
        return vkutil::VkExpected<std::vector<CompiledPass>>(build.context());
    }

    const auto scheduleResult = buildExecutionSchedule(edges);
    if (!scheduleResult.hasValue()) {
        return vkutil::VkExpected<std::vector<CompiledPass>>(scheduleResult.context());
    }

    const ExecutionSchedule& schedule = scheduleResult.value();

    std::vector<CompiledPass> compiled{};
    compiled.reserve(passes_.size());
    for (size_t order = 0; order < schedule.topologicalOrder.size(); ++order) {
        const PassId passId = schedule.topologicalOrder[order];
        compiled.push_back(CompiledPass{
            .id = passId,
            .scheduleOrder = order,
            .scheduleLevel = schedule.levelByPass[passId],
            .queueClass = passes_[passId].job.queueClass,
            .incomingBarriers = std::move(incomingBarriers[passId]),
            .outgoingBarriers = std::move(outgoingBarriers[passId])
            });
    }

    return compiled;
}

vkutil::VkExpected<RenderTaskGraph::CompiledTransientPlan> RenderTaskGraph::compileTransientPlan() const
{
    std::vector<Edge> edges{};
    std::vector<BarrierBatch> incomingBarriers{};
    std::vector<BarrierBatch> outgoingBarriers{};
    const auto buildResult = buildDependenciesAndBarriers(edges, incomingBarriers, outgoingBarriers);
    if (!buildResult.hasValue()) {
        return vkutil::VkExpected<CompiledTransientPlan>(buildResult.context());
    }

    const auto scheduleResult = buildExecutionSchedule(edges);
    if (!scheduleResult.hasValue()) {
        return vkutil::VkExpected<CompiledTransientPlan>(scheduleResult.context());
    }

    return buildTransientPlan(scheduleResult.value());
}

vkutil::VkExpected<SubmissionScheduler::FrameExecutionResult> RenderTaskGraph::execute(SubmissionScheduler& scheduler) const
{
    std::vector<Edge> edges{};
    std::vector<BarrierBatch> incomingBarriers{};
    std::vector<BarrierBatch> outgoingBarriers{};
    const auto build = buildDependenciesAndBarriers(edges, incomingBarriers, outgoingBarriers);
    if (!build.hasValue()) {
        return vkutil::VkExpected<SubmissionScheduler::FrameExecutionResult>(build.context());
    }

    const auto scheduleResult = buildExecutionSchedule(edges);
    if (!scheduleResult.hasValue()) {
        return vkutil::VkExpected<SubmissionScheduler::FrameExecutionResult>(scheduleResult.context());
    }

    const ExecutionSchedule& schedule = scheduleResult.value();

    scheduler.beginFrame();

    std::vector<SubmissionScheduler::JobId> jobIdsByPass{};
    jobIdsByPass.resize(passes_.size(), SubmissionScheduler::JobId{});

    std::vector<std::optional<vkutil::VkErrorContext>> recordContexts{};
    recordContexts.resize(passes_.size());

    for (const std::vector<PassId>& level : schedule.levels) {
        if (level.empty()) {
            continue;
        }

        PersistentRenderGraphTaskSystem::instance().run(level.size(), [&](uint32_t, size_t index) {
            const PassId passId = level[index];
            const PassNode& pass = passes_[passId];
            if (!pass.record) {
                recordContexts[passId] = vkutil::makeError("RenderTaskGraph::execute", VK_ERROR_INITIALIZATION_FAILED, "render_graph", "missing_record_callback").context();
                return;
            }

            const auto recordResult = pass.record(incomingBarriers[passId], outgoingBarriers[passId]);
            if (!recordResult.hasValue()) {
                recordContexts[passId] = recordResult.context();
            }
            });

        for (const PassId passId : level) {
            if (recordContexts[passId].has_value()) {
                return vkutil::VkExpected<SubmissionScheduler::FrameExecutionResult>(recordContexts[passId].value());
            }
        }
    }

    for (const PassId passId : schedule.topologicalOrder) {
        const PassNode& pass = passes_[passId];
        auto enqueueResult = scheduler.enqueueJob(pass.job);
        if (!enqueueResult.hasValue()) {
            return vkutil::VkExpected<SubmissionScheduler::FrameExecutionResult>(enqueueResult.context());
        }

        jobIdsByPass[passId] = enqueueResult.value();
    }

    for (const Edge& edge : edges) {
        const auto depResult = scheduler.enqueueDependency(
            jobIdsByPass[edge.producer],
            jobIdsByPass[edge.consumer],
            VK_NULL_HANDLE,
            edge.consumerWaitStage);
        if (!depResult.hasValue()) {
            return vkutil::VkExpected<SubmissionScheduler::FrameExecutionResult>(depResult.context());
        }
    }

    if (presentRequest_.has_value()) {
        const auto presentResult = scheduler.enqueuePresent(*presentRequest_);
        if (!presentResult.hasValue()) {
            return vkutil::VkExpected<SubmissionScheduler::FrameExecutionResult>(presentResult.context());
        }
    }

    return scheduler.executeFrame();
}
