#include <ecs/StructuralCommandBuffer.h>
#include <ecs/World.h>

#include <app/ecs/components/DebugTagComp.h>
#include <app/ecs/components/PositionComp.h>
#include <app/ecs/components/RotationComp.h>

#include <cassert>
#include <cstdint>

int main()
{
    World world{};
    StructuralCommandBuffer scb{};
    world.setStructuralCommandBuffer(&scb);

    const Entity e = world.createEntity();
    world.emplaceComponent<PositionComp>(e, PositionComp{ .x = 1.0F, .y = 2.0F, .z = 3.0F });
    world.emplaceComponent<RotationComp>(e, RotationComp{ .angleRadians = 0.3F, .angularVelocityRadiansPerSecond = 1.2F });
    world.emplaceComponent<DebugTagComp>(e, DebugTagComp{ .tag = 7 });

    assert(world.hasComponent<PositionComp>(e));
    assert(world.hasComponent<RotationComp>(e));
    assert(world.hasComponent<DebugTagComp>(e));

    auto rotId = world.componentTypeId<RotationComp>();
    assert(rotId.has_value());
    const uint64_t v0 = world.componentVersion(*rotId);
    world.emplaceComponent<RotationComp>(e, RotationComp{ .angleRadians = 2.0F, .angularVelocityRadiansPerSecond = 0.5F });
    const uint64_t v1 = world.componentVersion(*rotId);
    assert(v1 > v0);

    // query exclude/optional ergonomic API coverage
    size_t countWithExclude = 0;
    world.query<const RotationComp>().exclude<PositionComp>().each([&](Entity, const RotationComp&) {
        countWithExclude += 1;
    });
    assert(countWithExclude == 0);

    size_t countWithOptional = 0;
    world.query<const PositionComp>().optional<DebugTagComp>().each([&](Entity, const PositionComp&) {
        countWithOptional += 1;
    });
    assert(countWithOptional == 1);

    // plan cache invalidation under churn
    for (uint32_t i = 0; i < 128; ++i) {
        const Entity temp = world.createEntity();
        world.emplaceComponent<PositionComp>(temp);
        world.destroyEntity(temp);
    }

    world.beginReadPhase();
    bool threw = false;
    try {
        world.removeComponent<DebugTagComp>(e);
    } catch (...) {
        threw = true;
    }
    world.endReadPhase();
    assert(threw);

    scb.removeComponent<DebugTagComp>(e);
    world.playbackPhase(StructuralPlaybackPhase::PostSim);
    assert(!world.hasComponent<DebugTagComp>(e));

    scb.destroyEntity(e);
    world.endFrame();
    assert(!world.isAlive(e));
    return 0;
}
