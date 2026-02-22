#pragma once

#include <ecs/ComponentResidency.h>

struct SkinningComp {
    float blendWeight{ 0.0F };
};

template <>
struct ComponentResidencyTrait<SkinningComp> {
    static constexpr ComponentResidency value = ComponentResidency::HotArchetype;
};
