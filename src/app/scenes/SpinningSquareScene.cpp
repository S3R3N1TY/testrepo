#include "SpinningSquareScene.h"

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

void SpinningSquareScene::onLoad(World& world)
{
    clearWorld(world);

    const Entity square = world.createEntity();
    world.emplaceComponent<PositionComp>(square);
    world.emplaceComponent<ScaleComp>(square);
    world.emplaceComponent<RotationComp>(square, RotationComp{
        .angleRadians = 0.0F,
        .angularVelocityRadiansPerSecond = 1.2F });
    world.emplaceComponent<RenderComp>(square, RenderComp{
        .viewId = 0,
        .materialId = 2,
        .vertexCount = 6,
        .firstVertex = 3,
        .visible = true,
        .overrideClearColor = true,
        .clearColor = { 0.08F, 0.02F, 0.02F, 1.0F } });
}

void SpinningSquareScene::onUnload(World& world)
{
    clearWorld(world);
}

void SpinningSquareScene::onUpdate(World& world, const SimulationFrameInput& input)
{
    (void)world;
    (void)input;
}
