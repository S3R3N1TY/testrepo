#include "Simulation.h"

#include "ecs/components/PositionComp.h"
#include "ecs/components/RenderComp.h"
#include "ecs/components/RotationComp.h"
#include "ecs/components/ScaleComp.h"

Simulation::Simulation()
{
    createInitialScene();
}

void Simulation::createInitialScene()
{
    const Entity triangle = world_.createEntity();
    world_.emplaceComponent<PositionComp>(triangle);
    world_.emplaceComponent<ScaleComp>(triangle);
    world_.emplaceComponent<RotationComp>(triangle, RotationComp{
        .angleRadians = 0.0F,
        .angularVelocityRadiansPerSecond = 1.0F });
    world_.emplaceComponent<RenderComp>(triangle, RenderComp{
        .viewId = 0,
        .materialId = 1,
        .vertexCount = 3,
        .firstVertex = 0,
        .visible = true,
        .overrideClearColor = true,
        .clearColor = { 0.02F, 0.02F, 0.08F, 1.0F } });
}

void Simulation::tick(const SimulationFrameInput& input)
{
    spinningSys_.update(world_, input);
    frameGraphDirty_ = true;
}

FrameGraphInput Simulation::buildFrameGraphInput() const
{
    if (frameGraphDirty_) {
        cachedFrameGraphInput_ = renderExtractSys_.build(world_);
        frameGraphDirty_ = false;
    }
    return cachedFrameGraphInput_;
}
