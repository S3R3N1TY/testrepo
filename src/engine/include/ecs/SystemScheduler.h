#pragma once

#include <Engine.h>
#include <ecs/AccessDeclaration.h>
#include <ecs/World.h>

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

class WorldReadView;
class WorldWriteView;

class SystemScheduler {
public:
    enum class Phase : uint8_t {
        PreSim = 0,
        Sim,
        PostSim,
        RenderExtract,
        Count
    };

    using ReadUpdateFn = std::function<void(const WorldReadView&, const SimulationFrameInput&)>;
    using WriteUpdateFn = std::function<void(WorldWriteView&, const SimulationFrameInput&)>;

    void addRead(Phase phase, AccessDeclaration access, ReadUpdateFn fn);
    void addWrite(Phase phase, AccessDeclaration access, WriteUpdateFn fn);
    void addPhaseDependency(Phase before, Phase after);
    void setDebugAccessValidation(bool enabled) { debugAccessValidation_ = enabled; }

    void finalizeConfiguration();
    void run(Phase phase, World& world, const SimulationFrameInput& input) const;
    void validatePhaseGraph() const;

private:
    struct ScheduledSystem {
        AccessDeclaration access{};
        ReadUpdateFn readFn{};
        WriteUpdateFn writeFn{};
        bool writes{ false };
    };

    static bool hasConflict(const AccessDeclaration& lhs, const AccessDeclaration& rhs);

    std::array<std::vector<ScheduledSystem>, static_cast<size_t>(Phase::Count)> phases_{};
    std::vector<std::pair<Phase, Phase>> phaseDependencies_{};
    bool debugAccessValidation_{ false };
    bool finalized_{ false };
};
