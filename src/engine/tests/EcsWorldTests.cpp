#include <ecs/StructuralCommandBuffer.h>
#include <ecs/World.h>

#include <app/ecs/components/DebugTagComp.h>
#include <app/ecs/components/PositionComp.h>
#include <app/ecs/components/RotationComp.h>
#include <app/ecs/components/ScaleComp.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
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

    const auto posId = world.componentTypeId<PositionComp>();
    const auto rotId = world.componentTypeId<RotationComp>();
    assert(posId.has_value());
    assert(rotId.has_value());

    // Explicit deferred dependency correctness in one mixed transaction.
    const auto deferred = scb.createEntity();
    scb.emplaceComponent<PositionComp>(deferred, PositionComp{ .x = 5.0F, .y = 6.0F, .z = 7.0F });
    scb.setComponent<PositionComp>(deferred, PositionComp{ .x = 9.0F, .y = 9.0F, .z = 9.0F });
    scb.removeComponent<PositionComp>(deferred);
    scb.destroyEntity(deferred);
    world.playbackPhase(StructuralPlaybackPhase::PostSim);
    world.playbackPhase(StructuralPlaybackPhase::EndFrame);

    // Deferred token cannot be reused after epoch cleanup.
    bool staleDeferredRejected = false;
    try {
        scb.destroyEntity(deferred);
    }
    catch (...) {
        staleDeferredRejected = true;
    }
    assert(staleDeferredRejected);

    // Mid-batch failure rollback must restore full pre-state.
    const Entity rollbackE = world.createEntity();
    world.emplaceComponent<PositionComp>(rollbackE, PositionComp{ .x = 10.0F, .y = 20.0F, .z = 30.0F });
    world.emplaceComponent<RotationComp>(rollbackE, RotationComp{ .angleRadians = 1.0F, .angularVelocityRadiansPerSecond = 2.0F });
    const uint32_t generationBefore = rollbackE.generation;
    const auto snapBefore = world.snapshotEntity(rollbackE);
    assert(snapBefore.has_value());

    scb.setComponent<RotationComp>(rollbackE, RotationComp{ .angleRadians = 9.0F, .angularVelocityRadiansPerSecond = 9.0F });
    scb.removeComponent<PositionComp>(rollbackE);
    scb.destroyEntity(rollbackE);
    scb.setFailureInjection(FailureInjectionConfig{ .failAfterNApply = 2 });

    bool rollbackTriggered = false;
    try {
        world.playbackPhase(StructuralPlaybackPhase::PostSim);
        world.playbackPhase(StructuralPlaybackPhase::EndFrame);
    }
    catch (...) {
        rollbackTriggered = true;
    }
    assert(rollbackTriggered);

    assert(world.isAlive(rollbackE));
    assert(rollbackE.generation == generationBefore);
    const auto* restoredPos = world.getComponent<PositionComp>(rollbackE);
    const auto* restoredRot = world.getComponent<RotationComp>(rollbackE);
    assert(restoredPos != nullptr);
    assert(restoredPos->x == 10.0F);
    assert(restoredRot != nullptr);
    assert(restoredRot->angleRadians == 1.0F);

    scb.setFailureInjection(FailureInjectionConfig{});

    // Mutable query with no touch does not bump versions.
    world.beginSystemWriteScope();
    const uint64_t posVersion0 = world.componentVersion(*posId);
    world.query<PositionComp>().each([](Entity, WriteRef<PositionComp> pos) {
        (void)pos.get().x;
    });
    world.endSystemWriteScope();
    assert(world.componentVersion(*posId) == posVersion0);

    // Batched dirty flush: touching multiple rows in same chunk/column only bumps once.
    std::vector<Entity> dense{};
    dense.reserve(130);
    for (int i = 0; i < 130; ++i) {
        Entity ent = world.createEntity();
        world.emplaceComponent<PositionComp>(ent, PositionComp{ .x = static_cast<float>(i), .y = 0.0F, .z = 0.0F });
        dense.push_back(ent);
    }

    auto plan = world.queryBuilder().include<PositionComp>().plan();
    std::vector<World::ChunkHandle> chunkHandles{};
    world.forEachChunk<>(plan, [&](World::ChunkView view) {
        chunkHandles.push_back(view.handle);
    });
    assert(chunkHandles.size() >= 2);

    const uint64_t chunk0Before = world.chunkVersion(chunkHandles[0], *posId);
    const uint64_t chunk1Before = world.chunkVersion(chunkHandles[1], *posId);

    world.beginSystemWriteScope();
    int touched = 0;
    world.query<PositionComp>().each([&](Entity ent, WriteRef<PositionComp> pos) {
        if (ent.id == dense[0].id || ent.id == dense[1].id) {
            pos.touch();
            pos.get().x += 1.0F;
            touched += 1;
        }
    });
    world.endSystemWriteScope();
    assert(touched == 2);

    const uint64_t chunk0After = world.chunkVersion(chunkHandles[0], *posId);
    const uint64_t chunk1After = world.chunkVersion(chunkHandles[1], *posId);
    assert(chunk0After == chunk0Before + 1);
    assert(chunk1After == chunk1Before);

    // Optional mutable mutation path marks dirty via OptionalWriteRef.
    const uint64_t rotVersion0 = world.componentVersion(*rotId);
    world.beginSystemWriteScope();
    bool optionalMutated = false;
    world.query<Optional<RotationComp>>().each([&](Entity ent, OptionalWriteRef<RotationComp> rot) {
        if (ent.id == e.id) {
            auto* write = static_cast<WriteRef<RotationComp>*>(rot);
            assert(write != nullptr);
            write->touch();
            write->get().angleRadians += 0.25F;
            optionalMutated = true;
        }
    });
    world.endSystemWriteScope();
    assert(optionalMutated);
    assert(world.componentVersion(*rotId) == rotVersion0 + 1);

    return 0;
}
