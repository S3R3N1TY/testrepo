#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

// parasoft-begin-suppress ALL "suppress all violations"
#include <vulkan/vulkan.h>
// parasoft-end-suppress ALL "suppress all violations"

#include "SubmissionScheduler.h"
#include "VkUtils.h"

class RenderTaskGraph {
public:
    using ResourceId = uint32_t;
    using PassId = size_t;

    enum class ResourceAccessType : uint8_t {
        Read,
        Write,
        ReadWrite
    };

    enum class ResourceType : uint8_t {
        Global,
        Buffer,
        Image
    };

    struct ResourceDescriptor {
        ResourceType type{ ResourceType::Global };
        bool transient{ false };
        uint64_t aliasClass{ 0 };

        VkBuffer buffer{ VK_NULL_HANDLE };
        VkDeviceSize bufferOffset{ 0 };
        VkDeviceSize bufferSize{ VK_WHOLE_SIZE };
        VkDeviceSize transientBufferSize{ 0 };
        VkDeviceSize transientBufferAlignment{ 1 };

        VkImage image{ VK_NULL_HANDLE };
        VkImageSubresourceRange imageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkExtent3D transientImageExtent{ 0, 0, 0 };
        VkFormat transientImageFormat{ VK_FORMAT_UNDEFINED };
        VkImageUsageFlags transientImageUsage{ 0 };
        VkImageType transientImageType{ VK_IMAGE_TYPE_2D };
        uint32_t transientImageMipLevels{ 1 };
        uint32_t transientImageArrayLayers{ 1 };
        VkSampleCountFlagBits transientImageSamples{ VK_SAMPLE_COUNT_1_BIT };

        VkImageLayout initialImageLayout{ VK_IMAGE_LAYOUT_UNDEFINED };
        VkPipelineStageFlags2 initialStageMask{ VK_PIPELINE_STAGE_2_NONE };
        VkAccessFlags2 initialAccessMask{ VK_ACCESS_2_NONE };
        uint32_t initialQueueFamilyIndex{ VK_QUEUE_FAMILY_IGNORED };
    };

    struct ResourceUsage {
        ResourceId resource{ 0 };
        ResourceAccessType access{ ResourceAccessType::Read };
        VkPipelineStageFlags2 stageMask{ VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT };
        VkAccessFlags2 accessMask{ 0 };
        VkImageLayout imageLayout{ VK_IMAGE_LAYOUT_UNDEFINED };
        VkImageSubresourceRange imageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS };
        VkDeviceSize bufferOffset{ 0 };
        VkDeviceSize bufferSize{ VK_WHOLE_SIZE };
        uint32_t queueFamilyIndex{ VK_QUEUE_FAMILY_IGNORED };
    };

    struct BarrierBatch {
        std::vector<VkMemoryBarrier2> memoryBarriers{};
        std::vector<VkBufferMemoryBarrier2> bufferBarriers{};
        std::vector<VkImageMemoryBarrier2> imageBarriers{};

        [[nodiscard]] bool empty() const noexcept {
            return memoryBarriers.empty() && bufferBarriers.empty() && imageBarriers.empty();
        }
    };

    struct PassNode {
        SubmissionScheduler::JobRequest job{};
        std::vector<ResourceUsage> usages{};
        std::function<vkutil::VkExpected<void>(const BarrierBatch& incomingBarriers, const BarrierBatch& outgoingBarriers)> record{};
    };

    struct CompiledPass {
        PassId id{ 0 };
        size_t scheduleOrder{ 0 };
        size_t scheduleLevel{ 0 };
        SubmissionScheduler::QueueClass queueClass{ SubmissionScheduler::QueueClass::Graphics };
        BarrierBatch incomingBarriers{};
        BarrierBatch outgoingBarriers{};
    };

    struct TransientResourceLifetime {
        ResourceId resource{ 0 };
        size_t firstUseOrder{ 0 };
        size_t lastUseOrder{ 0 };
        ResourceType type{ ResourceType::Global };
    };

    struct TransientAliasAllocation {
        uint32_t aliasSlot{ 0 };
        ResourceType type{ ResourceType::Global };
        uint64_t aliasClass{ 0 };
        VkDeviceSize requiredBufferSize{ 0 };
        VkDeviceSize requiredBufferAlignment{ 1 };
        VkExtent3D requiredImageExtent{ 0, 0, 0 };
        VkFormat imageFormat{ VK_FORMAT_UNDEFINED };
        VkImageUsageFlags imageUsage{ 0 };
        VkImageType imageType{ VK_IMAGE_TYPE_2D };
        uint32_t imageMipLevels{ 1 };
        uint32_t imageArrayLayers{ 1 };
        VkSampleCountFlagBits imageSamples{ VK_SAMPLE_COUNT_1_BIT };
        std::vector<ResourceId> resources{};
    };

    struct CompiledTransientPlan {
        std::vector<TransientResourceLifetime> lifetimes{};
        std::vector<TransientAliasAllocation> aliasAllocations{};
        std::unordered_map<ResourceId, uint32_t> aliasSlotByResource{};
    };

    RenderTaskGraph() = default;

    void clear();
    [[nodiscard]] ResourceId createResource();
    [[nodiscard]] ResourceId createBufferResource(VkBuffer buffer,
        VkDeviceSize offset = 0,
        VkDeviceSize size = VK_WHOLE_SIZE,
        VkPipelineStageFlags2 initialStageMask = VK_PIPELINE_STAGE_2_NONE,
        VkAccessFlags2 initialAccessMask = VK_ACCESS_2_NONE,
        uint32_t initialQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED);
    [[nodiscard]] ResourceId createImageResource(VkImage image,
        const VkImageSubresourceRange& subresourceRange,
        VkImageLayout initialLayout,
        VkPipelineStageFlags2 initialStageMask = VK_PIPELINE_STAGE_2_NONE,
        VkAccessFlags2 initialAccessMask = VK_ACCESS_2_NONE,
        uint32_t initialQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED);
    [[nodiscard]] ResourceId createTransientBufferResource(
        VkDeviceSize size,
        VkDeviceSize alignment = 1,
        uint64_t aliasClass = 0,
        VkPipelineStageFlags2 initialStageMask = VK_PIPELINE_STAGE_2_NONE,
        VkAccessFlags2 initialAccessMask = VK_ACCESS_2_NONE,
        uint32_t initialQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED);
    [[nodiscard]] ResourceId createTransientImageResource(
        VkExtent3D extent,
        VkFormat format,
        VkImageUsageFlags usage,
        VkImageLayout initialLayout,
        uint64_t aliasClass = 0,
        VkImageType imageType = VK_IMAGE_TYPE_2D,
        uint32_t mipLevels = 1,
        uint32_t arrayLayers = 1,
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT,
        VkPipelineStageFlags2 initialStageMask = VK_PIPELINE_STAGE_2_NONE,
        VkAccessFlags2 initialAccessMask = VK_ACCESS_2_NONE,
        uint32_t initialQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED);
    [[nodiscard]] PassId addPass(PassNode pass);
    void setPresent(const SubmissionScheduler::PresentRequest& request);

    [[nodiscard]] vkutil::VkExpected<std::vector<CompiledPass>> compile() const;
    [[nodiscard]] vkutil::VkExpected<CompiledTransientPlan> compileTransientPlan() const;
    [[nodiscard]] vkutil::VkExpected<SubmissionScheduler::FrameExecutionResult> execute(SubmissionScheduler& scheduler) const;

private:
    struct Edge {
        PassId producer{ 0 };
        PassId consumer{ 0 };
        VkPipelineStageFlags2 consumerWaitStage{ VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT };
    };

    struct UsageRef {
        PassId pass{ 0 };
        ResourceUsage usage{};
    };

    struct ResourceState {
        ResourceDescriptor descriptor{};
        std::optional<UsageRef> lastWriter{};
        std::vector<UsageRef> readers{};
    };

    struct SyncContractDecision {
        bool requiresExecutionDependency{ false };
        bool requiresMemoryBarrier{ false };
        bool requiresLayoutTransition{ false };
        bool requiresQueueOwnershipTransfer{ false };
        VkPipelineStageFlags2 srcStageMask{ VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT };
        VkAccessFlags2 srcAccessMask{ VK_ACCESS_2_NONE };
        VkPipelineStageFlags2 dstStageMask{ VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT };
        VkAccessFlags2 dstAccessMask{ VK_ACCESS_2_NONE };
        VkImageLayout oldLayout{ VK_IMAGE_LAYOUT_UNDEFINED };
        VkImageLayout newLayout{ VK_IMAGE_LAYOUT_UNDEFINED };
        uint32_t srcQueueFamilyIndex{ VK_QUEUE_FAMILY_IGNORED };
        uint32_t dstQueueFamilyIndex{ VK_QUEUE_FAMILY_IGNORED };
    };

    struct ExecutionSchedule {
        std::vector<PassId> topologicalOrder{};
        std::vector<size_t> levelByPass{};
        std::vector<std::vector<PassId>> levels{};
    };

    [[nodiscard]] static bool isWriteAccess(ResourceAccessType access) noexcept;
    [[nodiscard]] static vkutil::VkExpected<void> validateUsageContract(const ResourceDescriptor& descriptor, const ResourceUsage& usage) noexcept;
    [[nodiscard]] static vkutil::VkExpected<SyncContractDecision> buildSyncContractDecision(
        const ResourceDescriptor& descriptor,
        const ResourceUsage& src,
        const ResourceUsage& dst) noexcept;
    [[nodiscard]] static bool requiresBarrier(const ResourceDescriptor& descriptor, const ResourceUsage& src, const ResourceUsage& dst) noexcept;
    [[nodiscard]] static ResourceUsage makeInitialUsage(const ResourceDescriptor& descriptor) noexcept;
    [[nodiscard]] static BarrierBatch makeBarrierBatch(const ResourceDescriptor& descriptor, const ResourceUsage& src, const ResourceUsage& dst) noexcept;
    [[nodiscard]] static BarrierBatch makeReleaseBarrierBatch(const ResourceDescriptor& descriptor, const ResourceUsage& src, const ResourceUsage& dst) noexcept;
    [[nodiscard]] static BarrierBatch makeAcquireBarrierBatch(const ResourceDescriptor& descriptor, const ResourceUsage& src, const ResourceUsage& dst) noexcept;
    [[nodiscard]] static bool imageRangesOverlap(const VkImageSubresourceRange& lhs, const VkImageSubresourceRange& rhs) noexcept;
    [[nodiscard]] static bool usagesOverlap(const ResourceDescriptor& descriptor, const ResourceUsage& lhs, const ResourceUsage& rhs) noexcept;

    [[nodiscard]] vkutil::VkExpected<void> buildDependenciesAndBarriers(
        std::vector<Edge>& outEdges,
        std::vector<BarrierBatch>& outIncomingBarriers,
        std::vector<BarrierBatch>& outOutgoingBarriers) const;
    [[nodiscard]] vkutil::VkExpected<ExecutionSchedule> buildExecutionSchedule(const std::vector<Edge>& edges) const;
    [[nodiscard]] vkutil::VkExpected<CompiledTransientPlan> buildTransientPlan(const ExecutionSchedule& schedule) const;
    [[nodiscard]] static bool transientResourcesCompatible(const ResourceDescriptor& lhs, const ResourceDescriptor& rhs) noexcept;

    std::unordered_map<ResourceId, ResourceDescriptor> resources_{};
    std::vector<PassNode> passes_{};
    std::optional<SubmissionScheduler::PresentRequest> presentRequest_{};
    ResourceId nextResourceId_{ 1 };
};
