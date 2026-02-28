#pragma once

#include "Scene.h"

class TestScene final : public Scene {
public:
    TestScene(uint32_t firstVertex, uint32_t vertexCount)
        : firstVertex_(firstVertex)
        , vertexCount_(vertexCount)
    {
    }

    [[nodiscard]] const char* name() const override { return "Test Scene"; }
    void onLoad(World& world) override;
    void onUnload(World& world) override;
    void onUpdate(World& world, const SimulationFrameInput& input) override;

private:
    uint32_t firstVertex_{ 0 };
    uint32_t vertexCount_{ 0 };
};
