#include "GpuAllocator.h"

#include <algorithm>
#include <stdexcept>

namespace {
constexpr VkDeviceSize kMinBlockSize = 4ull * 1024ull * 1024ull;

[[nodiscard]] constexpr size_t resourceClassIndex(GpuAllocator::ResourceClass resourceClass) noexcept
{
    return static_cast<size_t>(resourceClass);
}

[[nodiscard]] constexpr size_t lifetimeClassIndex(GpuAllocator::LifetimeClass lifetimeClass) noexcept
{
    return static_cast<size_t>(lifetimeClass);
}
}

GpuAllocator::GpuAllocator(VkDevice device, VkPhysicalDevice physicalDevice,
    bool bufferDeviceAddressEnabled,
    VkDeviceSize defaultPoolBlockSize,
    VkDeviceSize dedicatedThreshold)
    : device_(device)
    , physicalDevice_(physicalDevice)
    , bufferDeviceAddressEnabled_(bufferDeviceAddressEnabled)
    , defaultPoolBlockSize_(std::max(defaultPoolBlockSize, kMinBlockSize))
    , dedicatedThreshold_(std::max(dedicatedThreshold, kMinBlockSize))
{
    if (device_ == VK_NULL_HANDLE || physicalDevice_ == VK_NULL_HANDLE) {
        throw std::runtime_error("GpuAllocator: invalid device or physical device");
    }

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physicalDevice_, &props);
    nonCoherentAtomSize_ = std::max<VkDeviceSize>(1, props.limits.nonCoherentAtomSize);
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memProps_);
}

GpuAllocator::~GpuAllocator() noexcept
{
    reset();
}

VkDeviceSize GpuAllocator::alignUp(VkDeviceSize value, VkDeviceSize alignment) noexcept
{
    if (alignment <= 1) {
        return value;
    }
    return ((value + alignment - 1) / alignment) * alignment;
}

uint32_t GpuAllocator::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (uint32_t i = 0; i < memProps_.memoryTypeCount; ++i) {
        const bool typeOk = (typeBits & (1u << i)) != 0;
        const bool flagsOk = (memProps_.memoryTypes[i].propertyFlags & props) == props;
        if (typeOk && flagsOk) {
            return i;
        }
    }
    throw std::runtime_error("GpuAllocator: no suitable memory type found");
}

uint64_t GpuAllocator::makePoolKey(uint32_t memoryTypeIndex, VkMemoryAllocateFlags allocateFlags) noexcept
{
    return (static_cast<uint64_t>(allocateFlags) << 32) | static_cast<uint64_t>(memoryTypeIndex);
}

GpuAllocator::MemoryBlock& GpuAllocator::createPooledBlock(uint32_t memoryTypeIndex, VkMemoryAllocateFlags allocateFlags, VkDeviceSize minSize)
{
    const VkDeviceSize blockSize = std::max(defaultPoolBlockSize_, minSize);

    VkMemoryAllocateFlagsInfo allocFlagsInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO };
    allocFlagsInfo.flags = allocateFlags;

    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = blockSize;
    ai.memoryTypeIndex = memoryTypeIndex;
    ai.pNext = (allocateFlags != 0) ? &allocFlagsInfo : nullptr;

    VkDeviceMemory memory = VK_NULL_HANDLE;
    const VkResult allocRes = vkAllocateMemory(device_, &ai, nullptr, &memory);
    if (allocRes != VK_SUCCESS) {
        throw std::runtime_error("GpuAllocator: vkAllocateMemory failed while creating pooled block");
    }

    const uint64_t poolKey = makePoolKey(memoryTypeIndex, allocateFlags);
    auto& blocks = pooledBlocks_[poolKey];
    blocks.push_back(MemoryBlock{
        .memory = memory,
        .size = blockSize,
        .memoryTypeIndex = memoryTypeIndex,
        .poolKey = poolKey,
        .allocateFlags = allocateFlags,
        .freeRanges = { FreeRange{ 0, blockSize } }
        });

    return blocks.back();
}

void GpuAllocator::mergeFreeRanges(std::vector<FreeRange>& ranges)
{
    if (ranges.empty()) {
        return;
    }

    std::sort(ranges.begin(), ranges.end(), [](const FreeRange& a, const FreeRange& b) {
        return a.offset < b.offset;
        });

    size_t out = 0;
    for (size_t i = 1; i < ranges.size(); ++i) {
        FreeRange& curr = ranges[out];
        const FreeRange& next = ranges[i];
        if (curr.offset + curr.size == next.offset) {
            curr.size += next.size;
        }
        else {
            ++out;
            ranges[out] = next;
        }
    }
    ranges.resize(out + 1);
}

bool GpuAllocator::shouldUseDedicatedAllocation(const VkMemoryRequirements& req,
    const VkMemoryDedicatedRequirements& dedicatedReq,
    ResourceClass resourceClass,
    LifetimeClass lifetimeClass,
    VkMemoryPropertyFlags properties,
    bool forceDedicated) const noexcept
{
    if (forceDedicated || dedicatedReq.requiresDedicatedAllocation == VK_TRUE) {
        return true;
    }
    if (dedicatedReq.prefersDedicatedAllocation == VK_TRUE) {
        return true;
    }

    if (resourceClass == ResourceClass::Image && req.size >= dedicatedThreshold_ / 2) {
        return true;
    }

    if (lifetimeClass == LifetimeClass::Transient) {
        return false;
    }

    const bool deviceLocal = (properties & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
    if (deviceLocal && req.size >= dedicatedThreshold_) {
        return true;
    }
    return req.size >= (dedicatedThreshold_ * 2);
}

GpuAllocator::Allocation GpuAllocator::allocateForBuffer(
    const VkMemoryRequirements& req,
    VkMemoryPropertyFlags properties,
    VkMemoryAllocateFlags allocateFlags,
    VkBuffer dedicatedBuffer,
    bool forceDedicated,
    LifetimeClass lifetimeClass)
{
    return allocateInternal(req, properties, allocateFlags, forceDedicated, dedicatedBuffer, VK_NULL_HANDLE, ResourceClass::Buffer, lifetimeClass);
}

GpuAllocator::Allocation GpuAllocator::allocateForImage(
    const VkMemoryRequirements& req,
    VkMemoryPropertyFlags properties,
    VkImage dedicatedImage,
    bool forceDedicated,
    LifetimeClass lifetimeClass)
{
    return allocateInternal(req, properties, 0, forceDedicated, VK_NULL_HANDLE, dedicatedImage, ResourceClass::Image, lifetimeClass);
}

GpuAllocator::Allocation GpuAllocator::allocateInternal(const VkMemoryRequirements& req,
    VkMemoryPropertyFlags properties,
    VkMemoryAllocateFlags allocateFlags,
    bool forceDedicated,
    VkBuffer dedicatedBuffer,
    VkImage dedicatedImage,
    ResourceClass resourceClass,
    LifetimeClass lifetimeClass)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!valid()) {
        throw std::runtime_error("GpuAllocator::allocateInternal called on invalid allocator");
    }

    if ((allocateFlags & VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT) != 0 && !bufferDeviceAddressEnabled_) {
        throw std::runtime_error("GpuAllocator::allocateInternal: device address allocation requested but feature is disabled");
    }

    uint32_t memoryTypeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < memProps_.memoryTypeCount; ++i) {
        const bool typeOk = (req.memoryTypeBits & (1u << i)) != 0;
        const bool flagsOk = (memProps_.memoryTypes[i].propertyFlags & properties) == properties;
        if (typeOk && flagsOk) {
            memoryTypeIndex = i;
            break;
        }
    }
    if (memoryTypeIndex == UINT32_MAX) {
        throw std::runtime_error("GpuAllocator: no suitable memory type found");
    }

    const uint64_t poolKey = makePoolKey(memoryTypeIndex, allocateFlags);
    const VkDeviceSize requestSize = req.size;
    const VkDeviceSize requestAlign = std::max<VkDeviceSize>(1, req.alignment);

    const bool useDedicatedAllocation = forceDedicated || requestSize >= dedicatedThreshold_;
    if (useDedicatedAllocation) {
        VkMemoryAllocateFlagsInfo allocFlagsInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO };
        allocFlagsInfo.flags = allocateFlags;

        VkMemoryDedicatedAllocateInfo dedicatedInfo{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
        dedicatedInfo.buffer = dedicatedBuffer;
        dedicatedInfo.image = dedicatedImage;

        VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ai.allocationSize = requestSize;
        ai.memoryTypeIndex = memoryTypeIndex;

        if (dedicatedBuffer != VK_NULL_HANDLE || dedicatedImage != VK_NULL_HANDLE) {
            ai.pNext = &dedicatedInfo;
            if (allocateFlags != 0) {
                dedicatedInfo.pNext = &allocFlagsInfo;
            }
        }
        else if (allocateFlags != 0) {
            ai.pNext = &allocFlagsInfo;
        }

        Allocation out{};
        out.memoryTypeIndex = memoryTypeIndex;
        out.poolKey = poolKey;
        out.allocateFlags = allocateFlags;
        out.size = requestSize;
        out.offset = 0;
        out.dedicated = true;
        out.resourceClass = resourceClass;
        out.lifetimeClass = lifetimeClass;

        const VkResult allocRes = vkAllocateMemory(device_, &ai, nullptr, &out.memory);
        if (allocRes != VK_SUCCESS) {
            throw std::runtime_error("GpuAllocator: dedicated vkAllocateMemory failed");
        }
        allocationCount_.fetch_add(1, std::memory_order_relaxed);
        dedicatedAllocationCount_.fetch_add(1, std::memory_order_relaxed);
        bytesAllocated_.fetch_add(requestSize, std::memory_order_relaxed);
        allocationCountByResourceClass_[resourceClassIndex(resourceClass)].fetch_add(1, std::memory_order_relaxed);
        bytesAllocatedByResourceClass_[resourceClassIndex(resourceClass)].fetch_add(requestSize, std::memory_order_relaxed);
        bytesAllocatedByLifetimeClass_[lifetimeClassIndex(lifetimeClass)].fetch_add(requestSize, std::memory_order_relaxed);
        return out;
    }

    auto& blocks = pooledBlocks_[poolKey];
    for (auto& block : blocks) {
        for (size_t i = 0; i < block.freeRanges.size(); ++i) {
            auto range = block.freeRanges[i];
            const VkDeviceSize alignedOffset = alignUp(range.offset, requestAlign);
            const VkDeviceSize endOffset = alignedOffset + requestSize;
            if (endOffset > (range.offset + range.size)) {
                continue;
            }

            block.freeRanges.erase(block.freeRanges.begin() + static_cast<std::ptrdiff_t>(i));
            if (alignedOffset > range.offset) {
                block.freeRanges.push_back({ range.offset, alignedOffset - range.offset });
            }
            if (endOffset < (range.offset + range.size)) {
                block.freeRanges.push_back({ endOffset, (range.offset + range.size) - endOffset });
            }
            mergeFreeRanges(block.freeRanges);

            allocationCount_.fetch_add(1, std::memory_order_relaxed);
            pooledAllocationCount_.fetch_add(1, std::memory_order_relaxed);
            bytesAllocated_.fetch_add(requestSize, std::memory_order_relaxed);
            allocationCountByResourceClass_[resourceClassIndex(resourceClass)].fetch_add(1, std::memory_order_relaxed);
            bytesAllocatedByResourceClass_[resourceClassIndex(resourceClass)].fetch_add(requestSize, std::memory_order_relaxed);
            bytesAllocatedByLifetimeClass_[lifetimeClassIndex(lifetimeClass)].fetch_add(requestSize, std::memory_order_relaxed);
            return Allocation{
                .memory = block.memory,
                .offset = alignedOffset,
                .size = requestSize,
                .memoryTypeIndex = memoryTypeIndex,
                .poolKey = poolKey,
                .allocateFlags = allocateFlags,
                .dedicated = false,
                .resourceClass = resourceClass,
                .lifetimeClass = lifetimeClass
            };
        }
    }

    MemoryBlock& newBlock = createPooledBlock(memoryTypeIndex, allocateFlags, std::max(defaultPoolBlockSize_, requestSize + requestAlign));
    const VkDeviceSize alignedOffset = alignUp(0, requestAlign);
    const VkDeviceSize endOffset = alignedOffset + requestSize;
    newBlock.freeRanges.clear();
    if (endOffset < newBlock.size) {
        newBlock.freeRanges.push_back({ endOffset, newBlock.size - endOffset });
    }

    allocationCount_.fetch_add(1, std::memory_order_relaxed);
    pooledAllocationCount_.fetch_add(1, std::memory_order_relaxed);
    bytesAllocated_.fetch_add(requestSize, std::memory_order_relaxed);
    allocationCountByResourceClass_[resourceClassIndex(resourceClass)].fetch_add(1, std::memory_order_relaxed);
    bytesAllocatedByResourceClass_[resourceClassIndex(resourceClass)].fetch_add(requestSize, std::memory_order_relaxed);
    bytesAllocatedByLifetimeClass_[lifetimeClassIndex(lifetimeClass)].fetch_add(requestSize, std::memory_order_relaxed);
    return Allocation{
        .memory = newBlock.memory,
        .offset = alignedOffset,
        .size = requestSize,
        .memoryTypeIndex = memoryTypeIndex,
        .poolKey = poolKey,
        .allocateFlags = allocateFlags,
        .dedicated = false,
        .resourceClass = resourceClass,
        .lifetimeClass = lifetimeClass
    };
}

void GpuAllocator::free(const Allocation& allocation) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!valid() || allocation.memory == VK_NULL_HANDLE) {
        return;
    }

    if (allocation.dedicated) {
        vkFreeMemory(device_, allocation.memory, nullptr);
        freeCount_.fetch_add(1, std::memory_order_relaxed);
        bytesFreed_.fetch_add(allocation.size, std::memory_order_relaxed);
        bytesFreedByResourceClass_[resourceClassIndex(allocation.resourceClass)].fetch_add(allocation.size, std::memory_order_relaxed);
        bytesFreedByLifetimeClass_[lifetimeClassIndex(allocation.lifetimeClass)].fetch_add(allocation.size, std::memory_order_relaxed);
        return;
    }

    auto it = pooledBlocks_.find(allocation.poolKey);
    if (it == pooledBlocks_.end()) {
        return;
    }

    for (auto& block : it->second) {
        if (block.memory != allocation.memory) {
            continue;
        }
        block.freeRanges.push_back({ allocation.offset, allocation.size });
        mergeFreeRanges(block.freeRanges);
        freeCount_.fetch_add(1, std::memory_order_relaxed);
        bytesFreed_.fetch_add(allocation.size, std::memory_order_relaxed);
        bytesFreedByResourceClass_[resourceClassIndex(allocation.resourceClass)].fetch_add(allocation.size, std::memory_order_relaxed);
        bytesFreedByLifetimeClass_[lifetimeClassIndex(allocation.lifetimeClass)].fetch_add(allocation.size, std::memory_order_relaxed);
        return;
    }
}

void GpuAllocator::reset() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [_, blocks] : pooledBlocks_) {
        for (auto& block : blocks) {
            if (block.memory != VK_NULL_HANDLE) {
                vkFreeMemory(device_, block.memory, nullptr);
                block.memory = VK_NULL_HANDLE;
            }
        }
    }
    pooledBlocks_.clear();

    device_ = VK_NULL_HANDLE;
    physicalDevice_ = VK_NULL_HANDLE;
    memProps_ = VkPhysicalDeviceMemoryProperties{};
    bufferDeviceAddressEnabled_ = false;
    nonCoherentAtomSize_ = 1;

    allocationCount_.store(0, std::memory_order_relaxed);
    freeCount_.store(0, std::memory_order_relaxed);
    bytesAllocated_.store(0, std::memory_order_relaxed);
    bytesFreed_.store(0, std::memory_order_relaxed);
    dedicatedAllocationCount_.store(0, std::memory_order_relaxed);
    pooledAllocationCount_.store(0, std::memory_order_relaxed);
    for (size_t i = 0; i < bytesAllocatedByResourceClass_.size(); ++i) {
        bytesAllocatedByResourceClass_[i].store(0, std::memory_order_relaxed);
        bytesFreedByResourceClass_[i].store(0, std::memory_order_relaxed);
        allocationCountByResourceClass_[i].store(0, std::memory_order_relaxed);
        bytesAllocatedByLifetimeClass_[i].store(0, std::memory_order_relaxed);
        bytesFreedByLifetimeClass_[i].store(0, std::memory_order_relaxed);
    }
}

GpuAllocator::Telemetry GpuAllocator::telemetry() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    uint32_t poolCount = 0;
    uint64_t freeBytes = 0;
    uint64_t totalBytes = 0;
    for (const auto& [_, blocks] : pooledBlocks_) {
        poolCount += static_cast<uint32_t>(blocks.size());
        for (const auto& block : blocks) {
            totalBytes += block.size;
            for (const auto& range : block.freeRanges) {
                freeBytes += range.size;
            }
        }
    }

    const uint64_t allocated = bytesAllocated_.load(std::memory_order_relaxed);
    const uint64_t freed = bytesFreed_.load(std::memory_order_relaxed);
    const uint64_t inUse = (allocated >= freed) ? (allocated - freed) : 0;
    const double fragmentationRatio = totalBytes == 0 ? 0.0 : static_cast<double>(freeBytes) / static_cast<double>(totalBytes);

    Telemetry telemetry{};
    telemetry.allocationCount = allocationCount_.load(std::memory_order_relaxed);
    telemetry.freeCount = freeCount_.load(std::memory_order_relaxed);
    telemetry.bytesAllocated = allocated;
    telemetry.bytesFreed = freed;
    telemetry.bytesInUse = inUse;
    telemetry.dedicatedAllocationCount = dedicatedAllocationCount_.load(std::memory_order_relaxed);
    telemetry.pooledAllocationCount = pooledAllocationCount_.load(std::memory_order_relaxed);
    telemetry.poolCount = poolCount;
    telemetry.freeBytes = freeBytes;
    telemetry.totalBytes = totalBytes;
    telemetry.fragmentationRatio = fragmentationRatio;

    for (size_t i = 0; i < telemetry.bytesAllocatedByResourceClass.size(); ++i) {
        telemetry.bytesAllocatedByResourceClass[i] = bytesAllocatedByResourceClass_[i].load(std::memory_order_relaxed);
        telemetry.bytesFreedByResourceClass[i] = bytesFreedByResourceClass_[i].load(std::memory_order_relaxed);
        telemetry.allocationCountByResourceClass[i] = allocationCountByResourceClass_[i].load(std::memory_order_relaxed);
        telemetry.bytesAllocatedByLifetimeClass[i] = bytesAllocatedByLifetimeClass_[i].load(std::memory_order_relaxed);
        telemetry.bytesFreedByLifetimeClass[i] = bytesFreedByLifetimeClass_[i].load(std::memory_order_relaxed);
    }

    return telemetry;
}
