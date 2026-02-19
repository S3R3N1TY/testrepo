#pragma once

#include <array>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>

// parasoft-begin-suppress ALL "suppress all violations"
#include <vulkan/vulkan.h>
// parasoft-end-suppress ALL "suppress all violations"

 #include "VkUtils.h"
 #include "VkCommands.h"

class CommandContextAllocator {
public:
    struct QueueConfig {
        uint32_t queueFamilyIndex = 0;
        bool transient = false;
        uint32_t workerThreads = 1;
        uint32_t preallocatePerFrame = 8;
    };

    CommandContextAllocator() = default;
    CommandContextAllocator(VkDevice device, uint32_t framesInFlight, std::vector<QueueConfig> queues);
    [[nodiscard]] static vkutil::VkExpected<CommandContextAllocator> createResult(VkDevice device, uint32_t framesInFlight, std::vector<QueueConfig> queues);

    void setRuntimeGeneration(uint64_t generation) noexcept { runtimeGeneration_ = generation; }
    [[nodiscard]] uint64_t runtimeGeneration() const noexcept { return runtimeGeneration_; }

    [[nodiscard]] vkutil::VkExpected<void> beginFrame(uint32_t frameIndex, uint64_t expectedGeneration = 0);

    [[nodiscard]] vkutil::VkExpected<VkCommandBuffer> allocatePrimary(
        uint32_t queueSlot,
        const char* debugName = nullptr,
        VkCommandBufferUsageFlags usage = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        uint64_t expectedGeneration = 0,
        uint32_t workerIndex = 0);

    [[nodiscard]] vkutil::VkExpected<void> end(VkCommandBuffer commandBuffer, uint64_t expectedGeneration = 0);

    ~CommandContextAllocator();

private:
    struct QueueArena {
        uint32_t family = 0;
        uint32_t workerThreads = 1;
        VulkanCommandArena arena{};
        VulkanCommandArena::FrameToken frameToken{};
    };

    struct BorrowedRecord {
        uint32_t queueSlot = 0;
        VulkanCommandArena::BorrowedCommandBuffer borrowed{};
    };

    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t framesInFlight_ = 0;
    uint32_t currentFrame_ = 0;
    uint64_t runtimeGeneration_ = 0;
    std::vector<QueueArena> queueArenas_;
    std::unordered_map<VkCommandBuffer, BorrowedRecord> borrowedByHandle_;
    std::mutex borrowedMutex_{};

    [[nodiscard]] vkutil::VkExpected<void> checkGeneration(uint64_t expectedGeneration, const char* operation) const;
    [[nodiscard]] vkutil::VkExpected<void> init(VkDevice device, uint32_t framesInFlight, std::vector<QueueConfig> queues);
};

class DescriptorAllocator {
public:
    struct PoolRatio {
        VkDescriptorType type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        float ratio = 0.0f;
    };

    enum class PoolClass : uint8_t {
        Static = 0,
        Dynamic,
        Bindless,
        Count
    };

    struct PoolClassConfig {
        uint32_t maxSetsPerPool = 0;
        std::vector<PoolRatio> ratios;
        VkDescriptorPoolCreateFlags flags = 0;
    };

    DescriptorAllocator() = default;
    DescriptorAllocator(VkDevice device, uint32_t maxSetsPerPool, std::vector<PoolRatio> ratios);
    DescriptorAllocator(VkDevice device, std::array<PoolClassConfig, static_cast<size_t>(PoolClass::Count)> classConfigs);
    [[nodiscard]] static vkutil::VkExpected<DescriptorAllocator> createResult(VkDevice device, uint32_t maxSetsPerPool, std::vector<PoolRatio> ratios);
    [[nodiscard]] static vkutil::VkExpected<DescriptorAllocator> createResult(VkDevice device, std::array<PoolClassConfig, static_cast<size_t>(PoolClass::Count)> classConfigs);

    [[nodiscard]] vkutil::VkExpected<void> beginFrame(uint32_t frameIndex);

    [[nodiscard]] vkutil::VkExpected<VkDescriptorSet> allocate(
        VkDescriptorSetLayout layout,
        const char* debugName = nullptr,
        uint64_t frameIndex = 0,
        PoolClass poolClass = PoolClass::Dynamic);

    [[nodiscard]] vkutil::VkExpected<void> allocateMany(
        const std::vector<VkDescriptorSetLayout>& layouts,
        std::vector<VkDescriptorSet>& outSets,
        uint64_t frameIndex = 0,
        PoolClass poolClass = PoolClass::Dynamic);

    [[nodiscard]] vkutil::VkExpected<void> resetClass(PoolClass poolClass, uint64_t frameIndex = 0);

    ~DescriptorAllocator();

private:
    struct PoolBank {
        PoolClassConfig config{};
        std::vector<VkDescriptorPool> readyPools;
        std::vector<VkDescriptorPool> usedPools;
        std::vector<VkDescriptorPool> pendingRecyclePools;
    };

    [[nodiscard]] vkutil::VkExpected<VkDescriptorPool> createPool(PoolClass poolClass, uint64_t frameIndex);
    [[nodiscard]] vkutil::VkExpected<void> resetBank(PoolBank& bank, uint64_t frameIndex);
    [[nodiscard]] vkutil::VkExpected<void> init(VkDevice device, std::array<PoolClassConfig, static_cast<size_t>(PoolClass::Count)> classConfigs);

    VkDevice device_ = VK_NULL_HANDLE;
    std::array<PoolBank, static_cast<size_t>(PoolClass::Count)> banks_{};
};
