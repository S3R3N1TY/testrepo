#pragma once

#include <Engine.h>
#include <ecs/World.h>

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

class SystemScheduler {
public:
    enum class Phase : uint8_t {
        PreSim = 0,
        Sim,
        PostSim,
        RenderExtract,
        Count
    };

    using UpdateFn = std::function<void(World&, const SimulationFrameInput&)>;

    void add(Phase phase, UpdateFn fn);
    void run(Phase phase, World& world, const SimulationFrameInput& input) const;

private:
    std::array<std::vector<UpdateFn>, static_cast<size_t>(Phase::Count)> phases_{};
};
