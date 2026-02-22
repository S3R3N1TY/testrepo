#pragma once

#include <array>

#include <ecs/ComponentResidency.h>

struct WorldBoundsComp {
    std::array<float, 3> center{ 0.0F, 0.0F, 0.0F };
    float radius{ 1.0F };
};

template <>
struct ComponentResidencyTrait<WorldBoundsComp> {
    static constexpr ComponentResidency value = ComponentResidency::HotArchetype;
};
