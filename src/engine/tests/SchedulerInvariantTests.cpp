#include <ecs/SystemScheduler.h>
#include <ecs/FrameViews.h>
#include <ecs/World.h>

#include <app/ecs/components/PositionComp.h>

#include <cassert>

int main()
{
    World world{};
    const Entity e = world.createEntity();
    world.emplaceComponent<PositionComp>(e);

    SystemScheduler scheduler{};
    scheduler.setDebugAccessValidation(true);
    scheduler.addPhaseDependency(SystemScheduler::Phase::PreSim, SystemScheduler::Phase::Sim);
    scheduler.addPhaseDependency(SystemScheduler::Phase::Sim, SystemScheduler::Phase::PostSim);

    AccessDeclaration bad{};
    if (auto pos = world.componentTypeId<PositionComp>(); pos.has_value()) {
        bad.write.insert(*pos);
    }

    scheduler.addWrite(SystemScheduler::Phase::Sim, bad,
        [](WorldWriteView& view, const SimulationFrameInput&) {
            view.query<const PositionComp>().each([](Entity, const PositionComp&) {});
        });

    bool threw = false;
    try {
        scheduler.run(SystemScheduler::Phase::Sim, world, SimulationFrameInput{});
    }
    catch (...) {
        threw = true;
    }
    assert(threw);

    return 0;
}
