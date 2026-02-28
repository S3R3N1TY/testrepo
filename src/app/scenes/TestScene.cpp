#include "TestScene.h"

#include "../ecs/components/PositionComp.h"
#include "../ecs/components/RenderComp.h"
#include "../ecs/components/ScaleComp.h"

namespace {
void clearWorld(World& world)
{
    const auto alive = world.entities();
    for (const Entity entity : alive) {
        world.destroyEntity(entity);
    }
}
}

void TestScene::onLoad(World& world)
{
    clearWorld(world);

    const Entity plane = world.createEntity();
    world.emplaceComponent<PositionComp>(plane);
    world.emplaceComponent<ScaleComp>(plane);
    world.emplaceComponent<RenderComp>(plane, RenderComp{
        .viewId = 0,
        .materialId = 1,
        .vertexCount = vertexCount_,
        .firstVertex = firstVertex_,
        .visible = true,
        .overrideClearColor = true,
        .clearColor = { 0.01F, 0.01F, 0.01F, 1.0F } });
}

void TestScene::onUnload(World& world)
{
    clearWorld(world);
}

void TestScene::onUpdate(World& world, const SimulationFrameInput& input)
{
    (void)world;
    (void)input;
}
