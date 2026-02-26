#include <ecs/SystemScheduler.h>

#include <ecs/FrameViews.h>
#include <ecs/StructuralPhase.h>

#include <future>
#include <queue>
#include <stdexcept>

void SystemScheduler::addRead(Phase phase, AccessDeclaration access, ReadUpdateFn fn)
{
    if (finalized_) {
        throw std::runtime_error("SystemScheduler configuration is finalized");
    }
    ScheduledSystem system{};
    system.access = std::move(access);
    system.readFn = std::move(fn);
    system.writes = false;
    phases_[static_cast<size_t>(phase)].emplace_back(std::move(system));
}

void SystemScheduler::addWrite(Phase phase, AccessDeclaration access, WriteUpdateFn fn)
{
    if (finalized_) {
        throw std::runtime_error("SystemScheduler configuration is finalized");
    }
    ScheduledSystem system{};
    system.access = std::move(access);
    system.writeFn = std::move(fn);
    system.writes = true;
    phases_[static_cast<size_t>(phase)].emplace_back(std::move(system));
}

void SystemScheduler::addPhaseDependency(Phase before, Phase after)
{
    if (finalized_) {
        throw std::runtime_error("SystemScheduler configuration is finalized");
    }
    phaseDependencies_.push_back({ before, after });
    validatePhaseGraph();
}

void SystemScheduler::finalizeConfiguration()
{
    validatePhaseGraph();
    finalized_ = true;
}

void SystemScheduler::validatePhaseGraph() const
{
    constexpr size_t kCount = static_cast<size_t>(Phase::Count);
    std::array<uint32_t, kCount> indegree{};
    std::array<std::vector<size_t>, kCount> out{};
    for (const auto& [before, after] : phaseDependencies_) {
        const size_t b = static_cast<size_t>(before);
        const size_t a = static_cast<size_t>(after);
        out[b].push_back(a);
        indegree[a] += 1;
    }

    std::queue<size_t> q{};
    for (size_t i = 0; i < kCount; ++i) {
        if (indegree[i] == 0) q.push(i);
    }
    size_t seen = 0;
    while (!q.empty()) {
        const size_t n = q.front();
        q.pop();
        seen += 1;
        for (size_t child : out[n]) {
            indegree[child] -= 1;
            if (indegree[child] == 0) q.push(child);
        }
    }
    if (seen != kCount) {
        throw std::runtime_error("SystemScheduler phase dependency cycle detected");
    }
}

bool SystemScheduler::hasConflict(const AccessDeclaration& lhs, const AccessDeclaration& rhs)
{
    for (const uint32_t t : lhs.write) {
        if (rhs.write.contains(t) || rhs.read.contains(t)) {
            return true;
        }
    }
    for (const uint32_t t : lhs.read) {
        if (rhs.write.contains(t)) {
            return true;
        }
    }
    return false;
}

void SystemScheduler::run(Phase phase, World& world, const SimulationFrameInput& input) const
{
#ifndef NDEBUG
    if (!finalized_) {
        throw std::runtime_error("SystemScheduler::run called before finalizeConfiguration");
    }
#endif
    const auto& list = phases_[static_cast<size_t>(phase)];
    if (list.empty()) {
        if (phase == Phase::PostSim) {
            world.playbackPhase(StructuralPlaybackPhase::PostSim);
        }
        return;
    }

    // Build dependency DAG and execute in conflict-free levels.
    const size_t n = list.size();
    std::vector<uint32_t> indegree(n, 0);
    std::vector<std::vector<size_t>> children(n);

    for (size_t i = 0; i < n; ++i) {
        for (size_t j = i + 1; j < n; ++j) {
            if (hasConflict(list[i].access, list[j].access)) {
                children[i].push_back(j);
                indegree[j] += 1;
            }
        }
    }

    std::vector<size_t> ready{};
    ready.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        if (indegree[i] == 0) {
            ready.push_back(i);
        }
    }

    size_t executed = 0;
    while (!ready.empty()) {
        std::vector<size_t> level = std::move(ready);
        ready.clear();

        bool allRead = true;
        for (size_t idx : level) {
            if (list[idx].writes) {
                allRead = false;
                break;
            }
        }

        if (allRead) {
            world.beginReadPhase();
            std::vector<std::future<void>> futures{};
            futures.reserve(level.size());
            for (size_t idx : level) {
                const bool debugValidation = debugAccessValidation_;
                futures.push_back(std::async(std::launch::async, [&world, &input, &list, idx, debugValidation]() {
                    WorldReadView readView{ world, debugValidation ? &list[idx].access : nullptr };
                    list[idx].readFn(readView, input);
                }));
            }
            for (auto& f : futures) {
                f.get();
            }
            world.endReadPhase();
        }
        else {
            for (size_t idx : level) {
                if (list[idx].writes) {
                    world.endReadPhase();
                    world.beginSystemWriteScope();
                    WorldWriteView writeView{ world, debugAccessValidation_ ? &list[idx].access : nullptr };
                    list[idx].writeFn(writeView, input);
                    world.endSystemWriteScope();
                }
                else {
                    world.beginReadPhase();
                    WorldReadView readView{ world, debugAccessValidation_ ? &list[idx].access : nullptr };
                    list[idx].readFn(readView, input);
                    world.endReadPhase();
                }
            }
        }

        executed += level.size();
        for (size_t node : level) {
            for (size_t child : children[node]) {
                indegree[child] -= 1;
                if (indegree[child] == 0) {
                    ready.push_back(child);
                }
            }
        }
    }

    if (executed != n) {
        throw std::runtime_error("SystemScheduler::run detected a dependency cycle in system access graph");
    }

    if (phase == Phase::PostSim) {
        world.playbackPhase(StructuralPlaybackPhase::PostSim);
    }
}
