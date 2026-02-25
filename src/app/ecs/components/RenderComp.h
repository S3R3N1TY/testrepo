#pragma once

#include <array>
#include <cstdint>

#include <ecs/ComponentResidency.h>

struct RenderComp {
    uint32_t viewId{ 0 };
    uint32_t materialId{ 1 };
    uint32_t meshId{ 1 };
    uint32_t vertexCount{ 3 };
    uint32_t firstVertex{ 0 };
    bool visible{ true };

    bool overrideClearColor{ false };
    std::array<float, 4> clearColor{ 0.02F, 0.02F, 0.08F, 1.0F };
};

template <>
struct ComponentResidencyTrait<RenderComp> {
    static constexpr ComponentResidency value = ComponentResidency::HotArchetype;
};
