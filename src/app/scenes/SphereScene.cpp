#include "SphereScene.h"

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

void SphereScene::onLoad(World& world)
{
    clearWorld(world);

    const Entity sphere = world.createEntity();
    world.emplaceComponent<PositionComp>(sphere);
    world.emplaceComponent<ScaleComp>(sphere);
    world.emplaceComponent<RotationComp>(sphere, RotationComp{
        .angleRadians = 0.0F,
        .angularVelocityRadiansPerSecond = 0.8F });
    world.emplaceComponent<RenderComp>(sphere, RenderComp{
        .viewId = 0,
        .materialId = 3,
        .vertexCount = vertexCount_,
        .firstVertex = firstVertex_,
        .visible = true,
        .overrideClearColor = true,
        .clearColor = { 0.01F, 0.01F, 0.01F, 1.0F } });
}

void SphereScene::onUnload(World& world)
{
    clearWorld(world);
}

void SphereScene::onUpdate(World& world, const SimulationFrameInput& input)
{
    (void)world;
    (void)input;
}
