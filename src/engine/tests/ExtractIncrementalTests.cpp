#include <app/ecs/systems/RenderExtractSys.h>

#include <app/ecs/components/LocalToWorldComp.h>
#include <app/ecs/components/RenderComp.h>
#include <app/ecs/components/RotationComp.h>
#include <app/ecs/components/VisibilityComp.h>

#include <ecs/World.h>

#include <cassert>
#include <vector>


namespace {
bool sameDraws(const std::vector<DrawPacket>& a, const std::vector<DrawPacket>& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].viewId != b[i].viewId || a[i].materialId != b[i].materialId || a[i].worldEntityId != b[i].worldEntityId || a[i].angleRadians != b[i].angleRadians) {
            return false;
        }
    }
    return true;
}

bool sameBatches(const std::vector<MaterialBatchPacket>& a, const std::vector<MaterialBatchPacket>& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].materialId != b[i].materialId || a[i].firstDrawPacket != b[i].firstDrawPacket || a[i].drawPacketCount != b[i].drawPacketCount) {
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
        world.emplaceComponent<RenderComp>(e, RenderComp{ .viewId = i % 2, .materialId = (i % 4) + 1, .vertexCount = 3, .firstVertex = 0, .visible = true });
        world.emplaceComponent<RotationComp>(e, RotationComp{ .angleRadians = static_cast<float>(i), .angularVelocityRadiansPerSecond = 1.0F });
        world.emplaceComponent<VisibilityComp>(e, VisibilityComp{ .visible = true });
        world.emplaceComponent<LocalToWorldComp>(e);
        entities.push_back(e);
    }

    RenderExtractSys extract{};
    const FrameGraphInput first = extract.build(world);
    assert(extract.lastRebuiltChunkCount() > 0);

    const FrameGraphInput second = extract.build(world);
    assert(extract.lastReusedChunkCount() > 0);
    assert(extract.lastRebuiltChunkCount() == 0);
    assert(sameDraws(first.drawPackets, second.drawPackets));
    assert(sameBatches(first.materialBatches, second.materialBatches));

    world.beginSystemWriteScope();
    world.query<RotationComp>().each([&](Entity e, WriteRef<RotationComp> rot) {
        if (e.id == entities.front().id) {
            rot.touch();
            rot.get().angleRadians += 1.0F;
        }
    });
    world.endSystemWriteScope();

    const FrameGraphInput third = extract.build(world);
    assert(extract.lastRebuiltChunkCount() == 1);

    const FrameGraphInput fourth = extract.build(world);
    assert(sameDraws(third.drawPackets, fourth.drawPackets));
    assert(sameBatches(third.materialBatches, fourth.materialBatches));

    return 0;
}
