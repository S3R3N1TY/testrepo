#pragma once

enum class ComponentResidency {
    HotArchetype,
    ColdSparse
};

template <typename T>
struct ComponentResidencyTrait {
    static constexpr ComponentResidency value = ComponentResidency::HotArchetype;
};
