// VkDescriptors.h
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <vector>
#include <unordered_map>
#include <deque>
#include <string>

// parasoft-begin-suppress ALL "suppress all violations"
#include <vulkan/vulkan.h>
// parasoft-end-suppress ALL "suppress all violations"

 #include "UniqueHandle.h"
 #include "VkUtils.h"
// ============================================================
// VulkanDescriptorSetLayout (RAII)
// Owns VkDescriptorSetLayout (device-owned).
// ============================================================

class VulkanDescriptorSetLayout {
public:
    VulkanDescriptorSetLayout() noexcept = default;

    VulkanDescriptorSetLayout(
        VkDevice device,
        const std::vector<VkDescriptorSetLayoutBinding>& bindings,
        VkDescriptorSetLayoutCreateFlags flags = 0,
        const void* pNext = nullptr);

    VulkanDescriptorSetLayout(const VulkanDescriptorSetLayout&) = delete;
    VulkanDescriptorSetLayout& operator=(const VulkanDescriptorSetLayout&) = delete;

    VulkanDescriptorSetLayout(VulkanDescriptorSetLayout&&) noexcept = default;
    VulkanDescriptorSetLayout& operator=(VulkanDescriptorSetLayout&&) noexcept = default;

    ~VulkanDescriptorSetLayout() = default;

    [[nodiscard]] VkDescriptorSetLayout get() const noexcept { return handle.get(); }
    [[nodiscard]] VkDevice              getDevice() const noexcept { return handle.getDevice(); }
    [[nodiscard]] bool                  valid() const noexcept { return static_cast<bool>(handle); }

private:
    vkhandle::DeviceUniqueHandle<VkDescriptorSetLayout, PFN_vkDestroyDescriptorSetLayout> handle;
};

class VulkanDescriptorPool {
public:
    VulkanDescriptorPool() noexcept = default;

    VulkanDescriptorPool(
        VkDevice device,
        const std::vector<VkDescriptorPoolSize>& poolSizes,
        uint32_t maxSets,
        VkDescriptorPoolCreateFlags flags = 0,
        const void* pNext = nullptr);

    VulkanDescriptorPool(const VulkanDescriptorPool&) = delete;
    VulkanDescriptorPool& operator=(const VulkanDescriptorPool&) = delete;

    VulkanDescriptorPool(VulkanDescriptorPool&&) noexcept = default;
    VulkanDescriptorPool& operator=(VulkanDescriptorPool&&) noexcept = default;

    ~VulkanDescriptorPool() = default;

    [[nodiscard]] VkDescriptorPool get() const noexcept { return handle.get(); }
    [[nodiscard]] VkDevice         getDevice() const noexcept { return handle.getDevice(); }
    [[nodiscard]] bool             valid() const noexcept { return static_cast<bool>(handle); }

    void allocateSets(
        const std::vector<VkDescriptorSetLayout>& layouts,
        std::vector<VkDescriptorSet>& outSets) const;

    template <size_t N>
    void allocateSets(
        const std::array<VkDescriptorSetLayout, N>& layouts,
        std::array<VkDescriptorSet, N>& outSets) const
    {
        outSets.fill(VK_NULL_HANDLE);
        allocateSetsImpl(static_cast<uint32_t>(N), layouts.data(), outSets.data());
    }

    void freeSets(const std::vector<VkDescriptorSet>& sets) const;
    void reset() const;

private:
    vkhandle::DeviceUniqueHandle<VkDescriptorPool, PFN_vkDestroyDescriptorPool> handle;

    void allocateSetsImpl(
        uint32_t count,
        const VkDescriptorSetLayout* layouts,
        VkDescriptorSet* outSets) const;
};

class DescriptorSetAllocator {
public:
    struct Profile {
        enum class PoolClass : uint8_t { FrameTransient, Material, Bindless, Custom };

        uint32_t profileId{ 0 };
        std::vector<VkDescriptorPoolSize> poolSizes{};
        uint32_t setsPerPool{ 64 };
        VkDescriptorPoolCreateFlags poolFlags{ 0 };
        bool transientFrame{ true };
        bool allowFreeIndividualSets{ false };
        PoolClass poolClass{ PoolClass::Custom };
        uint32_t compactionIntervalFrames{ 2 };
        uint32_t maxPoolAgeBeforeRecycle{ 8 };
        uint32_t staleThreadEpochsBeforeReclaim{ 6 };
        uint32_t maxTrackedTransientThreads{ 128 };
        uint32_t lowOccupancyRecycleThresholdPercent{ 25 };
        uint32_t targetOccupancyPercent{ 70 };

        [[nodiscard]] uint64_t key() const noexcept;
        [[nodiscard]] uint64_t configHash() const noexcept;
        [[nodiscard]] bool equivalentConfig(const Profile& other) const noexcept;
    };

    struct AllocationRequest {
        enum class AllocationClassHint : uint8_t { FrameTransient, Material, Bindless, Generic };

        uint64_t profileKey{ 0 };
        std::vector<VkDescriptorSetLayout> layouts{};
        std::vector<uint32_t> variableDescriptorCounts{};
        uint32_t frameIndex{ 0 };
        AllocationClassHint classHint{ AllocationClassHint::Generic };
    };

    struct AllocationResult {
        std::vector<VkDescriptorSet> sets{};
        VkDescriptorPool pool{ VK_NULL_HANDLE };
        uint64_t profileKey{ 0 };
    };

    struct Stats {
        uint32_t poolCount{ 0 };
        uint32_t liveSets{ 0 };
        uint32_t failedAllocations{ 0 };
        uint32_t fragmentedEvents{ 0 };
        uint32_t outOfPoolEvents{ 0 };
        uint32_t peakLiveSets{ 0 };
        uint32_t growthEvents{ 0 };
        uint32_t retiredPools{ 0 };
        uint64_t allocationAttempts{ 0 };
        uint64_t allocationRetries{ 0 };
        uint64_t successfulAllocations{ 0 };
        uint32_t unreclaimedTransientBins{ 0 };
        uint32_t unreclaimedTransientPools{ 0 };
        uint32_t compactionRuns{ 0 };
        uint32_t recycledLowOccupancyPools{ 0 };
        uint32_t occupancyLowPools{ 0 };
        uint32_t occupancyMediumPools{ 0 };
        uint32_t occupancyHighPools{ 0 };
        uint64_t retriesBeforeSuccessTotal{ 0 };
        uint64_t successAfterRetryCount{ 0 };
    };

    struct Telemetry {
        uint64_t allocationAttempts{ 0 };
        uint64_t allocationRetries{ 0 };
        uint64_t successfulAllocations{ 0 };
        uint64_t failedAllocations{ 0 };
        uint64_t setsAllocated{ 0 };
        uint64_t setsFreed{ 0 };
        uint32_t profiles{ 0 };
        uint32_t pools{ 0 };
        uint32_t unreclaimedTransientBins{ 0 };
        uint32_t unreclaimedTransientPools{ 0 };
        uint64_t outOfPoolFailures{ 0 };
        uint64_t fragmentedFailures{ 0 };
        uint32_t occupancyLowPools{ 0 };
        uint32_t occupancyMediumPools{ 0 };
        uint32_t occupancyHighPools{ 0 };
        uint64_t retriesBeforeSuccessTotal{ 0 };
        uint64_t successAfterRetryCount{ 0 };
    };

    DescriptorSetAllocator() noexcept = default;
    explicit DescriptorSetAllocator(VkDevice device, VkPhysicalDevice physicalDevice = VK_NULL_HANDLE);

    DescriptorSetAllocator(const DescriptorSetAllocator&) = delete;
    DescriptorSetAllocator& operator=(const DescriptorSetAllocator&) = delete;
    DescriptorSetAllocator(DescriptorSetAllocator&&) noexcept = delete;
    DescriptorSetAllocator& operator=(DescriptorSetAllocator&&) noexcept = delete;

    ~DescriptorSetAllocator() noexcept;

    [[nodiscard]] bool valid() const noexcept { return device_ != VK_NULL_HANDLE; }

    uint64_t registerProfile(const Profile& profile);
    [[nodiscard]] vkutil::VkExpected<AllocationResult> allocateResult(const AllocationRequest& request);
    [[nodiscard]] AllocationResult allocate(const AllocationRequest& request);

    static void setCurrentThreadSlot(uint32_t slot) noexcept;
    static void clearCurrentThreadSlot() noexcept;

    void free(const AllocationResult& allocation);
    void beginFrame(uint32_t frameIndex, std::optional<uint32_t> completedFrameIndex = std::nullopt);
    [[nodiscard]] Stats stats(uint64_t profileKey) const;
    [[nodiscard]] Telemetry telemetry() const;

private:
    struct PoolBucket {
        enum class SizeClass : uint8_t { Small, Medium, Large };

        VulkanDescriptorPool pool{};
        uint32_t liveSets{ 0 };
        uint32_t frameIndex{ 0 };
        uint64_t retireEpoch{ 0 };
        SizeClass sizeClass{ SizeClass::Medium };
        uint32_t maxSets{ 0 };
        uint64_t lastUsedEpoch{ 0 };
    };

    struct ProfileState {
        Profile profile{};
        uint64_t configHash{ 0 };
        Stats stats{};
        std::array<std::deque<PoolBucket>, 3> freePoolsByClass{};
        std::array<std::deque<PoolBucket>, 3> usedPoolsByClass{};
        std::unordered_map<uint32_t, std::array<std::deque<PoolBucket>, 3>> transientPoolsByFrame{};
        uint32_t activeSetsPerPool{ 0 };
        mutable std::mutex mutex{};
        std::array<std::mutex, 3> classMutexes{};
        std::array<uint32_t, 3> outOfPoolStreakByClass{};
        std::array<uint32_t, 3> fragmentedStreakByClass{};
        uint64_t epoch{ 0 };

        struct ThreadTransientPools {
            std::array<std::deque<PoolBucket>, 3> pools{};
            uint64_t lastTouchedEpoch{ 0 };
        };
        std::unordered_map<uint64_t, std::shared_ptr<ThreadTransientPools>> transientPoolsByThread{};
    };

    enum class PoolAllocationStatus : uint8_t { Success, OutOfPoolMemory, FragmentedPool, Fatal };

    struct PoolAllocationOutcome {
        PoolAllocationStatus status{ PoolAllocationStatus::Fatal };
        VkResult result{ VK_ERROR_UNKNOWN };
        AllocationResult allocation{};
    };

    [[nodiscard]] PoolBucket createPool(ProfileState& state, PoolBucket::SizeClass sizeClass, uint32_t frameIndex);
    [[nodiscard]] PoolAllocationOutcome allocateFromPool(ProfileState& state, PoolBucket& bucket, const AllocationRequest& request, std::unique_lock<std::mutex>* stateLock = nullptr);
    [[nodiscard]] static PoolBucket::SizeClass classifyRequest(const AllocationRequest& request) noexcept;
    [[nodiscard]] static size_t classIndex(PoolBucket::SizeClass sizeClass) noexcept;
    [[nodiscard]] static uint32_t growthNumerator(AllocationRequest::AllocationClassHint hint) noexcept;
    [[nodiscard]] static uint32_t growthDenominator(AllocationRequest::AllocationClassHint hint) noexcept;
    [[nodiscard]] static uint32_t growthNumerator(Profile::PoolClass poolClass) noexcept;
    [[nodiscard]] static uint32_t growthDenominator(Profile::PoolClass poolClass) noexcept;
    [[nodiscard]] static uint32_t occupancyPercent(const PoolBucket& bucket) noexcept;
    void rebalancePoolsForCompaction(ProfileState& state, std::array<std::deque<PoolBucket>, 3>& buckets);
    void runCompaction(ProfileState& state, uint32_t frameIndex);
    [[nodiscard]] uint32_t maxSetsPerPoolCap(const Profile& profile) const noexcept;
    [[nodiscard]] uint32_t descriptorLimitForType(VkDescriptorType type, bool updateAfterBind) const noexcept;
    [[nodiscard]] uint32_t clampedSetsPerPool(const ProfileState& state, uint32_t requestedSets) const noexcept;

    VkDevice device_{ VK_NULL_HANDLE };
    mutable std::shared_mutex mutex_{};
    std::unordered_map<uint64_t, std::shared_ptr<ProfileState>> profiles_{};
    std::atomic<uint64_t> allocationAttempts_{ 0 };
    std::atomic<uint64_t> allocationRetries_{ 0 };
    std::atomic<uint64_t> successfulAllocations_{ 0 };
    std::atomic<uint64_t> failedAllocations_{ 0 };
    std::atomic<uint64_t> setsAllocated_{ 0 };
    std::atomic<uint64_t> setsFreed_{ 0 };
    VkPhysicalDeviceLimits limits_{};
    VkPhysicalDeviceDescriptorIndexingProperties descriptorIndexingProperties_{};
    bool hasDeviceLimits_{ false };
};
