#pragma once

#include <Engine.h>
#include <ecs/StructuralCommandBuffer.h>
#include <ecs/SystemScheduler.h>
#include <ecs/World.h>

#include "ecs/systems/RenderExtractSys.h"

class Simulation final : public IGameSimulation {
public:
    Simulation();

    void tick(const SimulationFrameInput& input) override;
    [[nodiscard]] FrameGraphInput buildFrameGraphInput() const override;

private:
    void createInitialScene();
    void configureScheduler();

    World world_{};
    StructuralCommandBuffer structuralCommands_{};
    RenderExtractSys renderExtractSys_{};
    SystemScheduler scheduler_{};

    mutable FrameGraphInput cachedFrameGraphInput_{};
};
