#include <engine/ecs/Components.h>
#include <engine/ecs/SystemScheduler.h>
#include <engine/ecs/TransformPipeline.h>
#include <engine/ecs/World.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {

uint32_t parseArg(const char* value, uint32_t fallback)
{
    if (value == nullptr) {
        return fallback;
    }
    const long parsed = std::strtol(value, nullptr, 10);
    if (parsed <= 0) {
        return fallback;
    }
    return static_cast<uint32_t>(parsed);
}

float parseFloat(const char* value, float fallback)
{
    if (value == nullptr) {
        return fallback;
    }
    const float parsed = std::strtof(value, nullptr);
    if (parsed <= 0.0F) {
        return fallback;
    }
    return parsed;
}

} // namespace

int main(int argc, char** argv)
{
    const uint32_t entities = argc > 1 ? parseArg(argv[1], 10000) : 10000;
    const uint32_t frames = argc > 2 ? parseArg(argv[2], 240) : 240;
    const float maxAllowedMs = argc > 3 ? parseFloat(argv[3], 25.0F) : 25.0F;

    ecs::World world{};
    world.reserveEntities(entities);
    world.reserve<ecs::Transform>(entities);
    world.reserve<ecs::LinearVelocity>(entities);
    world.reserve<ecs::AngularVelocity>(entities);
    world.reserve<ecs::RenderVisibility>(entities);
    world.reserve<ecs::RenderLayer>(entities);
    world.reserve<ecs::LocalToWorld>(entities);
    world.reserve<ecs::TransformDirty>(entities);
    world.reserve<ecs::TransformPrevious>(entities);
    world.reserve<ecs::TransformHierarchyParent>(entities);

    for (uint32_t i = 0; i < entities; ++i) {
        const ecs::Entity e = world.create();
        (void)world.add<ecs::Transform>(e, ecs::Transform{});
        (void)world.add<ecs::LinearVelocity>(e, ecs::LinearVelocity{ .unitsPerSecond = { 0.1F, 0.05F, 0.0F } });
        (void)world.add<ecs::AngularVelocity>(e, ecs::AngularVelocity{ .radiansPerSecond = { 0.0F, 0.0F, 0.5F } });
        (void)world.add<ecs::RenderVisibility>(e, ecs::RenderVisibility{ .visible = true });
        (void)world.add<ecs::RenderLayer>(e, ecs::RenderLayer{ .value = i % 2u });
    }

    ecs::SystemScheduler scheduler{};
    scheduler.setMaxWorkerThreads(std::max<uint32_t>(1u, std::thread::hardware_concurrency()));
    ecs::transform::registerTransformSystems(scheduler);

    scheduler.addSystem<ecs::TypeList<ecs::LinearVelocity>, ecs::TypeList<ecs::Transform>>("bench.translate", ecs::SystemPhase::Simulation, ecs::StructuralWrites::No, [](auto& w, const ecs::SystemFrameContext& ctx) {
        w.template view<ecs::Transform, const ecs::LinearVelocity>().each([&](ecs::Entity, ecs::Transform& t, const ecs::LinearVelocity& v) {
            t.position[0] += v.unitsPerSecond[0] * ctx.deltaSeconds;
            t.position[1] += v.unitsPerSecond[1] * ctx.deltaSeconds;
        });
    });

    scheduler.addSystem<ecs::TypeList<ecs::AngularVelocity>, ecs::TypeList<ecs::Transform>>("bench.rotate", ecs::SystemPhase::Simulation, ecs::StructuralWrites::No, [](auto& w, const ecs::SystemFrameContext& ctx) {
        w.template view<ecs::Transform, const ecs::AngularVelocity>().each([&](ecs::Entity, ecs::Transform& t, const ecs::AngularVelocity& v) {
            t.rotationEulerRadians[2] += v.radiansPerSecond[2] * ctx.deltaSeconds;
        });
    });

    const auto begin = std::chrono::steady_clock::now();
    for (uint32_t frame = 0; frame < frames; ++frame) {
        scheduler.execute(world, ecs::SystemFrameContext{ .deltaSeconds = 1.0F / 60.0F, .frameIndex = frame });
    }
    const auto end = std::chrono::steady_clock::now();

    const double totalMs = std::chrono::duration<double, std::milli>(end - begin).count();
    const double avgMs = totalMs / static_cast<double>(frames);

    std::cout << "ecs_benchmark entities=" << entities
              << " frames=" << frames
              << " avg_ms=" << avgMs
              << " threshold_ms=" << maxAllowedMs
              << std::endl;

    if (avgMs > static_cast<double>(maxAllowedMs)) {
        std::cerr << "ecs_benchmark failed: average frame time above threshold" << std::endl;
        return 2;
    }

    return 0;
}
