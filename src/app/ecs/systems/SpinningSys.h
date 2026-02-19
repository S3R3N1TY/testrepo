#pragma once

#include <engine/Engine.h>
#include <engine/ecs/World.h>

class SpinningSys final {
public:
    void update(World& world, const SimulationFrameInput& input) const;
};
