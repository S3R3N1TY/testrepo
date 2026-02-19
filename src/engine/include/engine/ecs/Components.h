#pragma once

#include <array>
#include <cstdint>

namespace ecs {

struct Transform {
    std::array<float, 3> position{ 0.0F, 0.0F, 0.0F };
    std::array<float, 3> rotationEulerRadians{ 0.0F, 0.0F, 0.0F };
    std::array<float, 3> scale{ 1.0F, 1.0F, 1.0F };
};

struct LocalToWorld {
    std::array<float, 16> matrix{
        1.0F, 0.0F, 0.0F, 0.0F,
        0.0F, 1.0F, 0.0F, 0.0F,
        0.0F, 0.0F, 1.0F, 0.0F,
        0.0F, 0.0F, 0.0F, 1.0F
    };
};

struct TransformDirty {
    bool value{ true };
};

struct TransformPrevious {
    Transform value{};
};

struct TransformHierarchyParent {
    uint32_t parentIndex{ 0 };
    uint32_t parentGeneration{ 0 };
    bool hasParent{ false };
};

struct LinearVelocity {
    std::array<float, 3> unitsPerSecond{ 0.0F, 0.0F, 0.0F };
};

struct AngularVelocity {
    std::array<float, 3> radiansPerSecond{ 0.0F, 0.0F, 1.0F };
};

struct MeshRef {
    uint32_t viewId{ 0 };
    uint32_t materialId{ 1 };
    uint32_t vertexCount{ 3 };
    uint32_t firstVertex{ 0 };
};

struct RenderVisibility {
    bool visible{ true };
};

struct RenderLayer {
    uint32_t value{ 0 };
};

struct Lifetime {
    float secondsRemaining{ -1.0F };
};

} // namespace ecs
