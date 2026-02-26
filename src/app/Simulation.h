#pragma once

#include <Engine.h>
#include <ecs/World.h>

#include "ecs/systems/RenderExtractSys.h"
#include "ecs/systems/SpinningSys.h"
#include "scenes/Scene.h"
#include "scenes/SpinningSquareScene.h"
#include "scenes/SpinningTriangleScene.h"

#include <memory>
#include <vector>

class Simulation final : public IGameSimulation {
public:
    Simulation();

    void tick(const SimulationFrameInput& input) override;
    void drawMainMenuBar() override;
    [[nodiscard]] FrameGraphInput buildFrameGraphInput() const override;

private:
    void switchToScene(size_t sceneIndex);

    World world_{};
    SpinningSys spinningSys_{};
    RenderExtractSys renderExtractSys_{};

    std::vector<std::unique_ptr<Scene>> scenes_{};
    size_t activeSceneIndex_{ 0 };
    bool hasActiveScene_{ false };

    mutable FrameGraphInput cachedFrameGraphInput_{};
    mutable bool frameGraphDirty_{ true };
};
