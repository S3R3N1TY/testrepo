#pragma once

#include <ecs/ComponentResidency.h>

struct AnimationComp {
    float timeSeconds{ 0.0F };
    float speed{ 1.0F };
};

template <>
struct ComponentResidencyTrait<AnimationComp> {
    static constexpr ComponentResidency value = ComponentResidency::HotArchetype;
};
