#pragma once

#include <engine/Engine.h>
#include <engine/ecs/World.h>

#include "ecs/systems/RenderExtractSys.h"
#include "ecs/systems/SpinningSys.h"

class Simulation final : public IGameSimulation {
public:
    Simulation();

    void tick(const SimulationFrameInput& input) override;
    [[nodiscard]] FrameGraphInput buildFrameGraphInput() const override;

private:
    void createInitialScene();

    World world_{};
    SpinningSys spinningSys_{};
    RenderExtractSys renderExtractSys_{};

    mutable FrameGraphInput cachedFrameGraphInput_{};
    mutable bool frameGraphDirty_{ true };
};
