#include "RenderExtractSys.h"

#include "../components/LocalToWorldComp.h"
#include "../components/RenderComp.h"
#include "../components/RotationComp.h"
#include "../components/VisibilityComp.h"

#include <algorithm>
#include <unordered_set>
#include <vector>

namespace {
bool drawLess(const DrawPacket& a, const DrawPacket& b)
{
    if (a.materialId != b.materialId) {
        return a.materialId < b.materialId;
    }
    if (a.viewId != b.viewId) {
        return a.viewId < b.viewId;
    }
    if (a.angleRadians != b.angleRadians) {
        return a.angleRadians < b.angleRadians;
    }
    return a.worldEntityId < b.worldEntityId;
}
}

FrameGraphInput RenderExtractSys::build(const World& world) const
{
    FrameGraphInput output{};
    lastRebuiltChunkCount_ = 0;
    lastReusedChunkCount_ = 0;
    output.runTransferStage = true;
    output.runComputeStage = true;

    auto builder = world.queryBuilder();
    builder.include<RenderComp>()
        .include<RotationComp>()
        .include<VisibilityComp>()
        .include<LocalToWorldComp>();
    const auto& plan = builder.plan();

    const auto renderType = world.componentTypeId<RenderComp>();
    const auto rotType = world.componentTypeId<RotationComp>();
    const auto visType = world.componentTypeId<VisibilityComp>();
    const auto l2wType = world.componentTypeId<LocalToWorldComp>();

    std::vector<ChunkKey> seenKeys{};
    std::vector<std::pair<ChunkKey, const CachedChunkOutput*>> orderedCacheSlices{};

    world.forEachChunk<>(plan, [&](World::ChunkView view) {
        const ChunkKey key{ view.handle.archetypeId, view.handle.chunkIndex };
        seenKeys.push_back(key);
        ChunkExtractStamp stamp{
            .rotChunkVersion = world.chunkVersion(view.handle, *rotType),
            .visChunkVersion = world.chunkVersion(view.handle, *visType),
            .l2wChunkVersion = world.chunkVersion(view.handle, *l2wType),
            .renderChunkVersion = world.chunkVersion(view.handle, *renderType),
            .visDirtyEpoch = world.chunkDirtyEpoch(view.handle, *visType),
            .l2wDirtyEpoch = world.chunkDirtyEpoch(view.handle, *l2wType),
            .structuralVersion = world.chunkStructuralVersion(view.handle)
        };

        auto it = chunkCache_.find(key);
        if (it == chunkCache_.end() || !(it->second.stamp == stamp)) {
            lastRebuiltChunkCount_ += 1;
            CachedChunkOutput rebuilt{};
            rebuilt.stamp = stamp;
            for (size_t i = 0; i < view.count; ++i) {
                const Entity entity = view.entities[i];
                const auto* render = world.getComponent<RenderComp>(entity);
                const auto* rotation = world.getComponent<RotationComp>(entity);
                const auto* visibility = world.getComponent<VisibilityComp>(entity);
                const auto* l2w = world.getComponent<LocalToWorldComp>(entity);
                if (render == nullptr || rotation == nullptr || visibility == nullptr || l2w == nullptr) {
                    continue;
                }
                if (!visibility->visible || !render->visible) {
                    continue;
                }
                rebuilt.views.push_back(RenderViewPacket{ render->viewId, render->clearColor });
                rebuilt.draws.push_back(DrawPacket{
                    .viewId = render->viewId,
                    .materialId = render->materialId,
                    .vertexCount = render->vertexCount,
                    .firstVertex = render->firstVertex,
                    .angleRadians = rotation->angleRadians,
                    .worldPosition = { l2w->m[12], l2w->m[13], l2w->m[14] },
                    .worldEntityId = entity.id
                });
            }

            std::ranges::stable_sort(rebuilt.draws, drawLess);
            it = chunkCache_.insert_or_assign(key, std::move(rebuilt)).first;
        }
        else {
            lastReusedChunkCount_ += 1;
        }

        orderedCacheSlices.push_back({ key, &it->second });
    });

    std::ranges::sort(seenKeys, [](const ChunkKey& a, const ChunkKey& b) {
        if (a.archetypeId != b.archetypeId) {
            return a.archetypeId < b.archetypeId;
        }
        return a.chunkIndex < b.chunkIndex;
    });
    seenKeys.erase(std::unique(seenKeys.begin(), seenKeys.end()), seenKeys.end());

    std::unordered_set<ChunkKey, ChunkKeyHash> seenSet{};
    for (const ChunkKey& key : seenKeys) {
        seenSet.insert(key);
    }
    std::erase_if(chunkCache_, [&](const auto& kv) { return !seenSet.contains(kv.first); });

    std::ranges::sort(orderedCacheSlices, [](const auto& a, const auto& b) {
        if (a.first.archetypeId != b.first.archetypeId) {
            return a.first.archetypeId < b.first.archetypeId;
        }
        return a.first.chunkIndex < b.first.chunkIndex;
    });

    for (const auto& [_, cached] : orderedCacheSlices) {
        output.drawPackets.insert(output.drawPackets.end(), cached->draws.begin(), cached->draws.end());
        output.views.insert(output.views.end(), cached->views.begin(), cached->views.end());
    }

    std::ranges::sort(output.views, [](const RenderViewPacket& a, const RenderViewPacket& b) {
        return a.viewId < b.viewId;
    });
    output.views.erase(std::unique(output.views.begin(), output.views.end(), [](const RenderViewPacket& a, const RenderViewPacket& b) {
        return a.viewId == b.viewId;
    }), output.views.end());

    std::ranges::stable_sort(output.drawPackets, drawLess);

    output.materialBatches.clear();
    if (!output.drawPackets.empty()) {
        uint32_t first = 0;
        for (uint32_t i = 1; i <= output.drawPackets.size(); ++i) {
            if (i == output.drawPackets.size() || output.drawPackets[i].materialId != output.drawPackets[first].materialId) {
                output.materialBatches.push_back(MaterialBatchPacket{
                    .materialId = output.drawPackets[first].materialId,
                    .firstDrawPacket = first,
                    .drawPacketCount = i - first
                });
                first = i;
            }
        }
    }

    return output;
}
