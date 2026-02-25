#pragma once

#include <array>

#include <ecs/ComponentResidency.h>

struct FrustumPlane {
    std::array<float, 3> normal{ 0.0F, 0.0F, 0.0F };
    float d{ 0.0F };
};

struct CameraFrustum {
    std::array<FrustumPlane, 6> planes{};
};

struct CameraComp {
    std::array<float, 3> position{ 0.0F, 0.0F, 0.0F };
    std::array<float, 3> forward{ 0.0F, 0.0F, 1.0F };
    std::array<float, 3> right{ 1.0F, 0.0F, 0.0F };
    std::array<float, 3> up{ 0.0F, 1.0F, 0.0F };
    float verticalFovRadians{ 1.0471975512F };
    float aspectRatio{ 16.0F / 9.0F };
    float zNear{ 0.1F };
    float zFar{ 500.0F };
    CameraFrustum frustum{};
};

template <>
struct ComponentResidencyTrait<CameraComp> {
    static constexpr ComponentResidency value = ComponentResidency::HotArchetype;
};
