#pragma once

#include <Engine.h>
#include <ecs/World.h>

#include <unordered_map>

class RenderExtractSys final {
public:
    [[nodiscard]] FrameGraphInput build(const World& world) const;
    [[nodiscard]] uint32_t lastRebuiltChunkCount() const noexcept { return lastRebuiltChunkCount_; }
    [[nodiscard]] uint32_t lastReusedChunkCount() const noexcept { return lastReusedChunkCount_; }

private:
    struct ChunkKey {
        uint32_t archetypeId{ 0 };
        uint32_t chunkIndex{ 0 };
        friend bool operator==(const ChunkKey&, const ChunkKey&) = default;
    };

    struct ChunkKeyHash {
        size_t operator()(const ChunkKey& k) const noexcept
        {
            return (static_cast<size_t>(k.archetypeId) << 32u) ^ static_cast<size_t>(k.chunkIndex);
        }
    };

    struct ChunkExtractStamp {
        uint64_t rotChunkVersion{ 0 };
        uint64_t visChunkVersion{ 0 };
        uint64_t l2wChunkVersion{ 0 };
        uint64_t renderChunkVersion{ 0 };
        uint64_t visDirtyEpoch{ 0 };
        uint64_t l2wDirtyEpoch{ 0 };
        uint64_t structuralVersion{ 0 };
        friend bool operator==(const ChunkExtractStamp&, const ChunkExtractStamp&) = default;
    };

    struct CachedChunkOutput {
        ChunkExtractStamp stamp{};
        std::vector<RenderViewPacket> views{};
        std::vector<DrawPacket> draws{};
    };

    mutable std::unordered_map<ChunkKey, CachedChunkOutput, ChunkKeyHash> chunkCache_{};
    mutable uint32_t lastRebuiltChunkCount_{ 0 };
    mutable uint32_t lastReusedChunkCount_{ 0 };
};
