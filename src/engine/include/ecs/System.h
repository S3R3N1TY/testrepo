#pragma once

#include <Engine.h>

class World;

class ISystem {
public:
    virtual ~ISystem() = default;
    virtual void update(World& world, const SimulationFrameInput& input) = 0;
};
