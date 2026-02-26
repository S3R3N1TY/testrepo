#pragma once

#include "Scene.h"

class SpinningSquareScene final : public Scene {
public:
    const char* name() const override { return "Spinning Square"; }
    void onLoad(World& world) override;
    void onUnload(World& world) override;
    void onUpdate(World& world, const SimulationFrameInput& input) override;
};
