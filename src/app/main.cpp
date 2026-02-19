#include <engine/Engine.h>
#include <engine/ecs/Components.h>

#include <cmath>
#include <cstddef>
#include <utility>

namespace {

constexpr float kTwoPi = 6.283185307F;

class SpinningTriangleGame final : public IGameSimulation {
public:
    void configureWorld(ecs::World& world) override
    {
        const ecs::Entity triangle = world.create();
        (void)world.add<ecs::Transform>(triangle, ecs::Transform{});
        (void)world.add<ecs::LinearVelocity>(triangle, ecs::LinearVelocity{});
        (void)world.add<ecs::AngularVelocity>(triangle, ecs::AngularVelocity{ .radiansPerSecond = { 0.0F, 0.0F, 1.0F } });
        (void)world.add<ecs::MeshRef>(triangle, ecs::MeshRef{});
        (void)world.add<ecs::RenderVisibility>(triangle, ecs::RenderVisibility{ .visible = true });
        (void)world.add<ecs::RenderLayer>(triangle, ecs::RenderLayer{ .value = 0 });
        (void)world.add<ecs::Lifetime>(triangle, ecs::Lifetime{ .secondsRemaining = -1.0F });
    }

    void registerSystems(ecs::SystemScheduler& scheduler) override
    {
        ecs::SystemScheduler::SystemDesc lifetime{};
        lifetime.name = "lifetime.update";
        lifetime.phase = ecs::SystemPhase::PreSimulation;
        lifetime.reads = {};
        lifetime.writes = { ecs::SystemScheduler::typeOf<ecs::Lifetime>(), ecs::SystemScheduler::typeOf<ecs::RenderVisibility>() };
        lifetime.run = [](ecs::World& world, const ecs::SystemFrameContext& context) {
            world.view<ecs::Lifetime, ecs::RenderVisibility>().each(
                [&](ecs::Entity, ecs::Lifetime& life, ecs::RenderVisibility& visibility) {
                    if (life.secondsRemaining < 0.0F) {
                        return;
                    }
                    life.secondsRemaining -= context.deltaSeconds;
                    if (life.secondsRemaining <= 0.0F) {
                        visibility.visible = false;
                    }
                });
        };
        scheduler.addSystem(std::move(lifetime));

        ecs::SystemScheduler::SystemDesc translate{};
        translate.name = "transform.translate";
        translate.phase = ecs::SystemPhase::Simulation;
        translate.reads = { ecs::SystemScheduler::typeOf<ecs::LinearVelocity>() };
        translate.writes = { ecs::SystemScheduler::typeOf<ecs::Transform>() };
        translate.run = [](ecs::World& world, const ecs::SystemFrameContext& context) {
            world.view<ecs::Transform, ecs::LinearVelocity>().each(
                [&](ecs::Entity, ecs::Transform& transform, const ecs::LinearVelocity& velocity) {
                    for (size_t axis = 0; axis < 3; ++axis) {
                        transform.position[axis] += velocity.unitsPerSecond[axis] * context.deltaSeconds;
                    }
                });
        };
        scheduler.addSystem(std::move(translate));

        ecs::SystemScheduler::SystemDesc rotate{};
        rotate.name = "transform.rotate";
        rotate.phase = ecs::SystemPhase::Simulation;
        rotate.reads = { ecs::SystemScheduler::typeOf<ecs::AngularVelocity>() };
        rotate.writes = { ecs::SystemScheduler::typeOf<ecs::Transform>() };
        rotate.run = [](ecs::World& world, const ecs::SystemFrameContext& context) {
            world.view<ecs::Transform, ecs::AngularVelocity>().each(
                [&](ecs::Entity, ecs::Transform& transform, const ecs::AngularVelocity& velocity) {
                    for (size_t axis = 0; axis < 3; ++axis) {
                        transform.rotationEulerRadians[axis] += velocity.radiansPerSecond[axis] * context.deltaSeconds;
                    }
                    transform.rotationEulerRadians[2] = std::fmod(transform.rotationEulerRadians[2], kTwoPi);
                });
        };
        scheduler.addSystem(std::move(rotate));
    }
};

} // namespace

int main()
{
    SpinningTriangleGame game;
    Engine engine;
    engine.run(game);
}
