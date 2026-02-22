#include "RenderExtractSys.h"

#include "../components/LocalToWorldComp.h"
#include "../components/RenderComp.h"
#include "../components/RotationComp.h"
#include "../components/VisibilityComp.h"

#include <algorithm>
#include <unordered_map>

namespace {}

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

    std::unordered_map<uint32_t, RenderViewPacket> viewMap{};
    std::unordered_map<ChunkKey, bool, ChunkKeyHash> seen{};

    world.forEachChunk<>(plan, [&](World::ChunkView view) {
        const ChunkKey key{ view.handle.archetypeId, view.handle.chunkIndex };
        seen[key] = true;
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

            std::ranges::stable_sort(rebuilt.draws, [](const DrawPacket& a, const DrawPacket& b) {
                if (a.materialId != b.materialId) {
                    return a.materialId < b.materialId;
                }
                return a.worldEntityId < b.worldEntityId;
            });
            uint32_t first = 0;
            for (uint32_t i = 1; i <= rebuilt.draws.size(); ++i) {
                if (i == rebuilt.draws.size() || rebuilt.draws[i].materialId != rebuilt.draws[first].materialId) {
                    rebuilt.localMaterialBatches.push_back(MaterialBatchPacket{ rebuilt.draws[first].materialId, first, i - first });
                    first = i;
                }
            }
            it = chunkCache_.insert_or_assign(key, std::move(rebuilt)).first;
        }
        else {
            lastReusedChunkCount_ += 1;
        }

        for (const auto& v : it->second.views) {
            viewMap.insert_or_assign(v.viewId, v);
        }
        const uint32_t drawBase = static_cast<uint32_t>(output.drawPackets.size());
        output.drawPackets.insert(output.drawPackets.end(), it->second.draws.begin(), it->second.draws.end());
        for (const MaterialBatchPacket& local : it->second.localMaterialBatches) {
            output.materialBatches.push_back(MaterialBatchPacket{
                .materialId = local.materialId,
                .firstDrawPacket = drawBase + local.firstDrawPacket,
                .drawPacketCount = local.drawPacketCount
            });
        }
    });

    std::erase_if(chunkCache_, [&](const auto& kv) { return !seen.contains(kv.first); });

    for (const auto& [_, view] : viewMap) {
        output.views.push_back(view);
    }
    std::ranges::sort(output.views, {}, &RenderViewPacket::viewId);

    std::ranges::stable_sort(output.drawPackets, [](const DrawPacket& a, const DrawPacket& b) {
        if (a.materialId != b.materialId) {
            return a.materialId < b.materialId;
        }
        if (a.viewId != b.viewId) {
            return a.viewId < b.viewId;
        }
        return a.worldEntityId < b.worldEntityId;
    });

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
