#include "RenderExtractSys.h"

#include "../components/LightComp.h"
#include "../components/LocalToWorldComp.h"
#include "../components/RenderComp.h"
#include "../components/WorldBoundsComp.h"
#include "../components/RotationComp.h"
#include "../components/VisibilityComp.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <vector>

namespace {
bool instanceLess(const RenderInstanceSnapshot& a, const RenderInstanceSnapshot& b)
{
    if (a.material.id != b.material.id) {
        return a.material.id < b.material.id;
    }
    if (a.viewId != b.viewId) {
        return a.viewId < b.viewId;
    }
    return a.entityId < b.entityId;
}

bool lightLess(const RenderLightSnapshot& a, const RenderLightSnapshot& b)
{
    return a.lightId < b.lightId;
}

bool validTransform(const std::array<float, 16>& m)
{
    for (float value : m) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    return true;
}
} // namespace

RenderWorldSnapshot RenderExtractSys::build(const World& world, uint64_t simulationFrameIndex) const
{
    RenderWorldSnapshot output{};
    output.simulationFrameIndex = simulationFrameIndex;
    lastRebuiltChunkCount_ = 0;
    lastReusedChunkCount_ = 0;
    lastDiagnostics_ = {};

    auto builder = world.queryBuilder();
    builder.include<RenderComp>()
        .include<RotationComp>()
        .include<VisibilityComp>()
        .include<LocalToWorldComp>()
        .include<WorldBoundsComp>();
    const auto& plan = builder.plan();

    const auto renderType = world.componentTypeId<RenderComp>();
    const auto rotType = world.componentTypeId<RotationComp>();
    const auto visType = world.componentTypeId<VisibilityComp>();
    const auto l2wType = world.componentTypeId<LocalToWorldComp>();
    const auto wbType = world.componentTypeId<WorldBoundsComp>();

    std::vector<ChunkKey> seenKeys{};
    std::vector<std::pair<ChunkKey, const CachedChunkOutput*>> orderedCacheSlices{};

    world.forEachChunk<>(plan, [&](World::ChunkView view) {
        const ChunkKey key{ view.handle.archetypeId, view.handle.chunkIndex };
        seenKeys.push_back(key);
        ChunkExtractStamp stamp{
            .rotChunkVersion = world.chunkVersion(view.handle, *rotType),
            .visChunkVersion = world.chunkVersion(view.handle, *visType),
            .l2wChunkVersion = world.chunkVersion(view.handle, *l2wType),
            .worldBoundsChunkVersion = world.chunkVersion(view.handle, *wbType),
            .renderChunkVersion = world.chunkVersion(view.handle, *renderType),
            .visDirtyEpoch = world.chunkDirtyEpoch(view.handle, *visType),
            .l2wDirtyEpoch = world.chunkDirtyEpoch(view.handle, *l2wType),
            .worldBoundsDirtyEpoch = world.chunkDirtyEpoch(view.handle, *wbType),
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
                const auto* worldBounds = world.getComponent<WorldBoundsComp>(entity);
                if (render == nullptr || rotation == nullptr || visibility == nullptr || l2w == nullptr) {
                    continue;
                }
                if (!visibility->visible || !render->visible) {
                    continue;
                }
                if (render->materialId == 0 || render->meshId == 0 || !validTransform(l2w->m)) {
                    if (render->materialId == 0 || render->meshId == 0) {
                        lastDiagnostics_.droppedInvalidHandles += 1;
                    }
                    if (!validTransform(l2w->m)) {
                        lastDiagnostics_.droppedInvalidTransforms += 1;
                    }
                    continue;
                }

                std::optional<RenderInstanceSnapshot::Bounds> bounds{};
                if (worldBounds != nullptr) {
                    bool validBounds = std::isfinite(worldBounds->radius) && worldBounds->radius >= 0.0F;
                    for (float value : worldBounds->center) {
                        if (!std::isfinite(value)) {
                            validBounds = false;
                            break;
                        }
                    }

                    if (!validBounds) {
                        lastDiagnostics_.droppedInvalidBounds += 1;
                    }
                    else {
                        bounds = RenderInstanceSnapshot::Bounds{
                            .center = worldBounds->center,
                            .radius = worldBounds->radius
                        };
                    }
                }

                rebuilt.views.push_back(RenderViewSnapshot{ render->viewId, render->clearColor });
                rebuilt.instances.push_back(RenderInstanceSnapshot{
                    .instanceId = (static_cast<uint64_t>(view.handle.archetypeId) << 32ULL)
                        ^ static_cast<uint64_t>(entity.id),
                    .viewId = render->viewId,
                    .entityId = entity.id,
                    .localToWorld = l2w->m,
                    .mesh = RenderMeshRef{
                        .id = render->meshId,
                        .vertexCount = render->vertexCount,
                        .firstVertex = render->firstVertex,
                    },
                    .material = RenderMaterialRef{ .id = render->materialId },
                    .visibilityMask = 0xFFFFFFFFu,
                    .worldBounds = bounds
                });
            }

            std::ranges::stable_sort(rebuilt.instances, instanceLess);
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
        output.instances.insert(output.instances.end(), cached->instances.begin(), cached->instances.end());
        output.views.insert(output.views.end(), cached->views.begin(), cached->views.end());
    }

    std::ranges::sort(output.views, [](const RenderViewSnapshot& a, const RenderViewSnapshot& b) {
        return a.viewId < b.viewId;
    });
    output.views.erase(std::unique(output.views.begin(), output.views.end(), [](const RenderViewSnapshot& a, const RenderViewSnapshot& b) {
        return a.viewId == b.viewId;
    }), output.views.end());

    std::ranges::stable_sort(output.instances, instanceLess);
    {
        std::unordered_set<uint64_t> seen{};
        seen.reserve(output.instances.size());
        std::vector<RenderInstanceSnapshot> uniqueInstances{};
        uniqueInstances.reserve(output.instances.size());
        for (const RenderInstanceSnapshot& instance : output.instances) {
            if (!seen.insert(instance.instanceId).second) {
                lastDiagnostics_.duplicateInstanceIds += 1;
                continue;
            }
            uniqueInstances.push_back(instance);
        }
        output.instances = std::move(uniqueInstances);
    }

    if (!output.instances.empty()) {
        uint32_t first = 0;
        for (uint32_t i = 1; i <= output.instances.size(); ++i) {
            if (i == output.instances.size() || output.instances[i].material.id != output.instances[first].material.id) {
                output.materialGroups.push_back(RenderMaterialGroupSnapshot{
                    .materialId = output.instances[first].material.id,
                    .firstInstance = first,
                    .instanceCount = i - first
                });
                first = i;
            }
        }
    }

    auto lightBuilder = world.queryBuilder();
    lightBuilder.include<LightComp>();
    const auto& lightPlan = lightBuilder.plan();
    world.forEachChunk<>(lightPlan, [&](World::ChunkView view) {
        for (size_t i = 0; i < view.count; ++i) {
            const Entity entity = view.entities[i];
            const auto* light = world.getComponent<LightComp>(entity);
            if (light == nullptr || !std::isfinite(light->intensity)) {
                continue;
            }
            output.lights.push_back(RenderLightSnapshot{
                .lightId = entity.id,
                .worldPosition = light->worldPosition,
                .intensity = light->intensity
            });
        }
    });
    std::ranges::sort(output.lights, lightLess);

    return output;
}
