// UniqueHandle.h
#pragma once

#include <utility>
#include <type_traits>
#include <cstdint>
#include <functional>
#include <concepts>
#include <cassert>
#include <memory>
#include <optional>
#include <cstdlib>
#include <cstdio>

// parasoft-begin-suppress ALL "suppress all violations"
#include <vulkan/vulkan.h>
// parasoft-end-suppress ALL "suppress all violations"

namespace vkhandle {

template<typename ParentHandle, typename Handle, typename DestroyFn>
concept DestroyInvocable = std::invocable<DestroyFn, ParentHandle, Handle, const VkAllocationCallbacks*>;

enum class DeleteMode : uint8_t {
    Immediate,
    Deferred
};

enum class DeferredFallbackPolicy : uint8_t {
    BestEffortImmediate,
    StrictRequireQueue
};

enum class InvariantViolationPolicy : uint8_t {
    Abort,
    ImmediateFallback,
    ReportAndLeakSafely
};

[[nodiscard]] constexpr InvariantViolationPolicy defaultInvariantViolationPolicy() noexcept
{
#ifndef NDEBUG
    return InvariantViolationPolicy::Abort;
#else
    return InvariantViolationPolicy::ImmediateFallback;
#endif
}

template<typename ParentHandle, typename Handle, typename DestroyFn>
class IDeletionQueue {
public:
    virtual ~IDeletionQueue() = default;
    [[nodiscard]] virtual bool requiresRetireValue() const noexcept = 0;
    virtual void enqueue(ParentHandle parent,
        Handle handle,
        DestroyFn destroyFn,
        std::optional<VkAllocationCallbacks> allocator,
        std::optional<uint64_t> retireAfterValue) noexcept = 0;
};

template<typename ParentHandle, typename Handle, typename DestroyFn>
requires DestroyInvocable<ParentHandle, Handle, DestroyFn>
class UniqueHandle {
public:
    using parent_type = ParentHandle;
    using handle_type = Handle;
    using destroy_type = DestroyFn;
    using deletion_queue_type = IDeletionQueue<ParentHandle, Handle, DestroyFn>;

    UniqueHandle() noexcept = default;

    UniqueHandle(ParentHandle parent, Handle h, DestroyFn d,
        const VkAllocationCallbacks* allocator = nullptr,
        const std::shared_ptr<deletion_queue_type>& deletionQueue = {},
        DeleteMode mode = DeleteMode::Immediate,
        DeferredFallbackPolicy deferredFallbackPolicy = DeferredFallbackPolicy::BestEffortImmediate,
        InvariantViolationPolicy invariantPolicy = defaultInvariantViolationPolicy()) noexcept(std::is_nothrow_move_constructible_v<DestroyFn>)
        : parent_(parent)
        , handle_(h)
        , destroy_(std::move(d))
        , deletionQueue_(deletionQueue)
        , deleteMode_(mode)
        , deferredFallbackPolicy_(deferredFallbackPolicy)
        , invariantPolicy_(invariantPolicy)
    {
        setAllocator(allocator);
    }

    ~UniqueHandle() noexcept { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept(
        std::is_nothrow_move_constructible_v<DestroyFn>&&
        std::is_nothrow_move_assignable_v<DestroyFn>)
    {
        *this = std::move(other);
    }

    UniqueHandle& operator=(UniqueHandle&& other) noexcept(std::is_nothrow_move_assignable_v<DestroyFn>)
    {
        if (this != &other) {
            reset();

            parent_ = other.parent_;
            handle_ = other.handle_;
            destroy_ = std::move(other.destroy_);
            allocatorStorage_ = other.allocatorStorage_;
            deletionQueue_ = other.deletionQueue_;
            deleteMode_ = other.deleteMode_;
            deferredFallbackPolicy_ = other.deferredFallbackPolicy_;
            deferredRetireAfterValue_ = other.deferredRetireAfterValue_;
            invariantPolicy_ = other.invariantPolicy_;

            other.parent_ = ParentHandle{};
            other.handle_ = Handle{};
            other.allocatorStorage_.reset();
            other.deletionQueue_.reset();
            other.deleteMode_ = DeleteMode::Immediate;
            other.deferredFallbackPolicy_ = DeferredFallbackPolicy::BestEffortImmediate;
            other.deferredRetireAfterValue_.reset();
            other.invariantPolicy_ = defaultInvariantViolationPolicy();
        }
        return *this;
    }

    void swap(UniqueHandle& other) noexcept
    {
        static_assert(std::is_nothrow_swappable_v<DestroyFn>, "DestroyFn must be nothrow swappable");
        std::swap(parent_, other.parent_);
        std::swap(handle_, other.handle_);
        std::swap(destroy_, other.destroy_);
        std::swap(allocatorStorage_, other.allocatorStorage_);
        std::swap(deletionQueue_, other.deletionQueue_);
        std::swap(deleteMode_, other.deleteMode_);
        std::swap(deferredFallbackPolicy_, other.deferredFallbackPolicy_);
        std::swap(deferredRetireAfterValue_, other.deferredRetireAfterValue_);
        std::swap(invariantPolicy_, other.invariantPolicy_);
    }

    [[nodiscard]] Handle get() const noexcept { return handle_; }
    [[nodiscard]] ParentHandle getParent() const noexcept { return parent_; }
    [[nodiscard]] VkDevice getDevice() const noexcept requires std::is_same_v<ParentHandle, VkDevice> { return parent_; }
    [[nodiscard]] const VkAllocationCallbacks* getAllocator() const noexcept
    {
        return allocatorStorage_.has_value() ? &allocatorStorage_.value() : nullptr;
    }
    [[nodiscard]] DeleteMode deleteMode() const noexcept { return deleteMode_; }
    [[nodiscard]] std::optional<uint64_t> deferredRetireAfterValue() const noexcept { return deferredRetireAfterValue_; }
    [[nodiscard]] InvariantViolationPolicy invariantPolicy() const noexcept { return invariantPolicy_; }

    [[nodiscard]] explicit operator bool() const noexcept { return handle_ != Handle{}; }

    void setDeleteMode(DeleteMode mode) noexcept { deleteMode_ = mode; }
    void setDeferredFallbackPolicy(DeferredFallbackPolicy policy) noexcept { deferredFallbackPolicy_ = policy; }
    void setDeletionQueue(const std::shared_ptr<deletion_queue_type>& queue) noexcept { deletionQueue_ = queue; }
    void setDeferredRetireAfterValue(std::optional<uint64_t> retireAfterValue) noexcept
    {
        deferredRetireAfterValue_ = retireAfterValue;
    }
    void setInvariantPolicy(InvariantViolationPolicy policy) noexcept
    {
        invariantPolicy_ = policy;
    }

    void reset() noexcept
    {
        if (handle_ != Handle{}) {
            assert(parent_ != ParentHandle{} && "UniqueHandle: non-null handle without valid parent");
            if (parent_ == ParentHandle{}) {
                if (handleInvariantViolation("UniqueHandle invariant violation: non-null handle without valid parent", false)) {
                    clearState();
                    return;
                }
            }

            const VkAllocationCallbacks* allocator = getAllocator();
            if (deleteMode_ == DeleteMode::Deferred) {
                if (const auto queue = deletionQueue_.lock()) {
                    if (queue->requiresRetireValue() && !deferredRetireAfterValue_.has_value()) {
                        if (handleInvariantViolation("UniqueHandle invariant violation: deferred deletion requires retireAfterValue", true)) {
                            clearState();
                            return;
                        }
                    }
                    else {
                        queue->enqueue(parent_, handle_, destroy_, allocatorStorage_, deferredRetireAfterValue_);
                    }
                }
                else {
                    if (deferredFallbackPolicy_ == DeferredFallbackPolicy::StrictRequireQueue) {
                        if (handleInvariantViolation("UniqueHandle invariant violation: strict deferred deletion queue unavailable", true)) {
                            clearState();
                            return;
                        }
                    }
                    else {
                        destroy_(parent_, handle_, allocator);
                    }
                }
            }
            else {
                destroy_(parent_, handle_, allocator);
            }
        }
        clearState();
    }

    void reset(ParentHandle parent, Handle h, DestroyFn d,
        const VkAllocationCallbacks* allocator = nullptr,
        const std::shared_ptr<deletion_queue_type>& deletionQueue = {},
        DeleteMode mode = DeleteMode::Immediate,
        DeferredFallbackPolicy deferredFallbackPolicy = DeferredFallbackPolicy::BestEffortImmediate,
        InvariantViolationPolicy invariantPolicy = defaultInvariantViolationPolicy()) noexcept(std::is_nothrow_move_assignable_v<DestroyFn>)
    {
        reset();
        parent_ = parent;
        handle_ = h;
        destroy_ = std::move(d);
        setAllocator(allocator);
        deletionQueue_ = deletionQueue;
        deleteMode_ = mode;
        deferredFallbackPolicy_ = deferredFallbackPolicy;
        deferredRetireAfterValue_.reset();
        invariantPolicy_ = invariantPolicy;
    }

    [[nodiscard]] Handle release() noexcept
    {
        Handle out = handle_;
        clearState();
        return out;
    }

private:
    void setAllocator(const VkAllocationCallbacks* allocator) noexcept
    {
        if (allocator != nullptr) {
            allocatorStorage_ = *allocator;
        }
        else {
            allocatorStorage_.reset();
        }
    }

    void clearState() noexcept
    {
        handle_ = Handle{};
        parent_ = ParentHandle{};
        allocatorStorage_.reset();
        deletionQueue_.reset();
        deleteMode_ = DeleteMode::Immediate;
        deferredFallbackPolicy_ = DeferredFallbackPolicy::BestEffortImmediate;
        deferredRetireAfterValue_.reset();
        invariantPolicy_ = defaultInvariantViolationPolicy();
    }

    [[nodiscard]] bool handleInvariantViolation(const char* message, bool canImmediateDestroy) noexcept
    {
        reportInvariantViolation(message, canImmediateDestroy);
        if (invariantPolicy_ == InvariantViolationPolicy::Abort) {
            std::abort();
        }

        if (invariantPolicy_ == InvariantViolationPolicy::ImmediateFallback && canImmediateDestroy) {
            destroy_(parent_, handle_, getAllocator());
            return true;
        }

        return true;
    }

    template<typename T>
    [[nodiscard]] static uintptr_t toAddressLikeValue(T value) noexcept
    {
        if constexpr (std::is_pointer_v<T>) {
            return reinterpret_cast<uintptr_t>(value);
        }
        else {
            return static_cast<uintptr_t>(value);
        }
    }

    void reportInvariantViolation(const char* message, bool canImmediateDestroy) const noexcept
    {
        std::fprintf(
            stderr,
            "%s [policy=%u canImmediateDestroy=%u deleteMode=%u hasQueue=%u hasRetireValue=%u parent=0x%llx handle=0x%llx]\n",
            message,
            static_cast<unsigned>(invariantPolicy_),
            static_cast<unsigned>(canImmediateDestroy ? 1u : 0u),
            static_cast<unsigned>(deleteMode_),
            static_cast<unsigned>(deletionQueue_.expired() ? 0u : 1u),
            static_cast<unsigned>(deferredRetireAfterValue_.has_value() ? 1u : 0u),
            static_cast<unsigned long long>(toAddressLikeValue(parent_)),
            static_cast<unsigned long long>(toAddressLikeValue(handle_)));
    }

    ParentHandle parent_{ ParentHandle{} };
    Handle handle_{ Handle{} };
    DestroyFn destroy_{};
    std::optional<VkAllocationCallbacks> allocatorStorage_{};
    std::weak_ptr<deletion_queue_type> deletionQueue_{};
    DeleteMode deleteMode_{ DeleteMode::Immediate };
    DeferredFallbackPolicy deferredFallbackPolicy_{ DeferredFallbackPolicy::BestEffortImmediate };
    std::optional<uint64_t> deferredRetireAfterValue_{};
    InvariantViolationPolicy invariantPolicy_{ defaultInvariantViolationPolicy() };
};

template<typename Handle, typename DestroyFn>
using DeviceUniqueHandle = UniqueHandle<VkDevice, Handle, DestroyFn>;

template<typename Handle, typename DestroyFn>
using InstanceUniqueHandle = UniqueHandle<VkInstance, Handle, DestroyFn>;

template<typename ParentHandle, typename Handle, typename DestroyFn>
inline void swap(UniqueHandle<ParentHandle, Handle, DestroyFn>& a,
    UniqueHandle<ParentHandle, Handle, DestroyFn>& b) noexcept
{
    a.swap(b);
}

} // namespace vkhandle
