#pragma once

#include <ecs/ComponentResidency.h>

struct RenderBoundsComp {
    float localSphereRadius{ 1.0F };
};

template <>
struct ComponentResidencyTrait<RenderBoundsComp> {
    static constexpr ComponentResidency value = ComponentResidency::HotArchetype;
};
