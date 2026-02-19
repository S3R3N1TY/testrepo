#pragma once

#include <utility> // std::exchange
#include <vector>
#include <cstdint>
#include <optional>
#include <mutex>
#include <memory>
#include <atomic>
#include <deque>

// parasoft-begin-suppress ALL "suppress all violations"
#include <vulkan/vulkan.h>
// parasoft-end-suppress ALL "suppress all violations"

#include "VkUtils.h"
#include "VkSync.h"
// ===================== Command pool =====================

class VulkanCommandPool {
public:
    VulkanCommandPool() noexcept = default;
    [[nodiscard]] static vkutil::VkExpected<VulkanCommandPool> create(VkDevice device, uint32_t queueFamilyIndex);

    VulkanCommandPool(const VulkanCommandPool&) = delete;
    VulkanCommandPool& operator=(const VulkanCommandPool&) = delete;

    VulkanCommandPool(VulkanCommandPool&& other) noexcept;
    VulkanCommandPool& operator=(VulkanCommandPool&& other) noexcept;

    ~VulkanCommandPool() noexcept;

    [[nodiscard]] VkCommandPool get() const noexcept { return commandPool; }
    [[nodiscard]] VkDevice      getDevice() const noexcept { return device; }
    [[nodiscard]] bool          valid() const noexcept { return commandPool != VK_NULL_HANDLE; }

    // Destroys the pool (and all command buffers allocated from it)
    void reset() noexcept;

private:
    VulkanCommandPool(VkDevice device, VkCommandPool commandPool) noexcept
        : device(device), commandPool(commandPool) {}

    friend class VulkanCommandBuffer;

    VkDevice      device{ VK_NULL_HANDLE };
    VkCommandPool commandPool{ VK_NULL_HANDLE };
};

// ===================== Command buffer =====================

class VulkanCommandBuffer {
public:
    VulkanCommandBuffer() noexcept = default;

    VulkanCommandBuffer(const VulkanCommandBuffer&) = delete;
    VulkanCommandBuffer& operator=(const VulkanCommandBuffer&) = delete;

    VulkanCommandBuffer(VulkanCommandBuffer&& other) noexcept;
    VulkanCommandBuffer& operator=(VulkanCommandBuffer&& other) noexcept;

    ~VulkanCommandBuffer() noexcept;

    [[nodiscard]] VkCommandBuffer get() const noexcept { return commandBuffer; }
    [[nodiscard]] VkDevice        getDevice() const noexcept { return device; }
    [[nodiscard]] VkCommandPool   getPool() const noexcept { return commandPool; }
    [[nodiscard]] bool            valid() const noexcept { return commandBuffer != VK_NULL_HANDLE; }

    // Frees this command buffer back to its pool
    void reset() noexcept;

    // Reset recording state (vkResetCommandBuffer)
    [[nodiscard]] vkutil::VkExpected<void> resetRecording(VkCommandBufferResetFlags flags = 0);

    [[nodiscard]] vkutil::VkExpected<void> begin(VkCommandBufferUsageFlags flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
    [[nodiscard]] vkutil::VkExpected<void> beginSecondary(const VkCommandBufferInheritanceInfo& inheritance, VkCommandBufferUsageFlags flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
    [[nodiscard]] vkutil::VkExpected<void> end();

    [[nodiscard]] static vkutil::VkExpected<VulkanCommandBuffer> create(VkDevice device, VkCommandPool commandPool);

private:
    VulkanCommandBuffer(VkDevice device, VkCommandPool commandPool, VkCommandBuffer commandBuffer) noexcept
        : device(device), commandPool(commandPool), commandBuffer(commandBuffer) {}

    VkDevice        device{ VK_NULL_HANDLE };
    VkCommandPool   commandPool{ VK_NULL_HANDLE };
    VkCommandBuffer commandBuffer{ VK_NULL_HANDLE };
};


class VulkanCommandArena {
public:
    struct Config {
        VkDevice device{ VK_NULL_HANDLE };
        uint32_t queueFamilyIndex{ 0 };
        uint32_t framesInFlight{ 0 };
        uint32_t workerThreads{ 1 };
        uint32_t preallocatePerFrame{ 8 };
        VkCommandPoolCreateFlags poolFlags{ VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
        bool waitForIdleOnDestroy{ false };
    };

    enum class FrameLifecycleState : uint8_t { Available, InFlight, Retired };

    enum class FrameWaitPolicy : uint8_t { Poll, Wait };

    enum class CommandBufferLevel : uint8_t { Primary, Secondary };
    enum class SecondaryRecordingMode : uint8_t { LegacyRenderPass, DynamicRendering };

    struct FrameSyncState {
        FrameLifecycleState lifecycle{ FrameLifecycleState::Available };
        bool signaled{ true };
        SyncTicket ticket{};
    };

    struct FrameToken {
        uint32_t frameIndex{ 0 };
        uint64_t epoch{ 0 };

        [[nodiscard]] bool valid() const noexcept { return epoch != 0; }
    };

    struct BorrowedCommandBuffer {
        VkCommandBuffer handle{ VK_NULL_HANDLE };
        uint32_t workerIndex{ 0 };
        uint32_t frameIndex{ 0 };
        uint64_t generation{ 0 };
        uint64_t epoch{ 0 };
        CommandBufferLevel level{ CommandBufferLevel::Primary };

        [[nodiscard]] bool valid() const noexcept { return handle != VK_NULL_HANDLE; }
    };

    struct BorrowedValidation {
        bool valid{ false };
        bool invalidHandle{ false };
        bool invalidWorkerIndex{ false };
        bool invalidFrameIndex{ false };
        bool staleGeneration{ false };
        bool staleEpoch{ false };

        [[nodiscard]] const char* reason() const noexcept;
    };

    class CommandRecorder {
    public:
        CommandRecorder() noexcept = default;
        CommandRecorder(VulkanCommandArena* arena, BorrowedCommandBuffer borrowed) noexcept
            : arena_(arena), borrowed_(borrowed)
        {
        }

        CommandRecorder(const CommandRecorder&) = delete;
        CommandRecorder& operator=(const CommandRecorder&) = delete;

        CommandRecorder(CommandRecorder&& other) noexcept
            : arena_(std::exchange(other.arena_, nullptr))
            , borrowed_(std::exchange(other.borrowed_, BorrowedCommandBuffer{}))
            , finished_(std::exchange(other.finished_, true))
        {
        }

        CommandRecorder& operator=(CommandRecorder&& other) noexcept
        {
            if (this != &other) {
                static_cast<void>(finish());
                arena_ = std::exchange(other.arena_, nullptr);
                borrowed_ = std::exchange(other.borrowed_, BorrowedCommandBuffer{});
                finished_ = std::exchange(other.finished_, true);
            }
            return *this;
        }

        ~CommandRecorder() noexcept;

        [[nodiscard]] VkCommandBuffer handle() const noexcept { return borrowed_.handle; }
        [[nodiscard]] const BorrowedCommandBuffer& borrowed() const noexcept { return borrowed_; }
        [[nodiscard]] bool valid() const noexcept { return arena_ != nullptr && borrowed_.valid() && !finished_; }

        [[nodiscard]] vkutil::VkExpected<void> finish() noexcept;

    private:
        VulkanCommandArena* arena_{ nullptr };
        BorrowedCommandBuffer borrowed_{};
        bool finished_{ false };
    };

    VulkanCommandArena() noexcept = default;
    explicit VulkanCommandArena(const Config& config);
    [[nodiscard]] static vkutil::VkExpected<VulkanCommandArena> createResult(const Config& config);

    VulkanCommandArena(const VulkanCommandArena&) = delete;
    VulkanCommandArena& operator=(const VulkanCommandArena&) = delete;
    VulkanCommandArena(VulkanCommandArena&&) noexcept = default;
    VulkanCommandArena& operator=(VulkanCommandArena&&) noexcept = default;

    ~VulkanCommandArena() noexcept;

    [[nodiscard]] bool valid() const noexcept { return device_ != VK_NULL_HANDLE && !workers_.empty(); }

    void bindSyncContext(const SyncContext* syncContext) noexcept { syncContext_ = syncContext; }

    [[nodiscard]] vkutil::VkExpected<FrameToken> beginFrame(uint32_t frameIndex, FrameWaitPolicy waitPolicy = FrameWaitPolicy::Poll);
    [[nodiscard]] vkutil::VkExpected<FrameToken> beginFrame(uint32_t frameIndex, VkFence frameFence);
    [[nodiscard]] vkutil::VkExpected<FrameToken> beginFrame(uint32_t frameIndex, uint64_t completedValue);

    [[nodiscard]] vkutil::VkExpected<BorrowedCommandBuffer> acquirePrimary(const FrameToken& token, uint32_t workerIndex = 0,
        VkCommandBufferUsageFlags usage = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
    [[nodiscard]] vkutil::VkExpected<BorrowedCommandBuffer> acquireSecondary(const FrameToken& token,
        const VkCommandBufferInheritanceInfo& inheritance,
        uint32_t workerIndex = 0,
        VkCommandBufferUsageFlags usage = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        SecondaryRecordingMode mode = SecondaryRecordingMode::LegacyRenderPass);
    [[nodiscard]] vkutil::VkExpected<CommandRecorder> acquireRecorderPrimary(const FrameToken& token, uint32_t workerIndex = 0,
        VkCommandBufferUsageFlags usage = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
    [[nodiscard]] vkutil::VkExpected<CommandRecorder> acquireRecorderSecondary(const FrameToken& token,
        const VkCommandBufferInheritanceInfo& inheritance,
        uint32_t workerIndex = 0,
        VkCommandBufferUsageFlags usage = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        SecondaryRecordingMode mode = SecondaryRecordingMode::LegacyRenderPass);

    [[nodiscard]] vkutil::VkExpected<void> endBorrowed(const BorrowedCommandBuffer& borrowed) const;
    [[nodiscard]] bool isBorrowedValid(const BorrowedCommandBuffer& borrowed) const noexcept;
    [[nodiscard]] BorrowedValidation validateBorrowed(const BorrowedCommandBuffer& borrowed) const noexcept;

    void markFrameSubmitted(uint32_t frameIndex, uint64_t submissionValue) noexcept;
    void markFrameSubmitted(uint32_t frameIndex, const SyncTicket& ticket) noexcept;
    void markFrameComplete(uint32_t frameIndex) noexcept;

private:
    [[nodiscard]] vkutil::VkExpected<void> init(const Config& config);
    struct FrameState {
        VkCommandPool pool{ VK_NULL_HANDLE };
        std::vector<VkCommandBuffer> primaryBuffers{};
        std::vector<VkCommandBuffer> secondaryBuffers{};
        uint32_t nextPrimary{ 0 };
        uint32_t nextSecondary{ 0 };
        std::shared_ptr<std::atomic<uint64_t>> generation{ std::make_shared<std::atomic<uint64_t>>(1) };
        std::shared_ptr<std::mutex> mutex{ std::make_shared<std::mutex>() };
    };

    struct AtomicFrameSyncState {
        std::atomic<uint8_t> lifecycle{ static_cast<uint8_t>(FrameLifecycleState::Available) };
        std::atomic<bool> signaled{ true };
        std::atomic<uint64_t> ticketValue{ 0 };
        std::atomic<uint32_t> ticketFrameIndex{ 0 };
        std::atomic<uint64_t> frameEpoch{ 0 };
    };

    [[nodiscard]] vkutil::VkExpected<FrameToken> beginFrameInternalLocked(uint32_t frameIndex, std::optional<FrameSyncState> observedCompletion);
    [[nodiscard]] vkutil::VkExpected<FrameToken> beginFrameInternal(uint32_t frameIndex, std::optional<FrameSyncState> observedCompletion);
    [[nodiscard]] vkutil::VkExpected<bool> updateFrameSyncState(uint32_t frameIndex, FrameWaitPolicy waitPolicy);
    [[nodiscard]] FrameSyncState loadFrameSyncStateLocked(uint32_t frameIndex) const noexcept;
    void storeFrameSyncStateLocked(uint32_t frameIndex, const FrameSyncState& state) noexcept;
    [[nodiscard]] std::unique_lock<std::mutex> lockFrameTransition(uint32_t frameIndex);
    [[nodiscard]] vkutil::VkExpected<BorrowedCommandBuffer> acquire(const FrameToken& token, CommandBufferLevel level,
        uint32_t workerIndex, VkCommandBufferUsageFlags usage, const VkCommandBufferInheritanceInfo* inheritance,
        SecondaryRecordingMode secondaryMode);

    VkDevice device_{ VK_NULL_HANDLE };
    uint32_t framesInFlight_{ 0 };
    bool waitForIdleOnDestroy_{ false };
    std::deque<AtomicFrameSyncState> frameSync_{};
    std::vector<std::shared_ptr<std::mutex>> frameTransitionMutexes_{};
    std::vector<std::vector<FrameState>> workers_{};
    const SyncContext* syncContext_{ nullptr };
};
