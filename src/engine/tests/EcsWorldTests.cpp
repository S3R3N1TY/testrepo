#include <ecs/StructuralCommandBuffer.h>
#include <ecs/World.h>

#include <app/ecs/components/DebugTagComp.h>
#include <app/ecs/components/PositionComp.h>
#include <app/ecs/components/RotationComp.h>
#include <app/ecs/components/ScaleComp.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <future>
#include <ranges>
#include <vector>

int main()
{
    World world{};
    StructuralCommandBuffer scb{};
    world.setStructuralCommandBuffer(&scb);

    const Entity e = world.createEntity();
    world.emplaceComponent<PositionComp>(e, PositionComp{ .x = 1.0F, .y = 2.0F, .z = 3.0F });
    world.emplaceComponent<RotationComp>(e, RotationComp{ .angleRadians = 0.3F, .angularVelocityRadiansPerSecond = 1.2F });
    world.emplaceComponent<DebugTagComp>(e, DebugTagComp{ .tag = 7 });

    const Entity noRot = world.createEntity();
    world.emplaceComponent<PositionComp>(noRot, PositionComp{ .x = -1.0F, .y = 0.0F, .z = 0.0F });

    assert(world.hasComponent<PositionComp>(e));
    assert(world.hasComponent<RotationComp>(e));
    assert(world.hasComponent<DebugTagComp>(e));

    auto rotId = world.componentTypeId<RotationComp>();
    assert(rotId.has_value());
    const uint64_t v0 = world.componentVersion(*rotId);
    world.emplaceComponent<RotationComp>(e, RotationComp{ .angleRadians = 2.0F, .angularVelocityRadiansPerSecond = 0.5F });
    const uint64_t v1 = world.componentVersion(*rotId);
    assert(v1 > v0);

    // Explicit access canonicalization: mixed declaration order should preserve per-type access.
    const auto plan = world.queryBuilder()
                          .include<RotationComp>(ComponentAccess::ReadWrite)
                          .include<PositionComp>(ComponentAccess::ReadOnly)
                          .plan();
    auto posId = world.componentTypeId<PositionComp>();
    assert(posId.has_value());
    bool sawPosRO = false;
    bool sawRotRW = false;
    for (const auto& remaps : plan.remapsByArchetype) {
        for (const auto& remap : remaps) {
            if (remap.componentType == *posId && remap.access == ComponentAccess::ReadOnly) {
                sawPosRO = true;
            }
            if (remap.componentType == *rotId && remap.access == ComponentAccess::ReadWrite) {
                sawRotRW = true;
            }
        }
    }
    assert(sawPosRO);
    assert(sawRotRW);

    // Optional payload semantics must be deterministic and non-null/null as expected.
    size_t optionalHits = 0;
    size_t optionalMisses = 0;
    world.query<const PositionComp, Optional<const RotationComp>>().each(
        [&](Entity entity, const PositionComp&, const RotationComp* rot) {
            if (entity.id == e.id) {
                assert(rot != nullptr);
                optionalHits += 1;
            }
            if (entity.id == noRot.id) {
                assert(rot == nullptr);
                optionalMisses += 1;
            }
        });
    assert(optionalHits == 1);
    assert(optionalMisses == 1);

    size_t excludeCount = 0;
    world.query<const PositionComp>().exclude<RotationComp>().each([&](Entity entity, const PositionComp&) {
        if (entity.id == noRot.id) {
            excludeCount += 1;
        }
        assert(entity.id != e.id);
    });
    assert(excludeCount == 1);

    // Optional eachChunk support: optional streams are nullable for missing archetypes.
    bool sawChunkWithOptionalNull = false;
    bool sawChunkWithOptionalData = false;
    world.query<const PositionComp, Optional<const RotationComp>>().eachChunk(
        [&](const Entity* entities, const PositionComp* pos, const RotationComp* rot, size_t count) {
            (void)pos;
            if (count == 0) {
                return;
            }
            if (rot == nullptr) {
                sawChunkWithOptionalNull = true;
            }
            else {
                sawChunkWithOptionalData = true;
            }
            (void)entities;
        });
    assert(sawChunkWithOptionalNull);
    assert(sawChunkWithOptionalData);

    // Query-plan thread safety and correctness under contention.
    constexpr size_t kExpectedPositionCount = 2;
    std::vector<std::future<void>> futures{};
    for (int i = 0; i < 16; ++i) {
        futures.push_back(std::async(std::launch::async, [&world]() {
            for (int j = 0; j < 128; ++j) {
                size_t local = 0;
                world.query<const PositionComp>().each([&](Entity, const PositionComp&) { local += 1; });
                assert(local == kExpectedPositionCount);
            }
        }));
    }
    for (auto& f : futures) {
        f.get();
    }

    // Archetype migration invariants under churn.
    for (uint32_t i = 0; i < 256; ++i) {
        const Entity temp = world.createEntity();
        world.emplaceComponent<PositionComp>(temp);
        world.emplaceComponent<RotationComp>(temp);
        world.removeComponent<RotationComp>(temp);
        if ((i % 3) == 0) {
            world.emplaceComponent<ScaleComp>(temp);
            world.removeComponent<ScaleComp>(temp);
        }
    }

    std::vector<uint32_t> firstPass{};
    std::vector<uint32_t> secondPass{};
    world.query<const PositionComp>().each([&](Entity entity, const PositionComp&) { firstPass.push_back(entity.id); });
    world.query<const PositionComp>().each([&](Entity entity, const PositionComp&) { secondPass.push_back(entity.id); });
    assert(firstPass == secondPass);

    // Change tracking should update when mutable query actually mutates data.
    const uint64_t posBefore = world.componentVersion(*posId);
    world.query<PositionComp>().each([&](Entity, WriteRef<PositionComp> p) {
        (void)p.get().x;
    });
    const uint64_t posUnchanged = world.componentVersion(*posId);
    assert(posUnchanged == posBefore);

    world.query<PositionComp>().each([&](Entity, WriteRef<PositionComp> p) {
        p.touch();
        p.get().x += 1.0F;
    });
    const uint64_t posAfter = world.componentVersion(*posId);
    assert(posAfter > posBefore);

    world.beginReadPhase();
    bool threw = false;
    try {
        world.removeComponent<DebugTagComp>(e);
    }
    catch (...) {
        threw = true;
    }
    world.endReadPhase();
    assert(threw);

    scb.removeComponent<DebugTagComp>(e);
    world.playbackPhase(StructuralPlaybackPhase::PostSim);
    assert(!world.hasComponent<DebugTagComp>(e));

    const Entity rollbackE = world.createEntity();
    world.emplaceComponent<RotationComp>(rollbackE, RotationComp{ .angleRadians = 1.0F, .angularVelocityRadiansPerSecond = 2.0F });
    scb.setComponent<RotationComp>(rollbackE, RotationComp{ .angleRadians = 9.0F, .angularVelocityRadiansPerSecond = 9.0F });
    scb.setFailureInjection(FailureInjectionConfig{ .failAfterNApply = 1 });
    bool rollbackTriggered = false;
    try {
        world.playbackPhase(StructuralPlaybackPhase::PostSim);
    }
    catch (...) {
        rollbackTriggered = true;
    }
    assert(rollbackTriggered);
    scb.setFailureInjection(FailureInjectionConfig{});
    scb.setComponent<RotationComp>(rollbackE, RotationComp{ .angleRadians = 9.0F, .angularVelocityRadiansPerSecond = 9.0F });
    world.playbackPhase(StructuralPlaybackPhase::PostSim);
    const RotationComp* updated = world.getComponent<RotationComp>(rollbackE);
    assert(updated != nullptr);
    assert(updated->angleRadians == 9.0F);

    scb.removeComponent<RotationComp>(rollbackE);
    world.playbackPhase(StructuralPlaybackPhase::PostSim);
    assert(!world.hasComponent<RotationComp>(rollbackE));

    scb.destroyEntity(e);
    world.endFrame();
    assert(!world.isAlive(e));
    return 0;
}
