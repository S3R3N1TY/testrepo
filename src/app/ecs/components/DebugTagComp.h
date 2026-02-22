#pragma once

#include <cstdint>

#include <ecs/ComponentResidency.h>

struct DebugTagComp {
    uint32_t tag{ 0 };
};

template <>
struct ComponentResidencyTrait<DebugTagComp> {
    static constexpr ComponentResidency value = ComponentResidency::ColdSparse;
};
