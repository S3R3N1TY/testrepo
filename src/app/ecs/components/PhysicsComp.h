#pragma once

#include <ecs/ComponentResidency.h>

struct PhysicsComp {
    float vx{ 0.0F };
    float vy{ 0.0F };
    float vz{ 0.0F };
};

template <>
struct ComponentResidencyTrait<PhysicsComp> {
    static constexpr ComponentResidency value = ComponentResidency::HotArchetype;
};
