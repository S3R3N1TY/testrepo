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
#include "ecs/components/PhysicsComp.h"
#include "ecs/components/RenderBoundsComp.h"
#include "ecs/components/WorldBoundsComp.h"
#include "ecs/components/CameraComp.h"
#include "ecs/components/LightComp.h"

#include <ecs/FrameViews.h>

#include <algorithm>
#include <cmath>

Simulation::Simulation()
{
    world_.setStructuralCommandBuffer(&structuralCommands_);
    scheduler_.setDebugAccessValidation(true);
    scheduler_.addPhaseDependency(SystemScheduler::Phase::PreSim, SystemScheduler::Phase::Sim);
    scheduler_.addPhaseDependency(SystemScheduler::Phase::Sim, SystemScheduler::Phase::PostSim);
    scheduler_.addPhaseDependency(SystemScheduler::Phase::PostSim, SystemScheduler::Phase::RenderExtract);
    createInitialScene();
    configureScheduler();
}

void Simulation::configureScheduler()
{
    AccessDeclaration physicsAccess{};
    if (auto pos = world_.componentTypeId<PositionComp>(); pos.has_value()) {
        physicsAccess.write.insert(*pos);
    }
    if (auto phys = world_.componentTypeId<PhysicsComp>(); phys.has_value()) {
        physicsAccess.read.insert(*phys);
    }
    scheduler_.addWrite(SystemScheduler::Phase::Sim, std::move(physicsAccess),
        [](WorldWriteView& view, const SimulationFrameInput& input) {
            view.query<PositionComp, const PhysicsComp>().each([&](Entity, WriteRef<PositionComp> pos, const PhysicsComp& phys) {
                pos.touch();
                pos.get().x += phys.vx * input.deltaSeconds;
                pos.get().y += phys.vy * input.deltaSeconds;
                pos.get().z += phys.vz * input.deltaSeconds;
            });
        });

    AccessDeclaration spinAccess{};
    if (auto rot = world_.componentTypeId<RotationComp>(); rot.has_value()) {
        spinAccess.write.insert(*rot);
    }

    scheduler_.addWrite(SystemScheduler::Phase::Sim, std::move(spinAccess),
        [](WorldWriteView& view, const SimulationFrameInput& input) {
            constexpr float kTwoPi = 6.283185307F;
            view.query<RotationComp>().each([&](Entity, WriteRef<RotationComp> rotation) {
                rotation.touch();
                rotation.get().angleRadians += input.deltaSeconds * rotation.get().angularVelocityRadiansPerSecond;
                rotation.get().angleRadians = std::fmod(rotation.get().angleRadians, kTwoPi);
                if (rotation.get().angleRadians < 0.0F) {
                    rotation.get().angleRadians += kTwoPi;
                }
            });
        });



    AccessDeclaration animationAccess{};
    if (auto anim = world_.componentTypeId<AnimationComp>(); anim.has_value()) {
        animationAccess.write.insert(*anim);
    }
    scheduler_.addWrite(SystemScheduler::Phase::Sim, std::move(animationAccess),
        [](WorldWriteView& view, const SimulationFrameInput& input) {
            view.query<AnimationComp>().each([&](Entity, WriteRef<AnimationComp> anim) {
                anim.touch();
                anim.get().timeSeconds += input.deltaSeconds * anim.get().speed;
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
            view.query<const AnimationComp, SkinningComp>().each([&](Entity, const AnimationComp& anim, WriteRef<SkinningComp> skin) {
                skin.touch();
                skin.get().blendWeight = 0.5F + 0.5F * std::sin(anim.timeSeconds);
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
                [&](Entity, const PositionComp& pos, const RotationComp& rot, const ScaleComp& scale, WriteRef<LocalToWorldComp> l2w) {
                    const float c = std::cos(rot.angleRadians);
                    const float s = std::sin(rot.angleRadians);
                    l2w.touch();
                    l2w.get().m = {
                        c * scale.x, -s * scale.y, 0.0F, 0.0F,
                        s * scale.x,  c * scale.y, 0.0F, 0.0F,
                        0.0F,         0.0F,        scale.z, 0.0F,
                        pos.x,        pos.y,       pos.z,   1.0F
                    };
                });
        });

    AccessDeclaration worldBoundsAccess{};
    if (auto l2w = world_.componentTypeId<LocalToWorldComp>(); l2w.has_value()) {
        worldBoundsAccess.read.insert(*l2w);
    }
    if (auto rb = world_.componentTypeId<RenderBoundsComp>(); rb.has_value()) {
        worldBoundsAccess.read.insert(*rb);
    }
    if (auto wb = world_.componentTypeId<WorldBoundsComp>(); wb.has_value()) {
        worldBoundsAccess.write.insert(*wb);
    }
    scheduler_.addWrite(SystemScheduler::Phase::PostSim, std::move(worldBoundsAccess),
        [](WorldWriteView& view, const SimulationFrameInput&) {
            view.query<const LocalToWorldComp, const RenderBoundsComp, WorldBoundsComp>().each(
                [](Entity, const LocalToWorldComp& l2w, const RenderBoundsComp& rb, WriteRef<WorldBoundsComp> wb) {
                    wb.touch();
                    wb.get().center = { l2w.m[12], l2w.m[13], l2w.m[14] };
                    wb.get().radius = rb.localSphereRadius;
                });
        });

    AccessDeclaration cameraAccess{};
    if (auto pos = world_.componentTypeId<PositionComp>(); pos.has_value()) {
        cameraAccess.read.insert(*pos);
    }
    if (auto camera = world_.componentTypeId<CameraComp>(); camera.has_value()) {
        cameraAccess.write.insert(*camera);
    }
    scheduler_.addWrite(SystemScheduler::Phase::PostSim, std::move(cameraAccess),
        [](WorldWriteView& view, const SimulationFrameInput&) {
            view.query<const PositionComp, CameraComp>().each([](Entity, const PositionComp& pos, WriteRef<CameraComp> cam) {
                cam.touch();
                cam.get().position = { pos.x, pos.y, pos.z + 10.0F };
            });
        });

    AccessDeclaration lightAccess{};
    if (auto pos = world_.componentTypeId<PositionComp>(); pos.has_value()) {
        lightAccess.read.insert(*pos);
    }
    if (auto light = world_.componentTypeId<LightComp>(); light.has_value()) {
        lightAccess.write.insert(*light);
    }
    scheduler_.addWrite(SystemScheduler::Phase::PostSim, std::move(lightAccess),
        [](WorldWriteView& view, const SimulationFrameInput&) {
            view.query<const PositionComp, LightComp>().each([](Entity, const PositionComp& pos, WriteRef<LightComp> light) {
                light.touch();
                light.get().worldPosition = { pos.x, pos.y, pos.z };
            });
        });


    AccessDeclaration cullAccess{};
    if (auto wb = world_.componentTypeId<WorldBoundsComp>(); wb.has_value()) {
        cullAccess.read.insert(*wb);
    }
    if (auto cam = world_.componentTypeId<CameraComp>(); cam.has_value()) {
        cullAccess.read.insert(*cam);
    }
    if (auto vis = world_.componentTypeId<VisibilityComp>(); vis.has_value()) {
        cullAccess.write.insert(*vis);
    }

    scheduler_.addWrite(SystemScheduler::Phase::PostSim, std::move(cullAccess),
        [](WorldWriteView& view, const SimulationFrameInput&) {
            CameraComp camera{};
            bool hasCamera = false;
            view.query<const CameraComp>().each([&](Entity, const CameraComp& cam) {
                if (!hasCamera) {
                    camera = cam;
                    hasCamera = true;
                }
            });
            if (!hasCamera) {
                return;
            }

            view.query<const WorldBoundsComp, VisibilityComp>().eachChunk(
                [&](const Entity* entities, const WorldBoundsComp* bounds, VisibilityComp* visRaw, size_t count) {
                    auto dot3 = [](const std::array<float,3>& a, const std::array<float,3>& b) {
                        return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
                    };
                    auto sub3 = [](const std::array<float,3>& a, const std::array<float,3>& b) {
                        return std::array<float,3>{ a[0]-b[0], a[1]-b[1], a[2]-b[2] };
                    };
                    auto normalize3 = [](const std::array<float,3>& v) {
                        const float len = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
                        const float inv = (len > 1e-6F) ? (1.0F / len) : 1.0F;
                        return std::array<float,3>{ v[0]*inv, v[1]*inv, v[2]*inv };
                    };
                    auto add3 = [](const std::array<float,3>& a, const std::array<float,3>& b) {
                        return std::array<float,3>{ a[0]+b[0], a[1]+b[1], a[2]+b[2] };
                    };
                    auto scale3 = [](const std::array<float,3>& v, float s) {
                        return std::array<float,3>{ v[0]*s, v[1]*s, v[2]*s };
                    };

                    const std::array<float,3> F = normalize3(camera.forward);
                    const std::array<float,3> R = normalize3(camera.right);
                    const std::array<float,3> U = normalize3(camera.up);
                    const float tanHalf = std::tan(camera.verticalFovRadians * 0.5F);
                    const float tanHalfX = tanHalf * camera.aspectRatio;

                    const auto leftN = normalize3(add3(scale3(R, 1.0F), scale3(F, tanHalfX)));
                    const auto rightN = normalize3(add3(scale3(R, -1.0F), scale3(F, tanHalfX)));
                    const auto bottomN = normalize3(add3(scale3(U, 1.0F), scale3(F, tanHalf)));
                    const auto topN = normalize3(add3(scale3(U, -1.0F), scale3(F, tanHalf)));

                    auto sphereInFrustum = [&](const WorldBoundsComp& b) {
                        const std::array<float,3> center = b.center;
                        const std::array<float,3> rel = sub3(center, camera.position);
                        const float depth = dot3(rel, F);
                        if (depth + b.radius < camera.zNear || depth - b.radius > camera.zFar) {
                            return false;
                        }
                        if (dot3(rel, leftN) < -b.radius) return false;
                        if (dot3(rel, rightN) < -b.radius) return false;
                        if (dot3(rel, topN) < -b.radius) return false;
                        if (dot3(rel, bottomN) < -b.radius) return false;
                        return true;
                    };

                    bool coarseVisible = false;
                    for (size_t i = 0; i < count; ++i) {
                        if (sphereInFrustum(bounds[i])) {
                            coarseVisible = true;
                            break;
                        }
                    }
                    for (size_t i = 0; i < count; ++i) {
                        const bool visible = coarseVisible && sphereInFrustum(bounds[i]);
                        visRaw[i].visible = visible;
                        view.markModified<VisibilityComp>(entities[i]);
                    }
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
    world_.emplaceComponent<PhysicsComp>(triangle, PhysicsComp{ .vx = 0.0F, .vy = 0.0F, .vz = 0.0F });
    world_.emplaceComponent<RenderBoundsComp>(triangle, RenderBoundsComp{ .localSphereRadius = 1.0F });
    world_.emplaceComponent<WorldBoundsComp>(triangle);
    world_.emplaceComponent<LightComp>(triangle);
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

    const Entity cameraEntity = world_.createEntity();
    world_.emplaceComponent<PositionComp>(cameraEntity, PositionComp{ .x = 0.0F, .y = 0.0F, .z = 10.0F });
    world_.emplaceComponent<CameraComp>(cameraEntity);
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
