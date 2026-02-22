#pragma once

#include <ecs/ComponentResidency.h>

struct VisibilityComp {
    bool visible{ true };
};

template <>
struct ComponentResidencyTrait<VisibilityComp> {
    static constexpr ComponentResidency value = ComponentResidency::HotArchetype;
};
