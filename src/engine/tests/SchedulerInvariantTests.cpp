#include <ecs/FrameViews.h>
#include <ecs/SystemScheduler.h>
#include <ecs/World.h>

#include <app/ecs/components/LocalToWorldComp.h>
#include <app/ecs/components/PositionComp.h>
#include <app/ecs/components/VisibilityComp.h>

#include <cassert>
#include <vector>

int main()
{
    // Debug invariant: declared write with const query mismatch.
    {
        World world{};
        const Entity e = world.createEntity();
        world.emplaceComponent<PositionComp>(e);

        SystemScheduler scheduler{};
        scheduler.setDebugAccessValidation(true);

        AccessDeclaration bad{};
        if (auto pos = world.componentTypeId<PositionComp>(); pos.has_value()) {
            bad.write.insert(*pos);
        }

        scheduler.addWrite(SystemScheduler::Phase::Sim, bad,
            [](WorldWriteView& view, const SimulationFrameInput&) {
                view.query<const PositionComp>().each([](Entity, const PositionComp&) {});
            });
        scheduler.finalizeConfiguration();

        bool threw = false;
        try {
            scheduler.run(SystemScheduler::Phase::Sim, world, SimulationFrameInput{});
        }
        catch (...) {
            threw = true;
        }
        assert(threw);
    }

    // Debug invariant: undeclared write must fail.
    {
        World world{};
        const Entity e = world.createEntity();
        world.emplaceComponent<PositionComp>(e);

        SystemScheduler scheduler{};
        scheduler.setDebugAccessValidation(true);

        AccessDeclaration bad{};
        scheduler.addWrite(SystemScheduler::Phase::Sim, bad,
            [](WorldWriteView& view, const SimulationFrameInput&) {
                view.query<PositionComp>().each([](Entity, WriteRef<PositionComp> pos) {
                    pos.touch();
                });
            });
        scheduler.finalizeConfiguration();

        bool threw = false;
        try {
            scheduler.run(SystemScheduler::Phase::Sim, world, SimulationFrameInput{});
        }
        catch (...) {
            threw = true;
        }
        assert(threw);
    }

    // Configure-time phase cycle rejection.
    {
        SystemScheduler scheduler{};
        scheduler.addPhaseDependency(SystemScheduler::Phase::PreSim, SystemScheduler::Phase::Sim);
        scheduler.addPhaseDependency(SystemScheduler::Phase::Sim, SystemScheduler::Phase::PostSim);
        bool threw = false;
        try {
            scheduler.addPhaseDependency(SystemScheduler::Phase::PostSim, SystemScheduler::Phase::PreSim);
        }
        catch (...) {
            threw = true;
        }
        assert(threw);
    }

    // Phase ordering smoke: Sim -> PostSim -> RenderExtract effects.
    {
        World world{};
        const Entity e = world.createEntity();
        world.emplaceComponent<PositionComp>(e, PositionComp{ .x = 0.0F, .y = 0.0F, .z = 0.0F });
        world.emplaceComponent<LocalToWorldComp>(e);
        world.emplaceComponent<VisibilityComp>(e, VisibilityComp{ .visible = false });

        auto posId = world.componentTypeId<PositionComp>();
        auto l2wId = world.componentTypeId<LocalToWorldComp>();
        auto visId = world.componentTypeId<VisibilityComp>();
        assert(posId.has_value() && l2wId.has_value() && visId.has_value());

        SystemScheduler scheduler{};
        scheduler.setDebugAccessValidation(true);
        scheduler.addPhaseDependency(SystemScheduler::Phase::Sim, SystemScheduler::Phase::PostSim);
        scheduler.addPhaseDependency(SystemScheduler::Phase::PostSim, SystemScheduler::Phase::RenderExtract);

        std::vector<int> marks{};

        AccessDeclaration sim{};
        sim.write.insert(*posId);
        scheduler.addWrite(SystemScheduler::Phase::Sim, sim,
            [&marks](WorldWriteView& view, const SimulationFrameInput&) {
                marks.push_back(1);
                view.query<PositionComp>().each([](Entity, WriteRef<PositionComp> pos) {
                    pos.touch();
                    pos.get().x = 42.0F;
                });
            });

        AccessDeclaration post{};
        post.read.insert(*posId);
        post.write.insert(*l2wId);
        post.write.insert(*visId);
        scheduler.addWrite(SystemScheduler::Phase::PostSim, post,
            [&marks](WorldWriteView& view, const SimulationFrameInput&) {
                marks.push_back(2);
                view.query<const PositionComp, LocalToWorldComp, VisibilityComp>().each(
                    [](Entity, const PositionComp& pos, WriteRef<LocalToWorldComp> l2w, WriteRef<VisibilityComp> vis) {
                        l2w.touch();
                        vis.touch();
                        l2w.get().m[12] = pos.x;
                        vis.get().visible = pos.x > 0.0F;
                    });
            });

        AccessDeclaration extract{};
        extract.read.insert(*visId);
        scheduler.addRead(SystemScheduler::Phase::RenderExtract, extract,
            [&marks](const WorldReadView& view, const SimulationFrameInput&) {
                marks.push_back(3);
                bool sawVisible = false;
                view.query<const VisibilityComp>().each([&](Entity, const VisibilityComp& vis) {
                    sawVisible = sawVisible || vis.visible;
                });
                assert(sawVisible);
            });

        scheduler.finalizeConfiguration();
        scheduler.run(SystemScheduler::Phase::Sim, world, SimulationFrameInput{});
        scheduler.run(SystemScheduler::Phase::PostSim, world, SimulationFrameInput{});
        scheduler.run(SystemScheduler::Phase::RenderExtract, world, SimulationFrameInput{});
        assert((marks == std::vector<int>{ 1, 2, 3 }));
    }

    return 0;
}
