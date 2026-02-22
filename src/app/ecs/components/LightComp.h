#pragma once

#include <array>

#include <ecs/ComponentResidency.h>

struct LightComp {
    std::array<float, 3> worldPosition{ 0.0F, 0.0F, 0.0F };
    float intensity{ 1.0F };
};

template <>
struct ComponentResidencyTrait<LightComp> {
    static constexpr ComponentResidency value = ComponentResidency::HotArchetype;
};
