#pragma once

#include "Scene.h"

class SpinningTriangleScene final : public Scene {
public:
    const char* name() const override { return "Spinning Triangle"; }
    void onLoad(World& world) override;
    void onUnload(World& world) override;
    void onUpdate(World& world, const SimulationFrameInput& input) override;
};
