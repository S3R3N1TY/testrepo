#pragma once

#include <Engine.h>
#include <ecs/StructuralCommandBuffer.h>
#include <ecs/SystemScheduler.h>
#include <ecs/World.h>

#include "ecs/systems/RenderExtractSys.h"

class Simulation final : public IGameSimulation {
public:
    struct AssetBindingConfig {
        uint32_t meshId{ 1 };
        uint32_t materialId{ 1 };
    };

    Simulation();
    explicit Simulation(AssetBindingConfig assetBindings);

    void tick(const SimulationFrameInput& input) override;
    [[nodiscard]] RenderWorldSnapshot buildRenderSnapshot() const override;

private:
    void createInitialScene();
    void configureScheduler();

    AssetBindingConfig assetBindings_{};
    World world_{};
    StructuralCommandBuffer structuralCommands_{};
    RenderExtractSys renderExtractSys_{};
    SystemScheduler scheduler_{};

    mutable RenderWorldSnapshot cachedRenderSnapshot_{};
};
