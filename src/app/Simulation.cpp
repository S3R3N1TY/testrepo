#include "Simulation.h"

#include "ecs/components/PositionComp.h"
#include "ecs/components/RenderComp.h"
#include "ecs/components/RotationComp.h"
#include "ecs/components/ScaleComp.h"
#include "ecs/components/VisibilityComp.h"
#include "ecs/components/DebugTagComp.h"
#include "ecs/components/LocalToWorldComp.h"
#include "ecs/components/AnimationComp.h"
#include "ecs/components/SkinningComp.h"

#include <ecs/FrameViews.h>

#include <cmath>

Simulation::Simulation()
{
    world_.setStructuralCommandBuffer(&structuralCommands_);
    createInitialScene();
    configureScheduler();
}

void Simulation::configureScheduler()
{
    AccessDeclaration spinAccess{};
    if (auto rot = world_.componentTypeId<RotationComp>(); rot.has_value()) {
        spinAccess.write.insert(*rot);
    }

    scheduler_.addWrite(SystemScheduler::Phase::Sim, std::move(spinAccess),
        [](WorldWriteView& view, const SimulationFrameInput& input) {
            constexpr float kTwoPi = 6.283185307F;
            view.query<RotationComp>().each([&](Entity, RotationComp& rotation) {
                rotation.angleRadians += input.deltaSeconds * rotation.angularVelocityRadiansPerSecond;
                rotation.angleRadians = std::fmod(rotation.angleRadians, kTwoPi);
                if (rotation.angleRadians < 0.0F) {
                    rotation.angleRadians += kTwoPi;
                }
            });
        });



    AccessDeclaration animationAccess{};
    if (auto anim = world_.componentTypeId<AnimationComp>(); anim.has_value()) {
        animationAccess.write.insert(*anim);
    }
    scheduler_.addWrite(SystemScheduler::Phase::Sim, std::move(animationAccess),
        [](WorldWriteView& view, const SimulationFrameInput& input) {
            view.query<AnimationComp>().each([&](Entity, AnimationComp& anim) {
                anim.timeSeconds += input.deltaSeconds * anim.speed;
            });
        });

    AccessDeclaration skinningAccess{};
    if (auto anim = world_.componentTypeId<AnimationComp>(); anim.has_value()) {
        skinningAccess.read.insert(*anim);
    }
    if (auto skin = world_.componentTypeId<SkinningComp>(); skin.has_value()) {
        skinningAccess.write.insert(*skin);
    }
    scheduler_.addWrite(SystemScheduler::Phase::PostSim, std::move(skinningAccess),
        [](WorldWriteView& view, const SimulationFrameInput&) {
            view.query<const AnimationComp, SkinningComp>().each([&](Entity, const AnimationComp& anim, SkinningComp& skin) {
                skin.blendWeight = 0.5F + 0.5F * std::sin(anim.timeSeconds);
            });
        });

    AccessDeclaration transformAccess{};
    if (auto pos = world_.componentTypeId<PositionComp>(); pos.has_value()) {
        transformAccess.read.insert(*pos);
    }
    if (auto rot = world_.componentTypeId<RotationComp>(); rot.has_value()) {
        transformAccess.read.insert(*rot);
    }
    if (auto scale = world_.componentTypeId<ScaleComp>(); scale.has_value()) {
        transformAccess.read.insert(*scale);
    }
    if (auto l2w = world_.componentTypeId<LocalToWorldComp>(); l2w.has_value()) {
        transformAccess.write.insert(*l2w);
    }

    scheduler_.addWrite(SystemScheduler::Phase::PostSim, std::move(transformAccess),
        [](WorldWriteView& view, const SimulationFrameInput&) {
            view.query<const PositionComp, const RotationComp, const ScaleComp, LocalToWorldComp>().each(
                [&](Entity, const PositionComp& pos, const RotationComp& rot, const ScaleComp& scale, LocalToWorldComp& l2w) {
                    const float c = std::cos(rot.angleRadians);
                    const float s = std::sin(rot.angleRadians);
                    l2w.m = {
                        c * scale.x, -s * scale.y, 0.0F, 0.0F,
                        s * scale.x,  c * scale.y, 0.0F, 0.0F,
                        0.0F,         0.0F,        scale.z, 0.0F,
                        pos.x,        pos.y,       pos.z,   1.0F
                    };
                });
        });


    AccessDeclaration cullAccess{};
    if (auto l2w = world_.componentTypeId<LocalToWorldComp>(); l2w.has_value()) {
        cullAccess.read.insert(*l2w);
    }
    if (auto vis = world_.componentTypeId<VisibilityComp>(); vis.has_value()) {
        cullAccess.write.insert(*vis);
    }

    scheduler_.addWrite(SystemScheduler::Phase::PostSim, std::move(cullAccess),
        [](WorldWriteView& view, const SimulationFrameInput&) {
            view.query<const LocalToWorldComp, VisibilityComp>().each(
                [&](Entity, const LocalToWorldComp& l2w, VisibilityComp& vis) {
                    const float z = l2w.m[14];
                    vis.visible = (z > -500.0F && z < 500.0F);
                });
        });

    AccessDeclaration renderExtractAccess{};
    if (auto render = world_.componentTypeId<RenderComp>(); render.has_value()) {
        renderExtractAccess.read.insert(*render);
    }
    if (auto rot = world_.componentTypeId<RotationComp>(); rot.has_value()) {
        renderExtractAccess.read.insert(*rot);
    }
    if (auto vis = world_.componentTypeId<VisibilityComp>(); vis.has_value()) {
        renderExtractAccess.read.insert(*vis);
    }
    if (auto l2w = world_.componentTypeId<LocalToWorldComp>(); l2w.has_value()) {
        renderExtractAccess.read.insert(*l2w);
    }

    scheduler_.addRead(SystemScheduler::Phase::RenderExtract, std::move(renderExtractAccess),
        [this](const WorldReadView&, const SimulationFrameInput&) {
            cachedFrameGraphInput_ = renderExtractSys_.build(world_);
        });
}

void Simulation::createInitialScene()
{
    const Entity triangle = world_.createEntity();
    world_.emplaceComponent<PositionComp>(triangle);
    world_.emplaceComponent<ScaleComp>(triangle);
    world_.emplaceComponent<LocalToWorldComp>(triangle);
    world_.emplaceComponent<AnimationComp>(triangle);
    world_.emplaceComponent<SkinningComp>(triangle);
    world_.emplaceComponent<RotationComp>(triangle, RotationComp{
        .angleRadians = 0.0F,
        .angularVelocityRadiansPerSecond = 1.0F });
    world_.emplaceComponent<VisibilityComp>(triangle, VisibilityComp{ .visible = true });
    world_.emplaceComponent<DebugTagComp>(triangle, DebugTagComp{ .tag = 42 });
    world_.emplaceComponent<RenderComp>(triangle, RenderComp{
        .viewId = 0,
        .materialId = 1,
        .vertexCount = 3,
        .firstVertex = 0,
        .visible = true,
        .overrideClearColor = true,
        .clearColor = { 0.02F, 0.02F, 0.08F, 1.0F } });
}

void Simulation::tick(const SimulationFrameInput& input)
{
    world_.beginFrame();
    scheduler_.run(SystemScheduler::Phase::PreSim, world_, input);
    scheduler_.run(SystemScheduler::Phase::Sim, world_, input);
    scheduler_.run(SystemScheduler::Phase::PostSim, world_, input);
    scheduler_.run(SystemScheduler::Phase::RenderExtract, world_, input);
    world_.endFrame();
}

FrameGraphInput Simulation::buildFrameGraphInput() const
{
    return cachedFrameGraphInput_;
}
