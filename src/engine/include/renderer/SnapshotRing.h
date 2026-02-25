#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>

namespace renderer {

template <typename T, size_t N>
class SnapshotRing {
    // Concurrency contract: this ring is intentionally Single-Producer / Single-Consumer (SPSC).
    // Exactly one writer thread may call beginWrite/publish, and exactly one reader thread may
    // call beginReadStaged/endRead concurrently.
    //
    // The API shape keeps staged immutable handoff semantics for renderer decoupling while
    // avoiding MPMC arbitration complexity by design.

public:
    enum class SlotState : uint8_t {
        Free,
        Writing,
        Published,
        Reading
    };

    struct SlotMetadata {
        uint64_t writeEpoch{ 0 };
        uint64_t readEpoch{ 0 };
        SlotState state{ SlotState::Free };
        mutable std::mutex mutex{};
        mutable std::condition_variable cv{};
    };

    struct WriteTicket {
        T* snapshot{ nullptr };
        uint32_t slotIndex{ 0 };
        uint64_t writeEpoch{ 0 };
    };

    struct ReadTicket {
        const T* snapshot{ nullptr };
        uint32_t slotIndex{ 0 };
        uint64_t readEpoch{ 0 };
    };

    [[nodiscard]] WriteTicket beginWrite()
    {
        static_assert(N >= 2, "SnapshotRing requires at least two slots for staged handoff");
        while (true) {
            for (size_t attempt = 0; attempt < N; ++attempt) {
                const uint32_t slot = nextWriteSlot_;
                nextWriteSlot_ = (nextWriteSlot_ + 1u) % N;

                SlotMetadata& meta = slotsMeta_[slot];
                std::unique_lock lock(meta.mutex);
                if (meta.state == SlotState::Free) {
                    meta.state = SlotState::Writing;
                    const uint64_t writeEpoch = publishedEpoch_.load(std::memory_order_acquire) + 1u;
                    meta.writeEpoch = writeEpoch;
                    return WriteTicket{ .snapshot = &slots_[slot], .slotIndex = slot, .writeEpoch = writeEpoch };
                }
            }

            waitForAnyFreeSlot();
        }
    }

    void publish(const WriteTicket& ticket)
    {
        SlotMetadata& written = slotsMeta_[ticket.slotIndex];
        {
            std::scoped_lock lock(written.mutex);
            written.state = SlotState::Published;
            written.writeEpoch = ticket.writeEpoch;
        }

        const int32_t previousCurrent = publishedSlot_.exchange(static_cast<int32_t>(ticket.slotIndex), std::memory_order_acq_rel);
        const int32_t stalePrevious = previousPublishedSlot_.exchange(previousCurrent, std::memory_order_acq_rel);
        publishedEpoch_.store(ticket.writeEpoch, std::memory_order_release);

        if (stalePrevious >= 0 && stalePrevious != previousCurrent) {
            SlotMetadata& stale = slotsMeta_[static_cast<size_t>(stalePrevious)];
            std::scoped_lock lock(stale.mutex);
            if (stale.state == SlotState::Published) {
                stale.state = SlotState::Free;
                stale.cv.notify_all();
            }
        }

        written.cv.notify_all();
    }

    [[nodiscard]] std::optional<ReadTicket> beginReadStaged() const
    {
        const int32_t readSlot = previousPublishedSlot_.load(std::memory_order_acquire);
        if (readSlot < 0) {
            return std::nullopt;
        }

        SlotMetadata& meta = slotsMeta_[static_cast<size_t>(readSlot)];
        std::unique_lock lock(meta.mutex);
        if (meta.state != SlotState::Published) {
            return std::nullopt;
        }

        meta.state = SlotState::Reading;
        meta.readEpoch = meta.writeEpoch;

        return ReadTicket{
            .snapshot = &slots_[static_cast<size_t>(readSlot)],
            .slotIndex = static_cast<uint32_t>(readSlot),
            .readEpoch = meta.readEpoch
        };
    }

    void endRead(const ReadTicket& ticket) const
    {
        SlotMetadata& meta = slotsMeta_[ticket.slotIndex];
        {
            std::scoped_lock lock(meta.mutex);
            meta.state = SlotState::Free;
        }
        meta.cv.notify_all();
    }

    void reset()
    {
        for (SlotMetadata& meta : slotsMeta_) {
            std::scoped_lock lock(meta.mutex);
            meta.writeEpoch = 0;
            meta.readEpoch = 0;
            meta.state = SlotState::Free;
            meta.cv.notify_all();
        }
        publishedEpoch_.store(0, std::memory_order_release);
        previousPublishedSlot_.store(-1, std::memory_order_release);
        publishedSlot_.store(-1, std::memory_order_release);
        nextWriteSlot_ = 0;
    }

private:
    void waitForAnyFreeSlot()
    {
        for (SlotMetadata& meta : slotsMeta_) {
            std::unique_lock lock(meta.mutex);
            if (meta.state == SlotState::Free) {
                return;
            }
            meta.cv.wait_for(lock, std::chrono::microseconds(50), [&]() { return meta.state == SlotState::Free; });
            if (meta.state == SlotState::Free) {
                return;
            }
        }
        std::this_thread::yield();
    }

    std::array<T, N> slots_{};
    mutable std::array<SlotMetadata, N> slotsMeta_{};
    std::atomic<uint64_t> publishedEpoch_{ 0 };
    std::atomic<int32_t> previousPublishedSlot_{ -1 };
    std::atomic<int32_t> publishedSlot_{ -1 };
    uint32_t nextWriteSlot_{ 0 };
};

} // namespace renderer
