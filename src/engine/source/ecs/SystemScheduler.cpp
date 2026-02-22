#include <ecs/SystemScheduler.h>

#include <ecs/FrameViews.h>
#include <ecs/StructuralPhase.h>

#include <future>
#include <stdexcept>

void SystemScheduler::addRead(Phase phase, AccessDeclaration access, ReadUpdateFn fn)
{
    phases_[static_cast<size_t>(phase)].push_back(ScheduledSystem{
        .access = std::move(access),
        .readFn = std::move(fn),
        .writeFn = {},
        .writes = false
    });
}

void SystemScheduler::addWrite(Phase phase, AccessDeclaration access, WriteUpdateFn fn)
{
    phases_[static_cast<size_t>(phase)].push_back(ScheduledSystem{
        .access = std::move(access),
        .readFn = {},
        .writeFn = std::move(fn),
        .writes = true
    });
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
                futures.push_back(std::async(std::launch::async, [&world, &input, &list, idx]() {
                    WorldReadView readView{ world, &list[idx].access };
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
                    WorldWriteView writeView{ world, &list[idx].access };
                    list[idx].writeFn(writeView, input);
                }
                else {
                    world.beginReadPhase();
                    WorldReadView readView{ world, &list[idx].access };
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
