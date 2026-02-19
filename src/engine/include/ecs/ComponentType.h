#pragma once

#include <cstdint>

using ComponentTypeId = uint32_t;

namespace ecs::detail {
inline ComponentTypeId nextComponentTypeId() noexcept
{
    static ComponentTypeId nextId = 0;
    return nextId++;
}
} // namespace ecs::detail

template <typename T>
ComponentTypeId componentTypeId() noexcept
{
    static const ComponentTypeId id = ecs::detail::nextComponentTypeId();
    return id;
}
