#pragma once

#include <cstdint>
#include <stdexcept>
#include <vector>
#include <memory>

// parasoft-begin-suppress ALL "suppress all violations"
#include <vulkan/vulkan.h>
// parasoft-end-suppress ALL "suppress all violations"

#include "GpuAllocator.h"
#include "VkUtils.h"

class VulkanBuffer {
public:
    enum class AllocationPolicy : uint8_t {
        Auto,
        Upload,
        Readback,
        DeviceLocal,
        Transient
    };

    VulkanBuffer() noexcept = default;

    VulkanBuffer(VkDevice device,
        VkPhysicalDevice physicalDevice,
        VkDeviceSize size,
        VkBufferUsageFlags usage,
        VkMemoryPropertyFlags memoryProperties,
        bool bufferDeviceAddressEnabled = false,
        bool requiresDeviceAddress = false,
        AllocationPolicy allocationPolicy = AllocationPolicy::Auto,
        const std::vector<uint32_t>& queueFamilyIndices = {});

    VulkanBuffer(GpuAllocator& allocator,
        VkDeviceSize size,
        VkBufferUsageFlags usage,
        VkMemoryPropertyFlags memoryProperties,
        bool requiresDeviceAddress = false,
        AllocationPolicy allocationPolicy = AllocationPolicy::Auto,
        const std::vector<uint32_t>& queueFamilyIndices = {});

    VulkanBuffer(const VulkanBuffer&) = delete;
    VulkanBuffer& operator=(const VulkanBuffer&) = delete;

    VulkanBuffer(VulkanBuffer&& other) noexcept;
    VulkanBuffer& operator=(VulkanBuffer&& other) noexcept;

    ~VulkanBuffer() noexcept;

    [[nodiscard]] VkBuffer        get()       const noexcept { return buffer; }
    [[nodiscard]] VkDeviceMemory  getMemory() const noexcept { return allocation.memory; }
    [[nodiscard]] VkDeviceSize    getOffset() const noexcept { return allocation.offset; }
    [[nodiscard]] VkDeviceSize    getSize()   const noexcept { return size; }
    [[nodiscard]] bool            valid()     const noexcept { return buffer != VK_NULL_HANDLE; }

    [[nodiscard]] VkDevice        getDevice() const noexcept { return device; }
    [[nodiscard]] VkPhysicalDevice getPhysicalDevice() const noexcept { return physicalDevice; }
    [[nodiscard]] VkMemoryPropertyFlags memoryProperties() const noexcept { return memoryProps; }

    [[nodiscard]] vkutil::VkExpected<void*> mapResult(VkDeviceSize offset = 0, VkDeviceSize mapSize = VK_WHOLE_SIZE);
    [[nodiscard]] void* map(VkDeviceSize offset = 0, VkDeviceSize mapSize = VK_WHOLE_SIZE);
    void  unmap() noexcept;
    [[nodiscard]] void* mapped() const noexcept { return mappedPtr; }

    [[nodiscard]] vkutil::VkExpected<void> flushResult(VkDeviceSize offset = 0, VkDeviceSize flushSize = VK_WHOLE_SIZE) const;
    [[nodiscard]] vkutil::VkExpected<void> invalidateResult(VkDeviceSize offset = 0, VkDeviceSize invalidateSize = VK_WHOLE_SIZE) const;
    void flush(VkDeviceSize offset = 0, VkDeviceSize flushSize = VK_WHOLE_SIZE) const;
    void invalidate(VkDeviceSize offset = 0, VkDeviceSize invalidateSize = VK_WHOLE_SIZE) const;

    [[nodiscard]] vkutil::VkExpected<VkDeviceAddress> deviceAddress() const;
    [[nodiscard]] bool requiresDeviceAddress() const noexcept { return requiresDeviceAddress_; }
    [[nodiscard]] bool bufferDeviceAddressEnabled() const noexcept { return bufferDeviceAddressEnabled_; }
    [[nodiscard]] AllocationPolicy allocationPolicy() const noexcept { return allocationPolicy_; }

    void reset() noexcept;

private:
    VkDevice              device{ VK_NULL_HANDLE };
    VkPhysicalDevice      physicalDevice{ VK_NULL_HANDLE };
    VkBuffer              buffer{ VK_NULL_HANDLE };
    VkDeviceSize          size{ 0 };
    VkMemoryPropertyFlags memoryProps{ 0 };

    std::unique_ptr<GpuAllocator> ownedAllocator{};
    GpuAllocator* allocator{ nullptr };
    GpuAllocator::Allocation allocation{};

    void* mappedPtr{ nullptr };
    VkDeviceSize mappedOffset{ 0 };
    VkDeviceSize mappedSize{ 0 };

    VkDeviceSize nonCoherentAtomSize{ 1 };
    bool requiresDeviceAddress_{ false };
    bool bufferDeviceAddressEnabled_{ false };
    AllocationPolicy allocationPolicy_{ AllocationPolicy::Auto };

    [[nodiscard]] static bool usageSupportsDeviceAddress(VkBufferUsageFlags usage) noexcept;
    void validateAllocationPolicy(VkMemoryPropertyFlags memoryProperties) const;
    void validateDeviceAddressRequirements(VkBufferUsageFlags usage) const;

    [[nodiscard]] vkutil::VkExpected<VkMappedMemoryRange> prepareMappedRange(VkDeviceSize offset, VkDeviceSize size, const char* opName) const;
    void createBuffer(VkDeviceSize size,
        VkBufferUsageFlags usage,
        VkMemoryPropertyFlags memoryProperties,
        const std::vector<uint32_t>& queueFamilyIndices);
    [[nodiscard]] bool isHostVisible() const noexcept { return (memoryProps & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0; }
    [[nodiscard]] bool isHostCoherent() const noexcept { return (memoryProps & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0; }
};
