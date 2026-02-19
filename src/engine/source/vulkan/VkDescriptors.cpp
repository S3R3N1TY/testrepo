// VkDescriptors.cpp
#include <string>
#include <limits>
#include <optional>
#include <algorithm>
#include <thread>

 #include "VkDescriptors.h"
 #include "VkUtils.h"
 #include "DeferredDeletionService.h"


namespace {
thread_local std::optional<uint32_t> g_descriptorThreadSlot{};
std::atomic<uint32_t> g_nextDescriptorThreadSlot{ 1 };

uint32_t boundedScale(uint32_t value, uint32_t num, uint32_t den, uint32_t maxValue)
{
    if (den == 0) return value;
    const uint64_t scaled = (static_cast<uint64_t>(value) * num + (den - 1)) / den;
    return static_cast<uint32_t>(std::min<uint64_t>(std::max<uint64_t>(scaled, value + 1), maxValue));
}

VkDescriptorPoolSize scalePoolSize(const VkDescriptorPoolSize& in, uint32_t num, uint32_t den)
{
    VkDescriptorPoolSize out = in;
    out.descriptorCount = std::max<uint32_t>(1, static_cast<uint32_t>((static_cast<uint64_t>(in.descriptorCount) * num + (den - 1)) / den));
    return out;
}

uint64_t hashCombine(uint64_t seed, uint64_t value)
{
    constexpr uint64_t kMul = 0x9E3779B97F4A7C15ULL;
    seed ^= value + kMul + (seed << 6) + (seed >> 2);
    return seed;
}

uint64_t currentThreadKey()
{
    if (!g_descriptorThreadSlot.has_value()) {
        g_descriptorThreadSlot = g_nextDescriptorThreadSlot.fetch_add(1, std::memory_order_relaxed);
    }
    constexpr uint64_t kSlotTagMask = (1ull << 63);
    return kSlotTagMask | static_cast<uint64_t>(g_descriptorThreadSlot.value());
}
}

void DescriptorSetAllocator::setCurrentThreadSlot(uint32_t slot) noexcept
{
    g_descriptorThreadSlot = slot;
}

void DescriptorSetAllocator::clearCurrentThreadSlot() noexcept
{
    g_descriptorThreadSlot.reset();
}

VulkanDescriptorSetLayout::VulkanDescriptorSetLayout(
    VkDevice device,
    const std::vector<VkDescriptorSetLayoutBinding>& bindings,
    VkDescriptorSetLayoutCreateFlags flags,
    const void* pNext)
    : handle()
{
    if (device == VK_NULL_HANDLE) {
        throw std::runtime_error("VulkanDescriptorSetLayout: device is VK_NULL_HANDLE");
    }

    VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    ci.pNext = pNext;
    ci.flags = flags;
    ci.bindingCount = static_cast<uint32_t>(bindings.size());
    ci.pBindings = bindings.empty() ? nullptr : bindings.data();

    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    const VkResult res = vkCreateDescriptorSetLayout(device, &ci, nullptr, &layout);
    if (res != VK_SUCCESS) {
        vkutil::throwVkError("vkCreateDescriptorSetLayout", res);
    }

    handle = DeferredDeletionService::instance().makeDeferredHandle<VkDescriptorSetLayout, PFN_vkDestroyDescriptorSetLayout>(device, layout, vkDestroyDescriptorSetLayout);
}

VulkanDescriptorPool::VulkanDescriptorPool(
    VkDevice device,
    const std::vector<VkDescriptorPoolSize>& poolSizes,
    uint32_t maxSets,
    VkDescriptorPoolCreateFlags flags,
    const void* pNext)
    : handle()
{
    if (device == VK_NULL_HANDLE) {
        throw std::runtime_error("VulkanDescriptorPool: device is VK_NULL_HANDLE");
    }
    if (maxSets == 0) {
        throw std::runtime_error("VulkanDescriptorPool: maxSets must be > 0");
    }
    if (poolSizes.empty()) {
        throw std::runtime_error("VulkanDescriptorPool: poolSizes is empty");
    }

    VkDescriptorPoolCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    ci.pNext = pNext;
    ci.flags = flags;
    ci.maxSets = maxSets;
    ci.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    ci.pPoolSizes = poolSizes.data();

    VkDescriptorPool pool = VK_NULL_HANDLE;
    const VkResult res = vkCreateDescriptorPool(device, &ci, nullptr, &pool);
    if (res != VK_SUCCESS) {
        vkutil::throwVkError("vkCreateDescriptorPool", res);
    }

    handle = DeferredDeletionService::instance().makeDeferredHandle<VkDescriptorPool, PFN_vkDestroyDescriptorPool>(device, pool, vkDestroyDescriptorPool);
}

void VulkanDescriptorPool::allocateSets(
    const std::vector<VkDescriptorSetLayout>& layouts,
    std::vector<VkDescriptorSet>& outSets) const
{
    if (layouts.empty()) {
        outSets.clear();
        return;
    }

    outSets.assign(layouts.size(), VK_NULL_HANDLE);
    allocateSetsImpl(static_cast<uint32_t>(layouts.size()), layouts.data(), outSets.data());
}

void VulkanDescriptorPool::allocateSetsImpl(
    uint32_t count,
    const VkDescriptorSetLayout* layouts,
    VkDescriptorSet* outSets) const
{
    if (count == 0) return;

    if (!handle) {
        throw std::runtime_error("VulkanDescriptorPool::allocateSetsImpl: called on invalid pool");
    }
    if (!layouts || !outSets) {
        throw std::runtime_error("VulkanDescriptorPool::allocateSetsImpl: received null pointer(s)");
    }

    VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = handle.get();
    ai.descriptorSetCount = count;
    ai.pSetLayouts = layouts;

    const VkResult res = vkAllocateDescriptorSets(handle.getDevice(), &ai, outSets);
    if (res != VK_SUCCESS) {
        vkutil::throwVkError("vkAllocateDescriptorSets", res);
    }
}

void VulkanDescriptorPool::freeSets(const std::vector<VkDescriptorSet>& sets) const
{
    if (sets.empty()) {
        return;
    }
    if (!valid()) {
        throw std::runtime_error("VulkanDescriptorPool::freeSets called on invalid pool");
    }

    const VkResult res = vkFreeDescriptorSets(handle.getDevice(), handle.get(), static_cast<uint32_t>(sets.size()), sets.data());
    if (res != VK_SUCCESS) {
        vkutil::throwVkError("vkFreeDescriptorSets", res);
    }
}

void VulkanDescriptorPool::reset() const
{
    if (!valid()) {
        return;
    }

    const VkResult res = vkResetDescriptorPool(handle.getDevice(), handle.get(), 0);
    if (res != VK_SUCCESS) {
        vkutil::throwVkError("vkResetDescriptorPool", res);
    }
}

uint64_t DescriptorSetAllocator::Profile::key() const noexcept
{
    return static_cast<uint64_t>(profileId);
}

uint64_t DescriptorSetAllocator::Profile::configHash() const noexcept
{
    uint64_t seed = hashCombine(0, static_cast<uint64_t>(setsPerPool));
    seed = hashCombine(seed, static_cast<uint64_t>(poolFlags));
    seed = hashCombine(seed, static_cast<uint64_t>(transientFrame ? 1u : 0u));
    seed = hashCombine(seed, static_cast<uint64_t>(allowFreeIndividualSets ? 1u : 0u));
    seed = hashCombine(seed, static_cast<uint64_t>(poolClass));
    seed = hashCombine(seed, static_cast<uint64_t>(compactionIntervalFrames));
    seed = hashCombine(seed, static_cast<uint64_t>(maxPoolAgeBeforeRecycle));
    seed = hashCombine(seed, static_cast<uint64_t>(staleThreadEpochsBeforeReclaim));
    seed = hashCombine(seed, static_cast<uint64_t>(maxTrackedTransientThreads));
    seed = hashCombine(seed, static_cast<uint64_t>(lowOccupancyRecycleThresholdPercent));
    seed = hashCombine(seed, static_cast<uint64_t>(targetOccupancyPercent));

    std::vector<VkDescriptorPoolSize> sortedSizes = poolSizes;
    std::sort(sortedSizes.begin(), sortedSizes.end(), [](const VkDescriptorPoolSize& a, const VkDescriptorPoolSize& b) {
        if (a.type != b.type) {
            return a.type < b.type;
        }
        return a.descriptorCount < b.descriptorCount;
    });

    for (const VkDescriptorPoolSize& size : sortedSizes) {
        seed = hashCombine(seed, static_cast<uint64_t>(size.type));
        seed = hashCombine(seed, static_cast<uint64_t>(size.descriptorCount));
    }

    return seed;
}

bool DescriptorSetAllocator::Profile::equivalentConfig(const Profile& other) const noexcept
{
    return setsPerPool == other.setsPerPool
        && poolFlags == other.poolFlags
        && transientFrame == other.transientFrame
        && allowFreeIndividualSets == other.allowFreeIndividualSets
        && poolClass == other.poolClass
        && compactionIntervalFrames == other.compactionIntervalFrames
        && maxPoolAgeBeforeRecycle == other.maxPoolAgeBeforeRecycle
        && staleThreadEpochsBeforeReclaim == other.staleThreadEpochsBeforeReclaim
        && maxTrackedTransientThreads == other.maxTrackedTransientThreads
        && lowOccupancyRecycleThresholdPercent == other.lowOccupancyRecycleThresholdPercent
        && targetOccupancyPercent == other.targetOccupancyPercent
        && configHash() == other.configHash();
}

DescriptorSetAllocator::DescriptorSetAllocator(VkDevice device, VkPhysicalDevice physicalDevice)
    : device_(device)
{
    if (device_ == VK_NULL_HANDLE) {
        throw std::runtime_error("DescriptorSetAllocator: device is VK_NULL_HANDLE");
    }

    descriptorIndexingProperties_ = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES };
    if (physicalDevice != VK_NULL_HANDLE) {
        VkPhysicalDeviceProperties2 props2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        props2.pNext = &descriptorIndexingProperties_;
        vkGetPhysicalDeviceProperties2(physicalDevice, &props2);
        limits_ = props2.properties.limits;
        hasDeviceLimits_ = true;
    }
}

DescriptorSetAllocator::~DescriptorSetAllocator() noexcept
{
    std::unique_lock<std::shared_mutex> lock(mutex_);
    profiles_.clear();
    device_ = VK_NULL_HANDLE;
}

uint64_t DescriptorSetAllocator::registerProfile(const Profile& profile)
{
    std::unique_lock<std::shared_mutex> mapLock(mutex_);
    if (profile.poolSizes.empty()) {
        throw std::runtime_error("DescriptorSetAllocator::registerProfile: poolSizes cannot be empty");
    }
    if (profile.setsPerPool == 0) {
        throw std::runtime_error("DescriptorSetAllocator::registerProfile: setsPerPool must be > 0");
    }
    if (profile.lowOccupancyRecycleThresholdPercent > 100 || profile.targetOccupancyPercent > 100) {
        throw std::runtime_error("DescriptorSetAllocator::registerProfile: occupancy percents must be <= 100");
    }
    if (profile.maxTrackedTransientThreads == 0) {
        throw std::runtime_error("DescriptorSetAllocator::registerProfile: maxTrackedTransientThreads must be > 0");
    }

    const uint32_t maxSetsCap = maxSetsPerPoolCap(profile);
    if (profile.setsPerPool > maxSetsCap) {
        throw std::runtime_error("DescriptorSetAllocator::registerProfile: setsPerPool exceeds device-aware pool cap");
    }

    const bool updateAfterBind = (profile.poolFlags & VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT) != 0;
    uint64_t descriptorTotal = 0;
    for (const VkDescriptorPoolSize& poolSize : profile.poolSizes) {
        const uint32_t perSetLimit = descriptorLimitForType(poolSize.type, updateAfterBind);
        if (perSetLimit != UINT32_MAX) {
            const uint64_t maxDescriptorsForType = static_cast<uint64_t>(perSetLimit) * static_cast<uint64_t>(profile.setsPerPool);
            if (poolSize.descriptorCount > maxDescriptorsForType) {
                throw std::runtime_error("DescriptorSetAllocator::registerProfile: pool descriptor count exceeds device descriptor-set limits");
            }
        }
        descriptorTotal += poolSize.descriptorCount;
    }

    if (updateAfterBind
        && descriptorIndexingProperties_.maxUpdateAfterBindDescriptorsInAllPools > 0
        && descriptorTotal > descriptorIndexingProperties_.maxUpdateAfterBindDescriptorsInAllPools) {
        throw std::runtime_error("DescriptorSetAllocator::registerProfile: update-after-bind descriptor total exceeds device limits");
    }

    const uint64_t key = profile.key();
    const uint64_t profileConfigHash = profile.configHash();

    auto existing = profiles_.find(key);
    if (existing != profiles_.end()) {
        const std::shared_ptr<ProfileState>& existingState = existing->second;
        std::lock_guard<std::mutex> profileLock(existingState->mutex);
        if (!existingState->profile.equivalentConfig(profile)) {
            throw std::runtime_error("DescriptorSetAllocator::registerProfile: profileId conflict with mismatched configuration");
        }
        return key;
    }

    auto state = std::make_shared<ProfileState>();
    std::lock_guard<std::mutex> profileLock(state->mutex);
    state->profile = profile;
    state->configHash = profileConfigHash;
    state->activeSetsPerPool = profile.setsPerPool;
    state->outOfPoolStreakByClass.fill(0);
    state->fragmentedStreakByClass.fill(0);
    state->epoch = 0;
    profiles_[key] = state;
    return key;
}

size_t DescriptorSetAllocator::classIndex(PoolBucket::SizeClass sizeClass) noexcept
{
    return static_cast<size_t>(sizeClass);
}

uint32_t DescriptorSetAllocator::growthNumerator(AllocationRequest::AllocationClassHint hint) noexcept
{
    switch (hint) {
    case AllocationRequest::AllocationClassHint::FrameTransient: return 5;
    case AllocationRequest::AllocationClassHint::Material: return 3;
    case AllocationRequest::AllocationClassHint::Bindless: return 2;
    case AllocationRequest::AllocationClassHint::Generic: default: return 3;
    }
}

uint32_t DescriptorSetAllocator::growthDenominator(AllocationRequest::AllocationClassHint hint) noexcept
{
    switch (hint) {
    case AllocationRequest::AllocationClassHint::FrameTransient: return 2;
    case AllocationRequest::AllocationClassHint::Material: return 2;
    case AllocationRequest::AllocationClassHint::Bindless: return 1;
    case AllocationRequest::AllocationClassHint::Generic: default: return 2;
    }
}

uint32_t DescriptorSetAllocator::growthNumerator(Profile::PoolClass poolClass) noexcept
{
    switch (poolClass) {
    case Profile::PoolClass::FrameTransient: return 3;
    case Profile::PoolClass::Material: return 4;
    case Profile::PoolClass::Bindless: return 5;
    case Profile::PoolClass::Custom: default: return 3;
    }
}

uint32_t DescriptorSetAllocator::growthDenominator(Profile::PoolClass poolClass) noexcept
{
    switch (poolClass) {
    case Profile::PoolClass::FrameTransient: return 2;
    case Profile::PoolClass::Material: return 3;
    case Profile::PoolClass::Bindless: return 2;
    case Profile::PoolClass::Custom: default: return 2;
    }
}

uint32_t DescriptorSetAllocator::occupancyPercent(const PoolBucket& bucket) noexcept
{
    if (bucket.maxSets == 0) {
        return 0;
    }
    const uint64_t pct = (static_cast<uint64_t>(bucket.liveSets) * 100u) / bucket.maxSets;
    return static_cast<uint32_t>(std::min<uint64_t>(100, pct));
}

uint32_t DescriptorSetAllocator::descriptorLimitForType(VkDescriptorType type, bool updateAfterBind) const noexcept
{
    if (!hasDeviceLimits_) {
        return UINT32_MAX;
    }

    if (updateAfterBind) {
        switch (type) {
        case VK_DESCRIPTOR_TYPE_SAMPLER: return limits_.maxDescriptorSetUpdateAfterBindSamplers;
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: return limits_.maxDescriptorSetUpdateAfterBindSampledImages;
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: return limits_.maxDescriptorSetUpdateAfterBindSampledImages;
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: return limits_.maxDescriptorSetUpdateAfterBindStorageImages;
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: return limits_.maxDescriptorSetUpdateAfterBindUniformTexelBuffers;
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: return limits_.maxDescriptorSetUpdateAfterBindStorageTexelBuffers;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: return limits_.maxDescriptorSetUpdateAfterBindUniformBuffers;
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: return limits_.maxDescriptorSetUpdateAfterBindStorageBuffers;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: return limits_.maxDescriptorSetUniformBuffersDynamic;
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: return limits_.maxDescriptorSetStorageBuffersDynamic;
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: return limits_.maxDescriptorSetUpdateAfterBindInputAttachments;
        default: return UINT32_MAX;
        }
    }

    switch (type) {
    case VK_DESCRIPTOR_TYPE_SAMPLER: return limits_.maxDescriptorSetSamplers;
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: return limits_.maxDescriptorSetSampledImages;
    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: return limits_.maxDescriptorSetSampledImages;
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: return limits_.maxDescriptorSetStorageImages;
    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: return limits_.maxDescriptorSetUniformTexelBuffers;
    case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: return limits_.maxDescriptorSetStorageTexelBuffers;
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: return limits_.maxDescriptorSetUniformBuffers;
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: return limits_.maxDescriptorSetStorageBuffers;
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: return limits_.maxDescriptorSetUniformBuffersDynamic;
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: return limits_.maxDescriptorSetStorageBuffersDynamic;
    case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: return limits_.maxDescriptorSetInputAttachments;
    default: return UINT32_MAX;
    }
}

uint32_t DescriptorSetAllocator::maxSetsPerPoolCap(const Profile& profile) const noexcept
{
    uint32_t cap = 4096;
    if (!hasDeviceLimits_) {
        return cap;
    }

    const uint32_t boundSetScaled = std::max<uint32_t>(64, limits_.maxBoundDescriptorSets * 64u);
    cap = std::min(cap, boundSetScaled);

    if ((profile.poolFlags & VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT) != 0
        && descriptorIndexingProperties_.maxUpdateAfterBindDescriptorsInAllPools > 0) {
        cap = std::min<uint32_t>(cap, descriptorIndexingProperties_.maxUpdateAfterBindDescriptorsInAllPools);
    }

    return std::max<uint32_t>(1u, cap);
}

uint32_t DescriptorSetAllocator::clampedSetsPerPool(const ProfileState& state, uint32_t requestedSets) const noexcept
{
    return std::max<uint32_t>(1u, std::min(requestedSets, maxSetsPerPoolCap(state.profile)));
}

void DescriptorSetAllocator::rebalancePoolsForCompaction(ProfileState& state, std::array<std::deque<PoolBucket>, 3>& buckets)
{
    for (auto& queue : buckets) {
        std::vector<PoolBucket> ordered;
        ordered.reserve(queue.size());
        while (!queue.empty()) {
            ordered.push_back(std::move(queue.front()));
            queue.pop_front();
        }
        std::sort(ordered.begin(), ordered.end(), [&](const PoolBucket& a, const PoolBucket& b) {
            const uint32_t occA = occupancyPercent(a);
            const uint32_t occB = occupancyPercent(b);
            const uint32_t target = std::min<uint32_t>(100, std::max<uint32_t>(1, state.profile.targetOccupancyPercent));
            const uint32_t distA = (occA > target) ? (occA - target) : (target - occA);
            const uint32_t distB = (occB > target) ? (occB - target) : (target - occB);
            if (distA != distB) {
                return distA < distB;
            }
            if (occA != occB) {
                return occA > occB;
            }
            return a.lastUsedEpoch > b.lastUsedEpoch;
        });

        for (auto& bucket : ordered) {
            queue.push_back(std::move(bucket));
        }
    }
}

void DescriptorSetAllocator::runCompaction(ProfileState& state, uint32_t frameIndex)
{
    if (state.profile.compactionIntervalFrames == 0) {
        return;
    }
    if ((frameIndex % state.profile.compactionIntervalFrames) != 0) {
        return;
    }

    ++state.stats.compactionRuns;
    rebalancePoolsForCompaction(state, state.usedPoolsByClass);

    const uint32_t lowThreshold = std::min<uint32_t>(100, state.profile.lowOccupancyRecycleThresholdPercent);
    const uint32_t ageThreshold = std::max<uint32_t>(1, state.profile.maxPoolAgeBeforeRecycle);

    for (size_t idx = 0; idx < state.usedPoolsByClass.size(); ++idx) {
        auto& used = state.usedPoolsByClass[idx];
        for (auto it = used.begin(); it != used.end();) {
            const uint32_t occ = occupancyPercent(*it);
            const uint64_t age = (state.epoch > it->lastUsedEpoch) ? (state.epoch - it->lastUsedEpoch) : 0;
            const bool staleLowOccupancy = (occ <= lowThreshold) && (age >= ageThreshold);
            if (!staleLowOccupancy) {
                ++it;
                continue;
            }

            if (it->liveSets != 0) {
                ++it;
                continue;
            }

            it->pool.reset();
            state.freePoolsByClass[idx].push_back(std::move(*it));
            it = used.erase(it);
            ++state.stats.recycledLowOccupancyPools;
            ++state.stats.retiredPools;
        }
    }
}

DescriptorSetAllocator::PoolBucket::SizeClass DescriptorSetAllocator::classifyRequest(const AllocationRequest& request) noexcept
{
    const size_t layoutCount = request.layouts.size();
    if (request.classHint == AllocationRequest::AllocationClassHint::Bindless || layoutCount >= 16) {
        return PoolBucket::SizeClass::Large;
    }
    if (request.classHint == AllocationRequest::AllocationClassHint::FrameTransient || layoutCount <= 2) {
        return PoolBucket::SizeClass::Small;
    }
    return PoolBucket::SizeClass::Medium;
}

DescriptorSetAllocator::PoolBucket DescriptorSetAllocator::createPool(
    ProfileState& state,
    PoolBucket::SizeClass sizeClass,
    uint32_t frameIndex)
{
    const Profile& profile = state.profile;
    VkDescriptorPoolCreateFlags flags = profile.poolFlags;
    if (profile.allowFreeIndividualSets) {
        flags |= VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    }

    uint32_t setsPerPool = (state.activeSetsPerPool > 0) ? state.activeSetsPerPool : profile.setsPerPool;

    if (sizeClass == PoolBucket::SizeClass::Small) {
        setsPerPool = std::max<uint32_t>(8, setsPerPool / 2);
    } else if (sizeClass == PoolBucket::SizeClass::Large) {
        const uint32_t doubled = setsPerPool > (UINT32_MAX / 2) ? UINT32_MAX : setsPerPool * 2;
        setsPerPool = doubled;
    }

    setsPerPool = clampedSetsPerPool(state, setsPerPool);

    std::vector<VkDescriptorPoolSize> poolSizes = profile.poolSizes;
    const uint32_t num = std::max<uint32_t>(1, setsPerPool);
    const uint32_t den = std::max<uint32_t>(1, profile.setsPerPool);
    for (auto& size : poolSizes) {
        size = scalePoolSize(size, num, den);
    }

    VulkanDescriptorPool pool(device_, poolSizes, setsPerPool, flags);
    return PoolBucket{
        .pool = std::move(pool),
        .liveSets = 0,
        .frameIndex = frameIndex,
        .retireEpoch = 0,
        .sizeClass = sizeClass,
        .maxSets = setsPerPool,
        .lastUsedEpoch = state.epoch
    };
}

DescriptorSetAllocator::PoolAllocationOutcome DescriptorSetAllocator::allocateFromPool(
    ProfileState& state,
    PoolBucket& bucket,
    const AllocationRequest& request,
    std::unique_lock<std::mutex>* stateLock)
{
    const size_t bucketIdx = classIndex(bucket.sizeClass);
    {
        std::lock_guard<std::mutex> statsLock(state.mutex);
        ++state.stats.allocationAttempts;
    }

    PoolAllocationOutcome outcome{};
    outcome.status = PoolAllocationStatus::Fatal;
    outcome.result = VK_ERROR_UNKNOWN;
    outcome.allocation.profileKey = request.profileKey;

    std::vector<VkDescriptorSet> sets(request.layouts.size(), VK_NULL_HANDLE);

    VkDescriptorSetVariableDescriptorCountAllocateInfo variableInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO
    };

    VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = bucket.pool.get();
    ai.descriptorSetCount = static_cast<uint32_t>(request.layouts.size());
    ai.pSetLayouts = request.layouts.data();

    if (!request.variableDescriptorCounts.empty()) {
        if (request.variableDescriptorCounts.size() != request.layouts.size()) {
            outcome.status = PoolAllocationStatus::Fatal;
            outcome.result = VK_ERROR_INITIALIZATION_FAILED;
            std::lock_guard<std::mutex> statsLock(state.mutex);
            ++state.stats.failedAllocations;
            ++state.stats.allocationRetries;
            return outcome;
        }
        variableInfo.descriptorSetCount = ai.descriptorSetCount;
        variableInfo.pDescriptorCounts = request.variableDescriptorCounts.data();
        ai.pNext = &variableInfo;
    }

    if (stateLock != nullptr) {
        stateLock->unlock();
    }
    const VkResult res = vkAllocateDescriptorSets(device_, &ai, sets.data());
    if (stateLock != nullptr) {
        stateLock->lock();
    }

    outcome.result = res;

    if (res == VK_SUCCESS) {
        bucket.liveSets += ai.descriptorSetCount;
        bucket.lastUsedEpoch = state.epoch;
        {
            std::lock_guard<std::mutex> statsLock(state.mutex);
            state.stats.liveSets += ai.descriptorSetCount;
            state.stats.peakLiveSets = std::max(state.stats.peakLiveSets, state.stats.liveSets);
            ++state.stats.successfulAllocations;
        }
        outcome.allocation.pool = bucket.pool.get();
        outcome.allocation.sets = std::move(sets);
        state.outOfPoolStreakByClass[bucketIdx] = 0;
        state.fragmentedStreakByClass[bucketIdx] = 0;
        outcome.status = PoolAllocationStatus::Success;
        return outcome;
    }

    if (res == VK_ERROR_OUT_OF_POOL_MEMORY) {
        {
            std::lock_guard<std::mutex> statsLock(state.mutex);
            ++state.stats.outOfPoolEvents;
        }
        ++state.outOfPoolStreakByClass[bucketIdx];
        outcome.status = PoolAllocationStatus::OutOfPoolMemory;
    } else {
        state.outOfPoolStreakByClass[bucketIdx] = 0;
    }

    if (res == VK_ERROR_FRAGMENTED_POOL) {
        {
            std::lock_guard<std::mutex> statsLock(state.mutex);
            ++state.stats.fragmentedEvents;
        }
        ++state.fragmentedStreakByClass[bucketIdx];
        outcome.status = PoolAllocationStatus::FragmentedPool;
    } else {
        state.fragmentedStreakByClass[bucketIdx] = 0;
    }

    if (outcome.status == PoolAllocationStatus::Fatal) {
        outcome.status = PoolAllocationStatus::Fatal;
    }

    {
        std::lock_guard<std::mutex> statsLock(state.mutex);
        ++state.stats.failedAllocations;
        ++state.stats.allocationRetries;
    }
    return outcome;
}

vkutil::VkExpected<DescriptorSetAllocator::AllocationResult> DescriptorSetAllocator::allocateResult(const AllocationRequest& request)
{
    allocationAttempts_.fetch_add(1, std::memory_order_relaxed);

    std::shared_ptr<ProfileState> state;
    {
        std::shared_lock<std::shared_mutex> mapReadLock(mutex_);
        auto it = profiles_.find(request.profileKey);
        if (it == profiles_.end()) {
            failedAllocations_.fetch_add(1, std::memory_order_relaxed);
            return vkutil::VkExpected<AllocationResult>(vkutil::makeError("DescriptorSetAllocator::allocateResult", VK_ERROR_INITIALIZATION_FAILED, "descriptors").context());
        }
        state = it->second;
    }

    if (request.layouts.empty()) {
        failedAllocations_.fetch_add(1, std::memory_order_relaxed);
        return vkutil::VkExpected<AllocationResult>(vkutil::makeError("DescriptorSetAllocator::allocateResult", VK_ERROR_INITIALIZATION_FAILED, "descriptors").context());
    }

    uint64_t localRetries = 0;
    const auto sizeClass = classifyRequest(request);
    const size_t bucketIndex = classIndex(sizeClass);
    const bool isTransientRequest = state->profile.transientFrame || request.classHint == AllocationRequest::AllocationClassHint::FrameTransient;
    const uint64_t threadKey = currentThreadKey();
    std::unique_lock<std::mutex> classLock(state->classMutexes[bucketIndex]);

    std::shared_ptr<ProfileState::ThreadTransientPools> threadPools;
    if (isTransientRequest) {
        std::lock_guard<std::mutex> profileGuard(state->mutex);
        auto& slot = state->transientPoolsByThread[threadKey];
        if (!slot) {
            slot = std::make_shared<ProfileState::ThreadTransientPools>();
        }
        threadPools = slot;
    }

    auto finalizeSuccess = [&](const AllocationResult& allocation) -> vkutil::VkExpected<AllocationResult> {
        successfulAllocations_.fetch_add(1, std::memory_order_relaxed);
        setsAllocated_.fetch_add(allocation.sets.size(), std::memory_order_relaxed);
        state->stats.retriesBeforeSuccessTotal += localRetries;
        if (localRetries > 0) {
            ++state->stats.successAfterRetryCount;
        }
        return vkutil::VkExpected<AllocationResult>(allocation);
    };

    auto asExpectedError = [](VkResult res) {
        return vkutil::VkExpected<AllocationResult>(vkutil::checkResult(res, "vkAllocateDescriptorSets", "descriptors").context());
    };

    if (threadPools) {
        auto& localDeque = threadPools->pools[bucketIndex];
        for (PoolBucket& bucket : localDeque) {
            auto outcome = allocateFromPool(*state, bucket, request, &classLock);
            if (outcome.status == PoolAllocationStatus::Success) {
                threadPools->lastTouchedEpoch = state->epoch;
                return finalizeSuccess(outcome.allocation);
            }
            allocationRetries_.fetch_add(1, std::memory_order_relaxed);
            ++localRetries;
            if (outcome.status == PoolAllocationStatus::Fatal) {
                failedAllocations_.fetch_add(1, std::memory_order_relaxed);
                return asExpectedError(outcome.result);
            }
        }
    }

    for (PoolBucket& bucket : state->usedPoolsByClass[bucketIndex]) {
        auto outcome = allocateFromPool(*state, bucket, request, &classLock);
        if (outcome.status == PoolAllocationStatus::Success) {
            return finalizeSuccess(outcome.allocation);
        }
        allocationRetries_.fetch_add(1, std::memory_order_relaxed);
        ++localRetries;
        if (outcome.status == PoolAllocationStatus::Fatal) {
            failedAllocations_.fetch_add(1, std::memory_order_relaxed);
            return asExpectedError(outcome.result);
        }
    }

    while (!state->freePoolsByClass[bucketIndex].empty()) {
        PoolBucket bucket = std::move(state->freePoolsByClass[bucketIndex].front());
        state->freePoolsByClass[bucketIndex].pop_front();
        auto outcome = allocateFromPool(*state, bucket, request, &classLock);
        state->usedPoolsByClass[bucketIndex].push_back(std::move(bucket));
        if (outcome.status == PoolAllocationStatus::Success) {
            return finalizeSuccess(outcome.allocation);
        }
        allocationRetries_.fetch_add(1, std::memory_order_relaxed);
        ++localRetries;
        if (outcome.status == PoolAllocationStatus::Fatal) {
            failedAllocations_.fetch_add(1, std::memory_order_relaxed);
            return asExpectedError(outcome.result);
        }
    }

    if (state->outOfPoolStreakByClass[bucketIndex] >= 2) {
        const uint32_t hintNum = growthNumerator(request.classHint);
        const uint32_t hintDen = growthDenominator(request.classHint);
        const uint32_t profileNum = growthNumerator(state->profile.poolClass);
        const uint32_t profileDen = growthDenominator(state->profile.poolClass);
        const uint32_t num = std::max(hintNum, profileNum);
        const uint32_t den = std::max<uint32_t>(1, std::min(hintDen, profileDen));
        const uint32_t grown = boundedScale(state->activeSetsPerPool == 0 ? state->profile.setsPerPool : state->activeSetsPerPool, num, den, maxSetsPerPoolCap(state->profile));
        if (grown != state->activeSetsPerPool) {
            state->activeSetsPerPool = grown;
            ++state->stats.growthEvents;
        }
        state->outOfPoolStreakByClass[bucketIndex] = 0;
    }

    if (state->fragmentedStreakByClass[bucketIndex] >= 2 && !state->usedPoolsByClass[bucketIndex].empty()) {
        state->freePoolsByClass[bucketIndex].push_back(std::move(state->usedPoolsByClass[bucketIndex].front()));
        state->usedPoolsByClass[bucketIndex].pop_front();
        ++state->stats.retiredPools;
        state->fragmentedStreakByClass[bucketIndex] = 0;
    }

    PoolBucket newPool = createPool(*state, sizeClass, request.frameIndex);
    ++state->stats.poolCount;
    auto out = allocateFromPool(*state, newPool, request, &classLock);
    if (out.status != PoolAllocationStatus::Success) {
        allocationRetries_.fetch_add(1, std::memory_order_relaxed);
        ++localRetries;
        failedAllocations_.fetch_add(1, std::memory_order_relaxed);
        return asExpectedError(out.result);
    }

    if (threadPools) {
        newPool.frameIndex = request.frameIndex;
        threadPools->lastTouchedEpoch = state->epoch;
        threadPools->pools[bucketIndex].push_back(std::move(newPool));
    } else {
        state->usedPoolsByClass[bucketIndex].push_back(std::move(newPool));
    }

    return finalizeSuccess(out.allocation);
}

DescriptorSetAllocator::AllocationResult DescriptorSetAllocator::allocate(const AllocationRequest& request)
{
    auto res = allocateResult(request);
    if (!res.hasValue()) {
        vkutil::throwVkError("DescriptorSetAllocator::allocate", res.error());
    }
    return res.value();
}

void DescriptorSetAllocator::free(const AllocationResult& allocation)
{
    std::shared_ptr<ProfileState> state;
    {
        std::shared_lock<std::shared_mutex> mapReadLock(mutex_);
        auto it = profiles_.find(allocation.profileKey);
        if (it == profiles_.end() || allocation.sets.empty()) {
            return;
        }
        state = it->second;
    }

    std::lock_guard<std::mutex> profileLock(state->mutex);
    if (!state->profile.allowFreeIndividualSets) {
        return;
    }

    auto freeFromBuckets = [&](std::array<std::deque<PoolBucket>, 3>& buckets) {
        for (auto& queue : buckets) {
            for (PoolBucket& bucket : queue) {
                if (bucket.pool.get() != allocation.pool) {
                    continue;
                }

                bucket.pool.freeSets(allocation.sets);
                const uint32_t freedCount = static_cast<uint32_t>(allocation.sets.size());
                bucket.liveSets = (bucket.liveSets >= freedCount) ? (bucket.liveSets - freedCount) : 0;
                state->stats.liveSets = (state->stats.liveSets >= freedCount) ? (state->stats.liveSets - freedCount) : 0;
                setsFreed_.fetch_add(freedCount, std::memory_order_relaxed);
                return true;
            }
        }
        return false;
    };

    if (freeFromBuckets(state->usedPoolsByClass)) {
        return;
    }

    {
        for (auto& [_, threadPools] : state->transientPoolsByThread) {
            if (threadPools && freeFromBuckets(threadPools->pools)) {
                return;
            }
        }
    }

    for (auto& [_, bins] : state->transientPoolsByFrame) {
        if (freeFromBuckets(bins)) {
            return;
        }
    }

    static_cast<void>(freeFromBuckets(state->freePoolsByClass));
}

void DescriptorSetAllocator::beginFrame(uint32_t frameIndex, std::optional<uint32_t> completedFrameIndex)
{
    const uint32_t retireUpToFrame = completedFrameIndex.value_or(frameIndex);
    std::shared_lock<std::shared_mutex> mapReadLock(mutex_);
    for (auto& [_, state] : profiles_) {
        std::lock_guard<std::mutex> profileLock(state->mutex);
        if (!state->profile.transientFrame) {
            continue;
        }

        ++state->epoch;
        runCompaction(*state, frameIndex);

        bool reclaimedAny = false;
        {
            for (auto threadIt = state->transientPoolsByThread.begin(); threadIt != state->transientPoolsByThread.end();) {
                if (!threadIt->second) {
                    threadIt = state->transientPoolsByThread.erase(threadIt);
                    continue;
                }

                for (size_t idx = 0; idx < threadIt->second->pools.size(); ++idx) {
                    auto& transientQueue = threadIt->second->pools[idx];
                    while (!transientQueue.empty()) {
                        if (transientQueue.front().frameIndex > retireUpToFrame) {
                            break;
                        }
                        PoolBucket bucket = std::move(transientQueue.front());
                        transientQueue.pop_front();
                        bucket.pool.reset();
                        bucket.liveSets = 0;
                        bucket.retireEpoch = 0;
                        state->freePoolsByClass[idx].push_back(std::move(bucket));
                        reclaimedAny = true;
                    }
                }

                bool emptyThreadPools = true;
                for (const auto& q : threadIt->second->pools) {
                    if (!q.empty()) {
                        emptyThreadPools = false;
                        break;
                    }
                }
                if (emptyThreadPools) {
                    if ((state->epoch - threadIt->second->lastTouchedEpoch) > state->profile.staleThreadEpochsBeforeReclaim) {
                        threadIt = state->transientPoolsByThread.erase(threadIt);
                    } else {
                        ++threadIt;
                    }
                } else {
                    ++threadIt;
                }
            }
        }

        while (state->transientPoolsByThread.size() > state->profile.maxTrackedTransientThreads) {
            auto staleIt = state->transientPoolsByThread.end();
            uint64_t oldestEpoch = UINT64_MAX;
            for (auto it = state->transientPoolsByThread.begin(); it != state->transientPoolsByThread.end(); ++it) {
                if (!it->second) {
                    staleIt = it;
                    break;
                }
                if (it->second->lastTouchedEpoch < oldestEpoch) {
                    oldestEpoch = it->second->lastTouchedEpoch;
                    staleIt = it;
                }
            }
            if (staleIt == state->transientPoolsByThread.end()) {
                break;
            }
            state->transientPoolsByThread.erase(staleIt);
        }

        if (reclaimedAny) {
            state->stats.liveSets = 0;
        }

        uint32_t outstandingBins = 0;
        uint32_t outstandingPools = 0;
        for (const auto& [__, bins] : state->transientPoolsByFrame) {
            for (const auto& bin : bins) {
                if (!bin.empty()) {
                    ++outstandingBins;
                    outstandingPools += static_cast<uint32_t>(bin.size());
                }
            }
        }
        {
            for (const auto& [__, threadPools] : state->transientPoolsByThread) {
                if (!threadPools) {
                    continue;
                }
                for (const auto& bin : threadPools->pools) {
                    if (!bin.empty()) {
                        ++outstandingBins;
                        outstandingPools += static_cast<uint32_t>(bin.size());
                    }
                }
            }
        }
        state->stats.unreclaimedTransientBins = outstandingBins;
        state->stats.unreclaimedTransientPools = outstandingPools;
    }
}

DescriptorSetAllocator::Stats DescriptorSetAllocator::stats(uint64_t profileKey) const
{
    std::shared_lock<std::shared_mutex> mapReadLock(mutex_);
    auto it = profiles_.find(profileKey);
    if (it == profiles_.end()) {
        return Stats{};
    }
    const std::shared_ptr<ProfileState>& state = it->second;
    std::lock_guard<std::mutex> profileLock(state->mutex);
    Stats out = state->stats;
    out.occupancyLowPools = 0;
    out.occupancyMediumPools = 0;
    out.occupancyHighPools = 0;

    auto classifyOccupancy = [&](const PoolBucket& bucket) {
        const uint32_t occ = occupancyPercent(bucket);
        if (occ <= 25) {
            ++out.occupancyLowPools;
        } else if (occ <= 75) {
            ++out.occupancyMediumPools;
        } else {
            ++out.occupancyHighPools;
        }
    };

    for (const auto& bins : state->usedPoolsByClass) {
        for (const PoolBucket& bucket : bins) {
            classifyOccupancy(bucket);
        }
    }
    for (const auto& bins : state->freePoolsByClass) {
        for (const PoolBucket& bucket : bins) {
            classifyOccupancy(bucket);
        }
    }
    for (const auto& [__, bins] : state->transientPoolsByFrame) {
        for (const auto& queue : bins) {
            for (const PoolBucket& bucket : queue) {
                classifyOccupancy(bucket);
            }
        }
    }
    {
        for (const auto& [__, threadPools] : state->transientPoolsByThread) {
            if (!threadPools) {
                continue;
            }
            for (const auto& queue : threadPools->pools) {
                for (const PoolBucket& bucket : queue) {
                    classifyOccupancy(bucket);
                }
            }
        }
    }

    return out;
}

DescriptorSetAllocator::Telemetry DescriptorSetAllocator::telemetry() const
{
    std::shared_lock<std::shared_mutex> mapReadLock(mutex_);
    uint32_t totalPools = 0;
    uint32_t totalUnreclaimedBins = 0;
    uint32_t totalUnreclaimedPools = 0;
    uint64_t totalOutOfPoolFailures = 0;
    uint64_t totalFragmentedFailures = 0;
    uint32_t occupancyLowPools = 0;
    uint32_t occupancyMediumPools = 0;
    uint32_t occupancyHighPools = 0;
    uint64_t retriesBeforeSuccessTotal = 0;
    uint64_t successAfterRetryCount = 0;
    for (const auto& [_, state] : profiles_) {
        std::lock_guard<std::mutex> profileLock(state->mutex);
        totalOutOfPoolFailures += state->stats.outOfPoolEvents;
        totalFragmentedFailures += state->stats.fragmentedEvents;
        retriesBeforeSuccessTotal += state->stats.retriesBeforeSuccessTotal;
        successAfterRetryCount += state->stats.successAfterRetryCount;

        auto classifyOccupancy = [&](const PoolBucket& bucket) {
            const uint32_t occ = occupancyPercent(bucket);
            if (occ <= 25) {
                ++occupancyLowPools;
            } else if (occ <= 75) {
                ++occupancyMediumPools;
            } else {
                ++occupancyHighPools;
            }
        };

        for (size_t idx = 0; idx < state->freePoolsByClass.size(); ++idx) {
            totalPools += static_cast<uint32_t>(state->freePoolsByClass[idx].size() + state->usedPoolsByClass[idx].size());
            for (const PoolBucket& bucket : state->freePoolsByClass[idx]) {
                classifyOccupancy(bucket);
            }
            for (const PoolBucket& bucket : state->usedPoolsByClass[idx]) {
                classifyOccupancy(bucket);
            }
        }
        for (const auto& [__, bins] : state->transientPoolsByFrame) {
            for (const auto& bin : bins) {
                totalPools += static_cast<uint32_t>(bin.size());
                for (const PoolBucket& bucket : bin) {
                    classifyOccupancy(bucket);
                }
                if (!bin.empty()) {
                    ++totalUnreclaimedBins;
                    totalUnreclaimedPools += static_cast<uint32_t>(bin.size());
                }
            }
        }
        {
            for (const auto& [__, threadPools] : state->transientPoolsByThread) {
                if (!threadPools) {
                    continue;
                }
                for (const auto& bin : threadPools->pools) {
                    totalPools += static_cast<uint32_t>(bin.size());
                    for (const PoolBucket& bucket : bin) {
                        classifyOccupancy(bucket);
                    }
                    if (!bin.empty()) {
                        ++totalUnreclaimedBins;
                        totalUnreclaimedPools += static_cast<uint32_t>(bin.size());
                    }
                }
            }
        }
    }

    return Telemetry{
        .allocationAttempts = allocationAttempts_.load(std::memory_order_relaxed),
        .allocationRetries = allocationRetries_.load(std::memory_order_relaxed),
        .successfulAllocations = successfulAllocations_.load(std::memory_order_relaxed),
        .failedAllocations = failedAllocations_.load(std::memory_order_relaxed),
        .setsAllocated = setsAllocated_.load(std::memory_order_relaxed),
        .setsFreed = setsFreed_.load(std::memory_order_relaxed),
        .profiles = static_cast<uint32_t>(profiles_.size()),
        .pools = totalPools,
        .unreclaimedTransientBins = totalUnreclaimedBins,
        .unreclaimedTransientPools = totalUnreclaimedPools,
        .outOfPoolFailures = totalOutOfPoolFailures,
        .fragmentedFailures = totalFragmentedFailures,
        .occupancyLowPools = occupancyLowPools,
        .occupancyMediumPools = occupancyMediumPools,
        .occupancyHighPools = occupancyHighPools,
        .retriesBeforeSuccessTotal = retriesBeforeSuccessTotal,
        .successAfterRetryCount = successAfterRetryCount
    };
}
