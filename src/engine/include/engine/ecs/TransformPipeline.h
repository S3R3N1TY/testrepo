#pragma once

#include <engine/ecs/Components.h>
#include <engine/ecs/SystemScheduler.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <unordered_map>

namespace ecs::transform {

[[nodiscard]] inline uint64_t entityKey(Entity entity) noexcept
{
    return (static_cast<uint64_t>(entity.generation) << 32U) | static_cast<uint64_t>(entity.index);
}

[[nodiscard]] inline std::array<float, 16> multiplyMat4(const std::array<float, 16>& a, const std::array<float, 16>& b)
{
    std::array<float, 16> out{};
    for (uint32_t col = 0; col < 4; ++col) {
        for (uint32_t row = 0; row < 4; ++row) {
            float v = 0.0F;
            for (uint32_t k = 0; k < 4; ++k) {
                v += a[(k * 4U) + row] * b[(col * 4U) + k];
            }
            out[(col * 4U) + row] = v;
        }
    }
    return out;
}

[[nodiscard]] inline std::array<float, 16> composeLocalMatrix(const Transform& transform)
{
    const float rx = transform.rotationEulerRadians[0];
    const float ry = transform.rotationEulerRadians[1];
    const float rz = transform.rotationEulerRadians[2];

    const float sx = std::sin(rx);
    const float cx = std::cos(rx);
    const float sy = std::sin(ry);
    const float cy = std::cos(ry);
    const float sz = std::sin(rz);
    const float cz = std::cos(rz);

    const float r00 = cz * cy;
    const float r01 = cz * sy * sx - sz * cx;
    const float r02 = cz * sy * cx + sz * sx;

    const float r10 = sz * cy;
    const float r11 = sz * sy * sx + cz * cx;
    const float r12 = sz * sy * cx - cz * sx;

    const float r20 = -sy;
    const float r21 = cy * sx;
    const float r22 = cy * cx;

    return {
        r00 * transform.scale[0], r10 * transform.scale[0], r20 * transform.scale[0], 0.0F,
        r01 * transform.scale[1], r11 * transform.scale[1], r21 * transform.scale[1], 0.0F,
        r02 * transform.scale[2], r12 * transform.scale[2], r22 * transform.scale[2], 0.0F,
        transform.position[0], transform.position[1], transform.position[2], 1.0F
    };
}

inline void registerTransformSystems(SystemScheduler& scheduler)
{
    scheduler.addSystem<TypeList<Transform>, TypeList<LocalToWorld, TransformDirty, TransformPrevious, TransformHierarchyParent>>(
        "transform.ensure_components",
        SystemPhase::PreSimulation,
        StructuralWrites::Yes,
        [](auto& world, const SystemFrameContext&) {
            world.template view<const Transform>().each([&](Entity entity, const Transform&) {
                if (!world.template has<LocalToWorld>(entity)) {
                    world.queueAdd(entity, LocalToWorld{});
                }
                if (!world.template has<TransformDirty>(entity)) {
                    world.queueAdd(entity, TransformDirty{ .value = true });
                }
                if (!world.template has<TransformPrevious>(entity)) {
                    const Transform* t = world.template get<Transform>(entity);
                    if (t != nullptr) {
                        world.queueAdd(entity, TransformPrevious{ .value = *t });
                    }
                }
                if (!world.template has<TransformHierarchyParent>(entity)) {
                    world.queueAdd(entity, TransformHierarchyParent{});
                }
            });
        });

    scheduler.addSystem<TypeList<Transform, TransformPrevious>, TypeList<TransformDirty, TransformPrevious>>(
        "transform.detect_mutations",
        SystemPhase::PreSimulation,
        StructuralWrites::No,
        [](auto& world, const SystemFrameContext&) {
            world.template view<const Transform, TransformPrevious, TransformDirty>().each(
                [&](Entity, const Transform& transform, TransformPrevious& previous, TransformDirty& dirty) {
                    if (previous.value.position != transform.position
                        || previous.value.rotationEulerRadians != transform.rotationEulerRadians
                        || previous.value.scale != transform.scale) {
                        dirty.value = true;
                        previous.value = transform;
                    }
                });
        });

    scheduler.addSystem<TypeList<Transform, TransformHierarchyParent, TransformDirty>, TypeList<LocalToWorld, TransformDirty, TransformHierarchyParent>>(
        "transform.compose_hierarchy",
        SystemPhase::PostSimulation,
        StructuralWrites::No,
        [](auto& world, const SystemFrameContext&) {
            std::unordered_map<uint64_t, uint8_t> state{};
            state.reserve(1024);

            std::function<bool(Entity)> compose = [&](Entity entity) -> bool {
                const uint64_t key = entityKey(entity);
                const auto it = state.find(key);
                if (it != state.end()) {
                    if (it->second == 2U) {
                        return false;
                    }
                    if (it->second == 1U) {
                        return false;
                    }
                }
                state[key] = 1U;

                const Transform* transform = world.template get<Transform>(entity);
                TransformHierarchyParent* parent = world.template getMutable<TransformHierarchyParent>(entity);
                TransformDirty* dirty = world.template getMutable<TransformDirty>(entity);
                LocalToWorld* localToWorld = world.template getMutable<LocalToWorld>(entity);
                if (transform == nullptr || parent == nullptr || dirty == nullptr || localToWorld == nullptr) {
                    state[key] = 2U;
                    return false;
                }

                const std::array<float, 16> localMatrix = composeLocalMatrix(*transform);
                bool parentChanged = false;
                if (parent->hasParent) {
                    const Entity parentEntity{ .index = parent->parentIndex, .generation = parent->parentGeneration };
                    if (!world.isAlive(parentEntity)
                        || !world.template has<Transform>(parentEntity)
                        || !world.template has<LocalToWorld>(parentEntity)) {
                        parent->hasParent = false;
                    } else {
                        parentChanged = compose(parentEntity);
                        const LocalToWorld* parentL2W = world.template get<LocalToWorld>(parentEntity);
                        if (parentL2W != nullptr) {
                            localToWorld->matrix = multiplyMat4(parentL2W->matrix, localMatrix);
                        }
                    }
                }

                bool changed = false;
                if (!parent->hasParent) {
                    localToWorld->matrix = localMatrix;
                }
                if (dirty->value || parentChanged) {
                    changed = true;
                }
                dirty->value = false;

                state[key] = 2U;
                return changed;
            };

            world.template view<const Transform, TransformHierarchyParent, TransformDirty, LocalToWorld>().each(
                [&](Entity entity, const Transform&, TransformHierarchyParent&, TransformDirty&, LocalToWorld&) {
                    (void)compose(entity);
                });
        });
}

} // namespace ecs::transform
