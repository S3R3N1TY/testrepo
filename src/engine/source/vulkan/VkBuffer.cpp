#include <algorithm>
#include <utility>

#include "VkBuffer.h"
#include "VkUtils.h"

namespace {
VkDeviceSize getNonCoherentAtomSize(VkPhysicalDevice physicalDevice)
{
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physicalDevice, &props);
    return std::max<VkDeviceSize>(1, props.limits.nonCoherentAtomSize);
}
}

VulkanBuffer::VulkanBuffer(VkDevice device_,
    VkPhysicalDevice physicalDevice_,
    VkDeviceSize size_,
    VkBufferUsageFlags usage,
    VkMemoryPropertyFlags memoryProperties,
    bool bufferDeviceAddressEnabled,
    bool requiresDeviceAddress,
    AllocationPolicy allocationPolicy,
    const std::vector<uint32_t>& queueFamilyIndices)
    : device(device_)
    , physicalDevice(physicalDevice_)
    , size(size_)
    , memoryProps(memoryProperties)
    , ownedAllocator(std::make_unique<GpuAllocator>(device_, physicalDevice_, bufferDeviceAddressEnabled))
    , allocator(ownedAllocator.get())
    , requiresDeviceAddress_(requiresDeviceAddress)
    , bufferDeviceAddressEnabled_(bufferDeviceAddressEnabled)
    , allocationPolicy_(allocationPolicy)
{
    if (device == VK_NULL_HANDLE || physicalDevice == VK_NULL_HANDLE) {
        throw std::runtime_error("VulkanBuffer: device/physicalDevice is null");
    }
    nonCoherentAtomSize = getNonCoherentAtomSize(physicalDevice);
    validateAllocationPolicy(memoryProperties);
    validateDeviceAddressRequirements(usage);
    createBuffer(size, usage, memoryProperties, queueFamilyIndices);
}

VulkanBuffer::VulkanBuffer(GpuAllocator& allocator_,
    VkDeviceSize size_,
    VkBufferUsageFlags usage,
    VkMemoryPropertyFlags memoryProperties,
    bool requiresDeviceAddress,
    AllocationPolicy allocationPolicy,
    const std::vector<uint32_t>& queueFamilyIndices)
    : device(allocator_.device())
    , physicalDevice(allocator_.physicalDevice())
    , size(size_)
    , memoryProps(memoryProperties)
    , allocator(&allocator_)
    , requiresDeviceAddress_(requiresDeviceAddress)
    , bufferDeviceAddressEnabled_(allocator_.bufferDeviceAddressEnabled())
    , allocationPolicy_(allocationPolicy)
{
    if (!allocator->valid()) {
        throw std::runtime_error("VulkanBuffer: allocator is invalid");
    }
    nonCoherentAtomSize = allocator->nonCoherentAtomSize();
    validateAllocationPolicy(memoryProperties);
    validateDeviceAddressRequirements(usage);
    createBuffer(size, usage, memoryProperties, queueFamilyIndices);
}

void VulkanBuffer::createBuffer(VkDeviceSize size_,
    VkBufferUsageFlags usage,
    VkMemoryPropertyFlags memoryProperties,
    const std::vector<uint32_t>& queueFamilyIndices)
{
    if (size_ == 0) {
        throw std::runtime_error("VulkanBuffer: size must be > 0");
    }

    VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = size_;
    bi.usage = usage;
    if (queueFamilyIndices.size() > 1) {
        bi.sharingMode = VK_SHARING_MODE_CONCURRENT;
        bi.queueFamilyIndexCount = static_cast<uint32_t>(queueFamilyIndices.size());
        bi.pQueueFamilyIndices = queueFamilyIndices.data();
    } else {
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    const VkResult createRes = vkCreateBuffer(device, &bi, nullptr, &buffer);
    if (createRes != VK_SUCCESS) {
        vkutil::throwVkError("vkCreateBuffer", createRes);
    }

    VkMemoryDedicatedRequirements dedicatedReq{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS };
    VkMemoryRequirements2 req2{ VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
    req2.pNext = &dedicatedReq;
    VkBufferMemoryRequirementsInfo2 reqInfo{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2 };
    reqInfo.buffer = buffer;
    vkGetBufferMemoryRequirements2(device, &reqInfo, &req2);
    const VkMemoryRequirements req = req2.memoryRequirements;

    const VkMemoryAllocateFlags allocationFlags = requiresDeviceAddress_
        ? VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT
        : 0;

    const GpuAllocator::LifetimeClass lifetimeClass =
        (allocationPolicy_ == AllocationPolicy::Transient)
        ? GpuAllocator::LifetimeClass::Transient
        : GpuAllocator::LifetimeClass::Persistent;

    const bool useDedicatedAllocation = allocator->shouldUseDedicatedAllocation(
        req,
        dedicatedReq,
        GpuAllocator::ResourceClass::Buffer,
        lifetimeClass,
        memoryProperties,
        false);

    allocation = allocator->allocateForBuffer(req, memoryProperties, allocationFlags, buffer, useDedicatedAllocation, lifetimeClass);

    const VkResult bindRes = vkBindBufferMemory(device, buffer, allocation.memory, allocation.offset);
    if (bindRes != VK_SUCCESS) {
        if (allocator) {
            allocator->free(allocation);
        } else {
            vkFreeMemory(device, allocation.memory, nullptr);
        }
        allocation = {};
        vkDestroyBuffer(device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        vkutil::throwVkError("vkBindBufferMemory", bindRes);
    }
}

VulkanBuffer::VulkanBuffer(VulkanBuffer&& other) noexcept
    : device(std::exchange(other.device, VK_NULL_HANDLE))
    , physicalDevice(std::exchange(other.physicalDevice, VK_NULL_HANDLE))
    , buffer(std::exchange(other.buffer, VK_NULL_HANDLE))
    , size(std::exchange(other.size, 0))
    , memoryProps(std::exchange(other.memoryProps, 0))
    , allocator(std::exchange(other.allocator, nullptr))
    , allocation(std::exchange(other.allocation, GpuAllocator::Allocation{}))
    , mappedPtr(std::exchange(other.mappedPtr, nullptr))
    , mappedOffset(std::exchange(other.mappedOffset, 0))
    , mappedSize(std::exchange(other.mappedSize, 0))
    , nonCoherentAtomSize(std::exchange(other.nonCoherentAtomSize, 1))
    , requiresDeviceAddress_(std::exchange(other.requiresDeviceAddress_, false))
    , bufferDeviceAddressEnabled_(std::exchange(other.bufferDeviceAddressEnabled_, false))
    , allocationPolicy_(std::exchange(other.allocationPolicy_, AllocationPolicy::Auto))
{}

VulkanBuffer& VulkanBuffer::operator=(VulkanBuffer&& other) noexcept
{
    if (this != &other) {
        reset();
        device = std::exchange(other.device, VK_NULL_HANDLE);
        physicalDevice = std::exchange(other.physicalDevice, VK_NULL_HANDLE);
        buffer = std::exchange(other.buffer, VK_NULL_HANDLE);
        size = std::exchange(other.size, 0);
        memoryProps = std::exchange(other.memoryProps, 0);
        allocator = std::exchange(other.allocator, nullptr);
        allocation = std::exchange(other.allocation, GpuAllocator::Allocation{});
        mappedPtr = std::exchange(other.mappedPtr, nullptr);
        mappedOffset = std::exchange(other.mappedOffset, 0);
        mappedSize = std::exchange(other.mappedSize, 0);
        nonCoherentAtomSize = std::exchange(other.nonCoherentAtomSize, 1);
        requiresDeviceAddress_ = std::exchange(other.requiresDeviceAddress_, false);
        bufferDeviceAddressEnabled_ = std::exchange(other.bufferDeviceAddressEnabled_, false);
        allocationPolicy_ = std::exchange(other.allocationPolicy_, AllocationPolicy::Auto);
    }
    return *this;
}

VulkanBuffer::~VulkanBuffer() noexcept
{
    reset();
}

void VulkanBuffer::reset() noexcept
{
    unmap();

    if (buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
    }
    if (allocation.memory != VK_NULL_HANDLE) {
        if (allocator) {
            allocator->free(allocation);
        } else {
            vkFreeMemory(device, allocation.memory, nullptr);
        }
        allocation = {};
    }

    device = VK_NULL_HANDLE;
    physicalDevice = VK_NULL_HANDLE;
    size = 0;
    memoryProps = 0;
    allocator = nullptr;
    nonCoherentAtomSize = 1;
    requiresDeviceAddress_ = false;
    bufferDeviceAddressEnabled_ = false;
    allocationPolicy_ = AllocationPolicy::Auto;
}


vkutil::VkExpected<void*> VulkanBuffer::mapResult(VkDeviceSize offset, VkDeviceSize mapSize)
{
    if (!valid()) {
        return vkutil::VkExpected<void*>(VK_ERROR_INITIALIZATION_FAILED);
    }
    if (!isHostVisible()) {
        return vkutil::VkExpected<void*>(VK_ERROR_MEMORY_MAP_FAILED);
    }

    if (offset > size) {
        return vkutil::VkExpected<void*>(VK_ERROR_INITIALIZATION_FAILED);
    }

    const VkDeviceSize normalizedSize = (mapSize == VK_WHOLE_SIZE)
        ? (size - offset)
        : mapSize;

    if (normalizedSize > (size - offset)) {
        return vkutil::VkExpected<void*>(VK_ERROR_INITIALIZATION_FAILED);
    }

    if (mappedPtr) {
        if (mappedOffset != offset || mappedSize != normalizedSize) {
            return vkutil::VkExpected<void*>(VK_ERROR_MEMORY_MAP_FAILED);
        }
        return vkutil::VkExpected<void*>(mappedPtr);
    }

    void* ptr = nullptr;
    const VkResult mapRes = vkMapMemory(device, allocation.memory, allocation.offset + offset, mapSize, 0, &ptr);
    if (mapRes != VK_SUCCESS) {
        return vkutil::VkExpected<void*>(mapRes);
    }
    mappedPtr = ptr;
    mappedOffset = offset;
    mappedSize = normalizedSize;
    return vkutil::VkExpected<void*>(mappedPtr);
}


void* VulkanBuffer::map(VkDeviceSize offset, VkDeviceSize mapSize)
{
    auto res = mapResult(offset, mapSize);
    if (!res.hasValue()) {
        vkutil::throwVkError("VulkanBuffer::map", res.error());
    }
    return res.value();
}

void VulkanBuffer::unmap() noexcept
{
    if (mappedPtr && allocation.memory != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
        vkUnmapMemory(device, allocation.memory);
        mappedPtr = nullptr;
        mappedOffset = 0;
        mappedSize = 0;
    }
}

vkutil::VkExpected<void> VulkanBuffer::flushResult(VkDeviceSize offset, VkDeviceSize flushSize) const
{
    if (!valid()) {
        return vkutil::VkExpected<void>(VK_ERROR_INITIALIZATION_FAILED);
    }
    if (!isHostVisible()) {
        return vkutil::VkExpected<void>(VK_ERROR_MEMORY_MAP_FAILED);
    }
    if (isHostCoherent()) {
        return vkutil::VkExpected<void>();
    }

    auto rangeRes = prepareMappedRange(offset, flushSize, "flush");
    if (!rangeRes.hasValue()) { return vkutil::VkExpected<void>(rangeRes.error()); }
    const VkMappedMemoryRange range = rangeRes.value();
    const VkResult flushRes = vkFlushMappedMemoryRanges(device, 1, &range);
    if (flushRes != VK_SUCCESS) {
        return vkutil::VkExpected<void>(flushRes);
    }
    return vkutil::VkExpected<void>();
}

vkutil::VkExpected<void> VulkanBuffer::invalidateResult(VkDeviceSize offset, VkDeviceSize invalidateSize) const
{
    if (!valid()) {
        return vkutil::VkExpected<void>(VK_ERROR_INITIALIZATION_FAILED);
    }
    if (!isHostVisible()) {
        return vkutil::VkExpected<void>(VK_ERROR_MEMORY_MAP_FAILED);
    }
    if (isHostCoherent()) {
        return vkutil::VkExpected<void>();
    }

    auto rangeRes = prepareMappedRange(offset, invalidateSize, "invalidate");
    if (!rangeRes.hasValue()) { return vkutil::VkExpected<void>(rangeRes.error()); }
    const VkMappedMemoryRange range = rangeRes.value();
    const VkResult invalRes = vkInvalidateMappedMemoryRanges(device, 1, &range);
    if (invalRes != VK_SUCCESS) {
        return vkutil::VkExpected<void>(invalRes);
    }
    return vkutil::VkExpected<void>();
}

void VulkanBuffer::flush(VkDeviceSize offset, VkDeviceSize flushSize) const
{
    auto res = flushResult(offset, flushSize);
    if (!res.hasValue()) {
        vkutil::throwVkError("VulkanBuffer::flush", res.error());
    }
}

void VulkanBuffer::invalidate(VkDeviceSize offset, VkDeviceSize invalidateSize) const
{
    auto res = invalidateResult(offset, invalidateSize);
    if (!res.hasValue()) {
        vkutil::throwVkError("VulkanBuffer::invalidate", res.error());
    }
}

vkutil::VkExpected<VkMappedMemoryRange> VulkanBuffer::prepareMappedRange(VkDeviceSize offset, VkDeviceSize requestedSize, const char* opName) const
{
    (void)opName;
    if (!mappedPtr) {
        return vkutil::VkExpected<VkMappedMemoryRange>(VK_ERROR_MEMORY_MAP_FAILED);
    }

    if (offset > mappedSize) {
        return vkutil::VkExpected<VkMappedMemoryRange>(VK_ERROR_INITIALIZATION_FAILED);
    }

    const VkDeviceSize normalizedSize = (requestedSize == VK_WHOLE_SIZE)
        ? (mappedSize - offset)
        : requestedSize;

    if (normalizedSize > (mappedSize - offset)) {
        return vkutil::VkExpected<VkMappedMemoryRange>(VK_ERROR_INITIALIZATION_FAILED);
    }

    const VkDeviceSize atom = std::max<VkDeviceSize>(1, nonCoherentAtomSize);

    const VkDeviceSize absoluteOffset = allocation.offset + mappedOffset + offset;
    const VkDeviceSize alignedOffset = absoluteOffset - (absoluteOffset % atom);
    const VkDeviceSize absoluteEnd = absoluteOffset + normalizedSize;

    VkDeviceSize alignedSize = VK_WHOLE_SIZE;
    if (absoluteEnd < (allocation.offset + size)) {
        const VkDeviceSize roundedEnd = ((absoluteEnd + atom - 1) / atom) * atom;
        alignedSize = roundedEnd - alignedOffset;
    }

    VkMappedMemoryRange range{ VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };
    range.memory = allocation.memory;
    range.offset = alignedOffset;
    range.size = alignedSize;
    return vkutil::VkExpected<VkMappedMemoryRange>(range);
}

bool VulkanBuffer::usageSupportsDeviceAddress(VkBufferUsageFlags usage) noexcept
{
    return (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0;
}

void VulkanBuffer::validateDeviceAddressRequirements(VkBufferUsageFlags usage) const
{
    const bool usageRequestsAddress = usageSupportsDeviceAddress(usage);
    if ((requiresDeviceAddress_ || usageRequestsAddress) && !bufferDeviceAddressEnabled_) {
        throw std::runtime_error("VulkanBuffer: device address requested but feature is not enabled");
    }
}

void VulkanBuffer::validateAllocationPolicy(VkMemoryPropertyFlags memoryProperties) const
{
    const bool hostVisible = (memoryProperties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
    switch (allocationPolicy_) {
    case AllocationPolicy::Upload:
    case AllocationPolicy::Readback:
        if (!hostVisible) {
            throw std::runtime_error("VulkanBuffer: Upload/Readback policy requires HOST_VISIBLE memory");
        }
        break;
    case AllocationPolicy::DeviceLocal:
        if ((memoryProperties & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == 0) {
            throw std::runtime_error("VulkanBuffer: DeviceLocal policy requires DEVICE_LOCAL memory");
        }
        break;
    case AllocationPolicy::Transient:
    case AllocationPolicy::Auto:
    default:
        break;
    }
}


vkutil::VkExpected<VkDeviceAddress> VulkanBuffer::deviceAddress() const
{
    if (!valid()) {
        static_cast<void>(vkutil::makeError("VulkanBuffer::deviceAddress", VK_ERROR_INITIALIZATION_FAILED, "buffer"));
        return vkutil::VkExpected<VkDeviceAddress>(VK_ERROR_INITIALIZATION_FAILED);
    }
    if (!bufferDeviceAddressEnabled_) {
        static_cast<void>(vkutil::makeError("VulkanBuffer::deviceAddress", VK_ERROR_FEATURE_NOT_PRESENT, "buffer"));
        return vkutil::VkExpected<VkDeviceAddress>(VK_ERROR_FEATURE_NOT_PRESENT);
    }

    VkBufferDeviceAddressInfo info{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
    info.buffer = buffer;
    return vkutil::VkExpected<VkDeviceAddress>(vkGetBufferDeviceAddress(device, &info));
}
