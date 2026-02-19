 #include "VkResourceAllocators.h"

#include <algorithm>
#include <cmath>

namespace
{
    VkCommandPoolCreateFlags toPoolFlags(bool transient)
    {
        VkCommandPoolCreateFlags flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        if (transient) {
            flags |= VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        }
        return flags;
    }

    constexpr size_t toIndex(DescriptorAllocator::PoolClass c)
    {
        return static_cast<size_t>(c);
    }
}

CommandContextAllocator::CommandContextAllocator(VkDevice device, uint32_t framesInFlight, std::vector<QueueConfig> queues)
{
    const auto status = init(device, framesInFlight, std::move(queues));
    if (!status.hasValue()) {
        vkutil::throwVkError("CommandContextAllocator::CommandContextAllocator", status.error());
    }
}

vkutil::VkExpected<CommandContextAllocator> CommandContextAllocator::createResult(
    VkDevice device,
    uint32_t framesInFlight,
    std::vector<QueueConfig> queues)
{
    CommandContextAllocator out{};
    const auto status = out.init(device, framesInFlight, std::move(queues));
    if (!status.hasValue()) {
        return vkutil::VkExpected<CommandContextAllocator>(status.context());
    }
    return out;
}

vkutil::VkExpected<void> CommandContextAllocator::init(VkDevice device, uint32_t framesInFlight, std::vector<QueueConfig> queues)
{
    if (device == VK_NULL_HANDLE || framesInFlight == 0u || queues.empty()) {
        return vkutil::makeError("CommandContextAllocator::init", VK_ERROR_INITIALIZATION_FAILED, "command_allocator");
    }

    device_ = device;
    framesInFlight_ = framesInFlight;
    queueArenas_.clear();
    queueArenas_.reserve(queues.size());

    for (const QueueConfig& cfg : queues) {
        if (cfg.workerThreads == 0) {
            return vkutil::makeError("CommandContextAllocator::init", VK_ERROR_INITIALIZATION_FAILED, "command_allocator", "worker_threads_zero");
        }

        VulkanCommandArena::Config arenaConfig{};
        arenaConfig.device = device_;
        arenaConfig.queueFamilyIndex = cfg.queueFamilyIndex;
        arenaConfig.framesInFlight = framesInFlight_;
        arenaConfig.workerThreads = cfg.workerThreads;
        arenaConfig.preallocatePerFrame = cfg.preallocatePerFrame;
        arenaConfig.poolFlags = toPoolFlags(cfg.transient);

        auto arenaResult = VulkanCommandArena::createResult(arenaConfig);
        if (!arenaResult.hasValue()) {
            return vkutil::VkExpected<void>(arenaResult.context());
        }

        QueueArena qa{};
        qa.family = cfg.queueFamilyIndex;
        qa.workerThreads = cfg.workerThreads;
        qa.arena = std::move(arenaResult.value());
        queueArenas_.push_back(std::move(qa));
    }

    return {};
}

CommandContextAllocator::~CommandContextAllocator()
{
    std::scoped_lock lock(borrowedMutex_);
    borrowedByHandle_.clear();
}

vkutil::VkExpected<void> CommandContextAllocator::beginFrame(uint32_t frameIndex, uint64_t expectedGeneration)
{
    const auto generationStatus = checkGeneration(expectedGeneration, "CommandContextAllocator::beginFrame");
    if (!generationStatus.hasValue()) {
        return generationStatus;
    }
    if (frameIndex >= framesInFlight_) {
        return vkutil::makeError("CommandContextAllocator::beginFrame", VK_ERROR_INITIALIZATION_FAILED, "command_allocator");
    }

    currentFrame_ = frameIndex;
    for (QueueArena& qa : queueArenas_) {
        auto tokenRes = qa.arena.beginFrame(frameIndex, VulkanCommandArena::FrameWaitPolicy::Poll);
        if (!tokenRes.hasValue()) {
            return vkutil::VkExpected<void>(tokenRes.context());
        }
        qa.frameToken = tokenRes.value();
    }

    {
        std::scoped_lock lock(borrowedMutex_);
        borrowedByHandle_.clear();
    }

    return {};
}

vkutil::VkExpected<VkCommandBuffer> CommandContextAllocator::allocatePrimary(
    uint32_t queueSlot,
    const char* debugName,
    VkCommandBufferUsageFlags usage,
    uint64_t expectedGeneration,
    uint32_t workerIndex)
{
    const auto generationStatus = checkGeneration(expectedGeneration, "CommandContextAllocator::allocatePrimary");
    if (!generationStatus.hasValue()) {
        return vkutil::VkExpected<VkCommandBuffer>(generationStatus.context());
    }

    if (queueSlot >= queueArenas_.size() || framesInFlight_ == 0u) {
        return vkutil::VkExpected<VkCommandBuffer>(vkutil::makeError("CommandContextAllocator::allocatePrimary", VK_ERROR_INITIALIZATION_FAILED, "command_allocator").context());
    }

    QueueArena& qa = queueArenas_[queueSlot];
    if (!qa.frameToken.valid() || workerIndex >= qa.workerThreads) {
        return vkutil::VkExpected<VkCommandBuffer>(vkutil::makeError("CommandContextAllocator::allocatePrimary", VK_ERROR_INITIALIZATION_FAILED, "command_allocator", "invalid_worker_or_frame").context());
    }

    auto borrowedRes = qa.arena.acquirePrimary(qa.frameToken, workerIndex, usage);
    if (!borrowedRes.hasValue()) {
        return vkutil::VkExpected<VkCommandBuffer>(borrowedRes.context());
    }

    const VulkanCommandArena::BorrowedCommandBuffer borrowed = borrowedRes.value();
    if (debugName != nullptr) {
        vkutil::setObjectName(device_, VK_OBJECT_TYPE_COMMAND_BUFFER, borrowed.handle, debugName);
    }

    {
        std::scoped_lock lock(borrowedMutex_);
        borrowedByHandle_[borrowed.handle] = BorrowedRecord{ .queueSlot = queueSlot, .borrowed = borrowed };
    }

    return vkutil::VkExpected<VkCommandBuffer>(borrowed.handle);
}

vkutil::VkExpected<void> CommandContextAllocator::end(VkCommandBuffer commandBuffer, uint64_t expectedGeneration)
{
    const auto generationStatus = checkGeneration(expectedGeneration, "CommandContextAllocator::end");
    if (!generationStatus.hasValue()) {
        return generationStatus;
    }

    BorrowedRecord record{};
    {
        std::scoped_lock lock(borrowedMutex_);
        const auto it = borrowedByHandle_.find(commandBuffer);
        if (it == borrowedByHandle_.end()) {
            return vkutil::makeError("CommandContextAllocator::end", VK_ERROR_INITIALIZATION_FAILED, "command_allocator", "unknown_command_buffer");
        }
        record = it->second;
        borrowedByHandle_.erase(it);
    }

    if (record.queueSlot >= queueArenas_.size()) {
        return vkutil::makeError("CommandContextAllocator::end", VK_ERROR_INITIALIZATION_FAILED, "command_allocator", "invalid_queue_slot");
    }

    return queueArenas_[record.queueSlot].arena.endBorrowed(record.borrowed);
}

vkutil::VkExpected<void> CommandContextAllocator::checkGeneration(uint64_t expectedGeneration, const char* operation) const
{
    if (runtimeGeneration_ == 0 || expectedGeneration == 0) {
        return {};
    }
    if (runtimeGeneration_ != expectedGeneration) {
        return vkutil::makeError(operation, VK_ERROR_DEVICE_LOST, "command_allocator");
    }
    return {};
}

DescriptorAllocator::DescriptorAllocator(VkDevice device, uint32_t maxSetsPerPool, std::vector<PoolRatio> ratios)
{
    std::array<PoolClassConfig, static_cast<size_t>(PoolClass::Count)> classConfigs{};
    classConfigs[toIndex(PoolClass::Static)] = PoolClassConfig{ maxSetsPerPool, ratios, 0 };
    classConfigs[toIndex(PoolClass::Dynamic)] = PoolClassConfig{ maxSetsPerPool, ratios, 0 };
    classConfigs[toIndex(PoolClass::Bindless)] = PoolClassConfig{ maxSetsPerPool * 2u, ratios, VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT };

    const auto status = init(device, std::move(classConfigs));
    if (!status.hasValue()) {
        vkutil::throwVkError("DescriptorAllocator::DescriptorAllocator", status.error());
    }
}

DescriptorAllocator::DescriptorAllocator(VkDevice device, std::array<PoolClassConfig, static_cast<size_t>(PoolClass::Count)> classConfigs)
{
    const auto status = init(device, std::move(classConfigs));
    if (!status.hasValue()) {
        vkutil::throwVkError("DescriptorAllocator::DescriptorAllocator", status.error());
    }
}

vkutil::VkExpected<DescriptorAllocator> DescriptorAllocator::createResult(
    VkDevice device,
    uint32_t maxSetsPerPool,
    std::vector<PoolRatio> ratios)
{
    std::array<PoolClassConfig, static_cast<size_t>(PoolClass::Count)> classConfigs{};
    classConfigs[toIndex(PoolClass::Static)] = PoolClassConfig{ maxSetsPerPool, ratios, 0 };
    classConfigs[toIndex(PoolClass::Dynamic)] = PoolClassConfig{ maxSetsPerPool, ratios, 0 };
    classConfigs[toIndex(PoolClass::Bindless)] = PoolClassConfig{ maxSetsPerPool * 2u, ratios, VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT };
    return createResult(device, std::move(classConfigs));
}

vkutil::VkExpected<DescriptorAllocator> DescriptorAllocator::createResult(
    VkDevice device,
    std::array<PoolClassConfig, static_cast<size_t>(PoolClass::Count)> classConfigs)
{
    DescriptorAllocator out{};
    const auto status = out.init(device, std::move(classConfigs));
    if (!status.hasValue()) {
        return vkutil::VkExpected<DescriptorAllocator>(status.context());
    }
    return out;
}

vkutil::VkExpected<void> DescriptorAllocator::init(
    VkDevice device,
    std::array<PoolClassConfig, static_cast<size_t>(PoolClass::Count)> classConfigs)
{
    if (device == VK_NULL_HANDLE) {
        return vkutil::makeError("DescriptorAllocator::init", VK_ERROR_INITIALIZATION_FAILED, "descriptor_allocator");
    }

    for (size_t i = 0; i < classConfigs.size(); ++i) {
        if (classConfigs[i].maxSetsPerPool == 0 || classConfigs[i].ratios.empty()) {
            return vkutil::makeError("DescriptorAllocator::init", VK_ERROR_INITIALIZATION_FAILED, "descriptor_allocator", "invalid_pool_class_config");
        }
    }

    device_ = device;
    for (size_t i = 0; i < classConfigs.size(); ++i) {
        banks_[i].config = std::move(classConfigs[i]);
    }

    return {};
}

DescriptorAllocator::~DescriptorAllocator()
{
    for (auto& bank : banks_) {
        for (VkDescriptorPool pool : bank.readyPools) {
            if (pool != VK_NULL_HANDLE) {
                vkDestroyDescriptorPool(device_, pool, nullptr);
            }
        }
        for (VkDescriptorPool pool : bank.usedPools) {
            if (pool != VK_NULL_HANDLE) {
                vkDestroyDescriptorPool(device_, pool, nullptr);
            }
        }
        for (VkDescriptorPool pool : bank.pendingRecyclePools) {
            if (pool != VK_NULL_HANDLE) {
                vkDestroyDescriptorPool(device_, pool, nullptr);
            }
        }
    }
}

vkutil::VkExpected<void> DescriptorAllocator::resetBank(PoolBank& bank, uint64_t frameIndex)
{
    for (VkDescriptorPool pool : bank.pendingRecyclePools) {
        bank.readyPools.push_back(pool);
    }
    bank.pendingRecyclePools.clear();

    for (VkDescriptorPool pool : bank.usedPools) {
        const VkResult res = vkResetDescriptorPool(device_, pool, 0);
        if (res != VK_SUCCESS) {
            return vkutil::checkResult(res, "vkResetDescriptorPool", "descriptor_allocator", nullptr, frameIndex);
        }
        bank.readyPools.push_back(pool);
    }
    bank.usedPools.clear();
    return {};
}

vkutil::VkExpected<void> DescriptorAllocator::beginFrame(uint32_t frameIndex)
{
    for (auto& bank : banks_) {
        const auto status = resetBank(bank, frameIndex);
        if (!status.hasValue()) {
            return status;
        }
    }
    return {};
}

vkutil::VkExpected<VkDescriptorPool> DescriptorAllocator::createPool(PoolClass poolClass, uint64_t frameIndex)
{
    const PoolBank& bank = banks_[toIndex(poolClass)];

    std::vector<VkDescriptorPoolSize> sizes;
    sizes.reserve(bank.config.ratios.size());
    for (const PoolRatio& ratio : bank.config.ratios) {
        VkDescriptorPoolSize size{};
        size.type = ratio.type;
        size.descriptorCount = std::max(1u, static_cast<uint32_t>(std::ceil(ratio.ratio * static_cast<float>(bank.config.maxSetsPerPool))));
        sizes.push_back(size);
    }

    VkDescriptorPoolCreateInfo info{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    info.maxSets = bank.config.maxSetsPerPool;
    info.poolSizeCount = static_cast<uint32_t>(sizes.size());
    info.pPoolSizes = sizes.data();
    info.flags = bank.config.flags;

    VkDescriptorPool pool = VK_NULL_HANDLE;
    const VkResult res = vkCreateDescriptorPool(device_, &info, nullptr, &pool);
    if (res != VK_SUCCESS) {
        static_cast<void>(vkutil::checkResult(res, "vkCreateDescriptorPool", "descriptor_allocator", nullptr, frameIndex));
        return vkutil::VkExpected<VkDescriptorPool>(res);
    }

    return vkutil::VkExpected<VkDescriptorPool>(pool);
}

vkutil::VkExpected<VkDescriptorSet> DescriptorAllocator::allocate(
    VkDescriptorSetLayout layout,
    const char* debugName,
    uint64_t frameIndex,
    PoolClass poolClass)
{
    std::vector<VkDescriptorSetLayout> layouts{ layout };
    std::vector<VkDescriptorSet> out;
    const auto status = allocateMany(layouts, out, frameIndex, poolClass);
    if (!status.hasValue()) {
        return vkutil::VkExpected<VkDescriptorSet>(status.error());
    }
    if (debugName != nullptr && !out.empty()) {
        vkutil::setObjectName(device_, VK_OBJECT_TYPE_DESCRIPTOR_SET, out[0], debugName);
    }
    return vkutil::VkExpected<VkDescriptorSet>(out[0]);
}

vkutil::VkExpected<void> DescriptorAllocator::allocateMany(
    const std::vector<VkDescriptorSetLayout>& layouts,
    std::vector<VkDescriptorSet>& outSets,
    uint64_t frameIndex,
    PoolClass poolClass)
{
    if (layouts.empty()) {
        outSets.clear();
        return {};
    }

    PoolBank& bank = banks_[toIndex(poolClass)];

    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (!bank.readyPools.empty()) {
        pool = bank.readyPools.back();
        bank.readyPools.pop_back();
    } else {
        const auto created = createPool(poolClass, frameIndex);
        if (!created.hasValue()) {
            return vkutil::VkExpected<void>(created.error());
        }
        pool = created.value();
    }

    outSets.assign(layouts.size(), VK_NULL_HANDLE);

    VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = pool;
    ai.descriptorSetCount = static_cast<uint32_t>(layouts.size());
    ai.pSetLayouts = layouts.data();

    VkResult res = vkAllocateDescriptorSets(device_, &ai, outSets.data());
    if (res == VK_ERROR_OUT_OF_POOL_MEMORY || res == VK_ERROR_FRAGMENTED_POOL) {
        bank.pendingRecyclePools.push_back(pool);

        const auto replacement = createPool(poolClass, frameIndex);
        if (!replacement.hasValue()) {
            return vkutil::VkExpected<void>(replacement.error());
        }
        pool = replacement.value();
        ai.descriptorPool = pool;
        res = vkAllocateDescriptorSets(device_, &ai, outSets.data());
    }

    if (res != VK_SUCCESS) {
        bank.readyPools.push_back(pool);
        return vkutil::checkResult(res, "vkAllocateDescriptorSets", "descriptor_allocator", nullptr, frameIndex);
    }

    bank.usedPools.push_back(pool);
    return {};
}

vkutil::VkExpected<void> DescriptorAllocator::resetClass(PoolClass poolClass, uint64_t frameIndex)
{
    return resetBank(banks_[toIndex(poolClass)], frameIndex);
}
