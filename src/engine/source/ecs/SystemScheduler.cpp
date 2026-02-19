#include <ecs/SystemScheduler.h>

void SystemScheduler::add(Phase phase, UpdateFn fn)
{
    phases_[static_cast<size_t>(phase)].push_back(std::move(fn));
}

void SystemScheduler::run(Phase phase, World& world, const SimulationFrameInput& input) const
{
    const auto& list = phases_[static_cast<size_t>(phase)];
    for (const UpdateFn& fn : list) {
        fn(world, input);
    }
}
