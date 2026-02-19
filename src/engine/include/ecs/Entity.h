#pragma once

#include <cstddef>
#include <cstdint>

struct Entity {
    uint32_t id{ 0 };
    uint32_t generation{ 0 };

    friend bool operator==(Entity lhs, Entity rhs) = default;
};

struct EntityHash {
    [[nodiscard]] size_t operator()(const Entity& e) const noexcept
    {
        return (static_cast<size_t>(e.id) << 32U) ^ static_cast<size_t>(e.generation);
    }
};
