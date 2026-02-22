#pragma once

#include <array>

#include <ecs/ComponentResidency.h>

struct LocalToWorldComp {
    std::array<float, 16> m{
        1,0,0,0,
        0,1,0,0,
        0,0,1,0,
        0,0,0,1
    };
};

template <>
struct ComponentResidencyTrait<LocalToWorldComp> {
    static constexpr ComponentResidency value = ComponentResidency::HotArchetype;
};
