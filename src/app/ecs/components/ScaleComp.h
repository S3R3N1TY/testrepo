#pragma once

#include <ecs/ComponentResidency.h>

struct ScaleComp {
    float x{ 1.0F };
    float y{ 1.0F };
    float z{ 1.0F };
};

template <>
struct ComponentResidencyTrait<ScaleComp> {
    static constexpr ComponentResidency value = ComponentResidency::HotArchetype;
};
