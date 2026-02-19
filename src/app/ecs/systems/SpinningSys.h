#pragma once

#include <Engine.h>
#include <ecs/World.h>

class SpinningSys final {
public:
    void update(World& world, const SimulationFrameInput& input) const;
};
