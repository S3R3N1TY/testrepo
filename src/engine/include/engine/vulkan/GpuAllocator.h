#pragma once

#include <atomic>
#include <array>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

// parasoft-begin-suppress ALL "suppress all violations"
#include <vulkan/vulkan.h>
// parasoft-end-suppress ALL "suppress all violations"

class GpuAllocator {
public:
    enum class ResourceClass : uint8_t {
        Buffer = 0,
        Image = 1
    };

    enum class LifetimeClass : uint8_t {
        Persistent = 0,
        Transient = 1
    };

    struct Allocation {
        VkDeviceMemory memory{ VK_NULL_HANDLE };
        VkDeviceSize offset{ 0 };
        VkDeviceSize size{ 0 };
        uint32_t memoryTypeIndex{ UINT32_MAX };
        uint64_t poolKey{ 0 };
        VkMemoryAllocateFlags allocateFlags{ 0 };
        bool dedicated{ false };
        ResourceClass resourceClass{ ResourceClass::Buffer };
        LifetimeClass lifetimeClass{ LifetimeClass::Persistent };
    };

    struct Telemetry {
        uint64_t allocationCount{ 0 };
        uint64_t freeCount{ 0 };
        uint64_t bytesAllocated{ 0 };
        uint64_t bytesFreed{ 0 };
        uint64_t bytesInUse{ 0 };
        uint64_t dedicatedAllocationCount{ 0 };
        uint64_t pooledAllocationCount{ 0 };
        uint32_t poolCount{ 0 };
        uint64_t freeBytes{ 0 };
        uint64_t totalBytes{ 0 };
        double fragmentationRatio{ 0.0 };
        std::array<uint64_t, 2> bytesAllocatedByResourceClass{};
        std::array<uint64_t, 2> bytesFreedByResourceClass{};
        std::array<uint64_t, 2> allocationCountByResourceClass{};
        std::array<uint64_t, 2> bytesAllocatedByLifetimeClass{};
        std::array<uint64_t, 2> bytesFreedByLifetimeClass{};
    };

    GpuAllocator() noexcept = default;
    GpuAllocator(VkDevice device, VkPhysicalDevice physicalDevice,
        bool bufferDeviceAddressEnabled = false,
        VkDeviceSize defaultPoolBlockSize = 64ull * 1024ull * 1024ull,
        VkDeviceSize dedicatedThreshold = 16ull * 1024ull * 1024ull);

    GpuAllocator(const GpuAllocator&) = delete;
    GpuAllocator& operator=(const GpuAllocator&) = delete;

    GpuAllocator(GpuAllocator&&) = delete;
    GpuAllocator& operator=(GpuAllocator&&) = delete;

    ~GpuAllocator() noexcept;

    [[nodiscard]] bool valid() const noexcept { return device_ != VK_NULL_HANDLE; }
    [[nodiscard]] VkDevice device() const noexcept { return device_; }
    [[nodiscard]] VkPhysicalDevice physicalDevice() const noexcept { return physicalDevice_; }
    [[nodiscard]] VkDeviceSize nonCoherentAtomSize() const noexcept { return nonCoherentAtomSize_; }
    [[nodiscard]] bool bufferDeviceAddressEnabled() const noexcept { return bufferDeviceAddressEnabled_; }

    [[nodiscard]] Allocation allocateForBuffer(const VkMemoryRequirements& req,
        VkMemoryPropertyFlags properties,
        VkMemoryAllocateFlags allocateFlags = 0,
        VkBuffer dedicatedBuffer = VK_NULL_HANDLE,
        bool forceDedicated = false,
        LifetimeClass lifetimeClass = LifetimeClass::Persistent);
    [[nodiscard]] Allocation allocateForImage(const VkMemoryRequirements& req,
        VkMemoryPropertyFlags properties,
        VkImage dedicatedImage = VK_NULL_HANDLE,
        bool forceDedicated = false,
        LifetimeClass lifetimeClass = LifetimeClass::Persistent);

    [[nodiscard]] bool shouldUseDedicatedAllocation(const VkMemoryRequirements& req,
        const VkMemoryDedicatedRequirements& dedicatedReq,
        ResourceClass resourceClass,
        LifetimeClass lifetimeClass,
        VkMemoryPropertyFlags properties,
        bool forceDedicated = false) const noexcept;

    void free(const Allocation& allocation) noexcept;

    [[nodiscard]] uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const;

    [[nodiscard]] Telemetry telemetry() const;

    void reset() noexcept;

private:
    struct FreeRange {
        VkDeviceSize offset{ 0 };
        VkDeviceSize size{ 0 };
    };

    struct MemoryBlock {
        VkDeviceMemory memory{ VK_NULL_HANDLE };
        VkDeviceSize size{ 0 };
        uint32_t memoryTypeIndex{ UINT32_MAX };
        uint64_t poolKey{ 0 };
        VkMemoryAllocateFlags allocateFlags{ 0 };
        std::vector<FreeRange> freeRanges{};
    };

    VkDevice device_{ VK_NULL_HANDLE };
    VkPhysicalDevice physicalDevice_{ VK_NULL_HANDLE };
    VkPhysicalDeviceMemoryProperties memProps_{};
    bool bufferDeviceAddressEnabled_{ false };
    VkDeviceSize nonCoherentAtomSize_{ 1 };
    VkDeviceSize defaultPoolBlockSize_{ 0 };
    VkDeviceSize dedicatedThreshold_{ 0 };

    mutable std::mutex mutex_{};
    std::unordered_map<uint64_t, std::vector<MemoryBlock>> pooledBlocks_{};
    std::atomic<uint64_t> allocationCount_{ 0 };
    std::atomic<uint64_t> freeCount_{ 0 };
    std::atomic<uint64_t> bytesAllocated_{ 0 };
    std::atomic<uint64_t> bytesFreed_{ 0 };
    std::atomic<uint64_t> dedicatedAllocationCount_{ 0 };
    std::atomic<uint64_t> pooledAllocationCount_{ 0 };
    std::array<std::atomic<uint64_t>, 2> bytesAllocatedByResourceClass_{};
    std::array<std::atomic<uint64_t>, 2> bytesFreedByResourceClass_{};
    std::array<std::atomic<uint64_t>, 2> allocationCountByResourceClass_{};
    std::array<std::atomic<uint64_t>, 2> bytesAllocatedByLifetimeClass_{};
    std::array<std::atomic<uint64_t>, 2> bytesFreedByLifetimeClass_{};

    static VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment) noexcept;
    [[nodiscard]] static uint64_t makePoolKey(uint32_t memoryTypeIndex, VkMemoryAllocateFlags allocateFlags) noexcept;
    MemoryBlock& createPooledBlock(uint32_t memoryTypeIndex, VkMemoryAllocateFlags allocateFlags, VkDeviceSize minSize);
    static void mergeFreeRanges(std::vector<FreeRange>& ranges);
    [[nodiscard]] Allocation allocateInternal(const VkMemoryRequirements& req,
        VkMemoryPropertyFlags properties,
        VkMemoryAllocateFlags allocateFlags,
        bool forceDedicated,
        VkBuffer dedicatedBuffer,
        VkImage dedicatedImage,
        ResourceClass resourceClass,
        LifetimeClass lifetimeClass);
};
