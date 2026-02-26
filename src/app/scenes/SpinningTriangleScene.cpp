#include "SpinningTriangleScene.h"

#include "../ecs/components/PositionComp.h"
#include "../ecs/components/RenderComp.h"
#include "../ecs/components/RotationComp.h"
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

void SpinningTriangleScene::onLoad(World& world)
{
    clearWorld(world);

    const Entity triangle = world.createEntity();
    world.emplaceComponent<PositionComp>(triangle);
    world.emplaceComponent<ScaleComp>(triangle);
    world.emplaceComponent<RotationComp>(triangle, RotationComp{
        .angleRadians = 0.0F,
        .angularVelocityRadiansPerSecond = 1.0F });
    world.emplaceComponent<RenderComp>(triangle, RenderComp{
        .viewId = 0,
        .materialId = 1,
        .vertexCount = 3,
        .firstVertex = 0,
        .visible = true,
        .overrideClearColor = true,
        .clearColor = { 0.02F, 0.02F, 0.08F, 1.0F } });
}

void SpinningTriangleScene::onUnload(World& world)
{
    clearWorld(world);
}

void SpinningTriangleScene::onUpdate(World& world, const SimulationFrameInput& input)
{
    (void)world;
    (void)input;
}
