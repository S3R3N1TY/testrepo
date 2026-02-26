#pragma once

#include <Engine.h>
#include <ecs/World.h>

class Scene {
public:
    virtual ~Scene() = default;

    virtual const char* name() const = 0;
    virtual void onLoad(World& world) = 0;
    virtual void onUnload(World& world)
    {
        (void)world;
    }
    virtual void onUpdate(World& world, const SimulationFrameInput& input) = 0;
    virtual void onDraw(World& world) const
    {
        (void)world;
    }
};
