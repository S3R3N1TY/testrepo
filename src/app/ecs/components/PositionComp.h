#pragma once

#include <ecs/ComponentResidency.h>

struct PositionComp {
    float x{ 0.0F };
    float y{ 0.0F };
    float z{ 0.0F };
};

template <>
struct ComponentResidencyTrait<PositionComp> {
    static constexpr ComponentResidency value = ComponentResidency::HotArchetype;
};
