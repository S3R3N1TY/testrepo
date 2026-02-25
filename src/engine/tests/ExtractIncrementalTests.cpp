#include <app/ecs/systems/RenderExtractSys.h>

#include <app/ecs/components/LocalToWorldComp.h>
#include <app/ecs/components/RenderComp.h>
#include <app/ecs/components/RotationComp.h>
#include <app/ecs/components/VisibilityComp.h>

#include <ecs/World.h>

#include <cassert>
#include <vector>

namespace {
bool sameInstances(const std::vector<RenderInstanceSnapshot>& a, const std::vector<RenderInstanceSnapshot>& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].viewId != b[i].viewId || a[i].material.id != b[i].material.id || a[i].entityId != b[i].entityId) {
            return false;
        }
    }
    return true;
}
}

int main()
{
    World world{};
    std::vector<Entity> entities{};
    entities.reserve(130);
    for (uint32_t i = 0; i < 130; ++i) {
        Entity e = world.createEntity();
        world.emplaceComponent<RenderComp>(e, RenderComp{ .viewId = i % 2, .materialId = (i % 4) + 1, .meshId = 1, .vertexCount = 3, .firstVertex = 0, .visible = true });
        world.emplaceComponent<RotationComp>(e, RotationComp{ .angleRadians = static_cast<float>(i), .angularVelocityRadiansPerSecond = 1.0F });
        world.emplaceComponent<VisibilityComp>(e, VisibilityComp{ .visible = true });
        world.emplaceComponent<LocalToWorldComp>(e);
        entities.push_back(e);
    }

    RenderExtractSys extract{};
    const RenderWorldSnapshot first = extract.build(world, 1);
    assert(extract.lastRebuiltChunkCount() > 0);

    const RenderWorldSnapshot second = extract.build(world, 2);
    assert(extract.lastReusedChunkCount() > 0);
    assert(extract.lastRebuiltChunkCount() == 0);
    assert(sameInstances(first.instances, second.instances));
    assert(!first.materialGroups.empty());

    world.beginSystemWriteScope();
    world.query<RotationComp>().each([&](Entity e, WriteRef<RotationComp> rot) {
        if (e.id == entities.front().id) {
            rot.touch();
            rot.get().angleRadians += 1.0F;
        }
    });
    world.endSystemWriteScope();

    const RenderWorldSnapshot third = extract.build(world, 3);
    assert(extract.lastRebuiltChunkCount() == 1);

    const RenderWorldSnapshot fourth = extract.build(world, 4);
    assert(sameInstances(third.instances, fourth.instances));

    return 0;
}
