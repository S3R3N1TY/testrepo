#include <engine/Engine.h>
#include <engine/ecs/Components.h>

#include <cmath>
#include <cstddef>

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
        scheduler.addSystem<ecs::TypeList<>, ecs::TypeList<ecs::Lifetime>>("lifetime.update", ecs::SystemPhase::PreSimulation, ecs::StructuralWrites::Yes, [](auto& world, const ecs::SystemFrameContext& context) {
            world.template view<ecs::Lifetime>().each(
                [&](ecs::Entity entity, ecs::Lifetime& life) {
                    if (life.secondsRemaining < 0.0F) {
                        return;
                    }
                    life.secondsRemaining -= context.deltaSeconds;
                    if (life.secondsRemaining <= 0.0F) {
                        world.queueDestroy(entity);
                    }
                });
        });

        scheduler.addSystem<ecs::TypeList<ecs::LinearVelocity>, ecs::TypeList<ecs::Transform>>("transform.translate", ecs::SystemPhase::Simulation, ecs::StructuralWrites::No, [](auto& world, const ecs::SystemFrameContext& context) {
            world.template view<ecs::Transform, const ecs::LinearVelocity>().each(
                [&](ecs::Entity, ecs::Transform& transform, const ecs::LinearVelocity& velocity) {
                    for (size_t axis = 0; axis < 3; ++axis) {
                        transform.position[axis] += velocity.unitsPerSecond[axis] * context.deltaSeconds;
                    }
                });
        });

        scheduler.addSystem<ecs::TypeList<ecs::AngularVelocity>, ecs::TypeList<ecs::Transform>>("transform.rotate", ecs::SystemPhase::Simulation, ecs::StructuralWrites::No, [](auto& world, const ecs::SystemFrameContext& context) {
            world.template view<ecs::Transform, const ecs::AngularVelocity>().each(
                [&](ecs::Entity, ecs::Transform& transform, const ecs::AngularVelocity& velocity) {
                    for (size_t axis = 0; axis < 3; ++axis) {
                        transform.rotationEulerRadians[axis] += velocity.radiansPerSecond[axis] * context.deltaSeconds;
                    }
                    transform.rotationEulerRadians[2] = std::fmod(transform.rotationEulerRadians[2], kTwoPi);
                });
        });
    }
};

} // namespace

int main()
{
    SpinningTriangleGame game;
    Engine engine;
    engine.run(game);
}
