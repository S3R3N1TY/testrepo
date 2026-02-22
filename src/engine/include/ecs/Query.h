#pragma once

#include <ecs/Entity.h>

#include <array>
#include <cstddef>
#include <tuple>
#include <type_traits>
#include <vector>

class World;

template <typename... Ts>
class Query {
public:
    explicit Query(World& world)
        : world_(world)
    {
    }

    template <typename T>
    Query& exclude()
    {
        using Decayed = std::remove_cvref_t<T>;
        if (auto typeId = world_.template componentTypeId<Decayed>(); typeId.has_value()) {
            excludeTypes_.push_back(*typeId);
        }
        return *this;
    }

    template <typename T>
    Query& optional()
    {
        using Decayed = std::remove_cvref_t<T>;
        if (auto typeId = world_.template componentTypeId<Decayed>(); typeId.has_value()) {
            optionalTypes_.push_back(*typeId);
        }
        return *this;
    }

    template <typename Fn>
    void each(Fn&& fn)
    {
        const auto& plan = world_.template queryPlan<Ts...>(excludeTypes_, optionalTypes_);

        for (size_t matchIndex = 0; matchIndex < plan.matchingArchetypes.size(); ++matchIndex) {
            const uint32_t archetypeId = plan.matchingArchetypes[matchIndex];
            auto& archetype = world_.archetypes_[archetypeId];
            const auto& remaps = plan.remapsByArchetype[matchIndex];

            std::array<size_t, sizeof...(Ts)> columns{};
            std::array<size_t, sizeof...(Ts)> sizes{};
            loadLayout(remaps, columns, sizes, std::index_sequence_for<Ts...>{});

            for (uint32_t chunkIndex = 0; chunkIndex < archetype.chunks.size(); ++chunkIndex) {
                auto& chunk = archetype.chunks[chunkIndex];
                for (uint32_t row = 0; row < chunk.count; ++row) {
                    const Entity entity = chunk.entities[row];
                    invokeEach(std::forward<Fn>(fn), entity, chunk, row, columns, sizes, std::index_sequence_for<Ts...>{});
                }
            }
        }
    }

    template <typename Fn>
    void eachChunk(Fn&& fn)
    {
        const auto& plan = world_.template queryPlan<Ts...>(excludeTypes_, optionalTypes_);
        for (size_t matchIndex = 0; matchIndex < plan.matchingArchetypes.size(); ++matchIndex) {
            const uint32_t archetypeId = plan.matchingArchetypes[matchIndex];
            auto& archetype = world_.archetypes_[archetypeId];
            const auto& remaps = plan.remapsByArchetype[matchIndex];

            std::array<size_t, sizeof...(Ts)> columns{};
            std::array<size_t, sizeof...(Ts)> sizes{};
            loadLayout(remaps, columns, sizes, std::index_sequence_for<Ts...>{});

            for (uint32_t chunkIndex = 0; chunkIndex < archetype.chunks.size(); ++chunkIndex) {
                auto& chunk = archetype.chunks[chunkIndex];
                if (chunk.count == 0) {
                    continue;
                }
                invokeChunk(std::forward<Fn>(fn), chunk, columns, sizes, std::index_sequence_for<Ts...>{});
            }
        }
    }

private:
    template <size_t... Is>
    static void loadLayout(const std::vector<World::QueryPlan::ColumnRemap>& remaps,
        std::array<size_t, sizeof...(Ts)>& columns,
        std::array<size_t, sizeof...(Ts)>& sizes,
        std::index_sequence<Is...>)
    {
        ((columns[Is] = remaps[Is].columnIndex, sizes[Is] = remaps[Is].componentSize), ...);
    }

    template <typename Fn, size_t... Is>
    static void invokeEach(Fn&& fn,
        Entity entity,
        auto& chunk,
        uint32_t row,
        const std::array<size_t, sizeof...(Ts)>& columns,
        const std::array<size_t, sizeof...(Ts)>& sizes,
        std::index_sequence<Is...>)
    {
        std::forward<Fn>(fn)(entity,
            *reinterpret_cast<Ts*>(World::componentPtr(chunk, columns[Is], row, sizes[Is]))...);
    }

    template <typename Fn, size_t... Is>
    static void invokeChunk(Fn&& fn,
        auto& chunk,
        const std::array<size_t, sizeof...(Ts)>& columns,
        const std::array<size_t, sizeof...(Ts)>& sizes,
        std::index_sequence<Is...>)
    {
        std::forward<Fn>(fn)(
            chunk.entities.data(),
            reinterpret_cast<Ts*>(World::componentPtr(chunk, columns[Is], 0, sizes[Is]))...,
            chunk.count);
    }

    World& world_;
    std::vector<uint32_t> excludeTypes_{};
    std::vector<uint32_t> optionalTypes_{};
};

template <typename... Ts>
class ConstQuery {
public:
    explicit ConstQuery(const World& world)
        : world_(world)
    {
    }

    template <typename T>
    ConstQuery& exclude()
    {
        using Decayed = std::remove_cvref_t<T>;
        if (auto typeId = world_.template componentTypeId<Decayed>(); typeId.has_value()) {
            excludeTypes_.push_back(*typeId);
        }
        return *this;
    }

    template <typename T>
    ConstQuery& optional()
    {
        using Decayed = std::remove_cvref_t<T>;
        if (auto typeId = world_.template componentTypeId<Decayed>(); typeId.has_value()) {
            optionalTypes_.push_back(*typeId);
        }
        return *this;
    }

    template <typename Fn>
    void each(Fn&& fn) const
    {
        const auto& plan = world_.template queryPlan<Ts...>(excludeTypes_, optionalTypes_);

        for (size_t matchIndex = 0; matchIndex < plan.matchingArchetypes.size(); ++matchIndex) {
            const uint32_t archetypeId = plan.matchingArchetypes[matchIndex];
            const auto& archetype = world_.archetypes_[archetypeId];
            const auto& remaps = plan.remapsByArchetype[matchIndex];

            std::array<size_t, sizeof...(Ts)> columns{};
            std::array<size_t, sizeof...(Ts)> sizes{};
            loadLayout(remaps, columns, sizes, std::index_sequence_for<Ts...>{});

            for (uint32_t chunkIndex = 0; chunkIndex < archetype.chunks.size(); ++chunkIndex) {
                const auto& chunk = archetype.chunks[chunkIndex];
                for (uint32_t row = 0; row < chunk.count; ++row) {
                    const Entity entity = chunk.entities[row];
                    invokeEach(std::forward<Fn>(fn), entity, chunk, row, columns, sizes, std::index_sequence_for<Ts...>{});
                }
            }
        }
    }

    template <typename Fn>
    void eachChunk(Fn&& fn) const
    {
        const auto& plan = world_.template queryPlan<Ts...>(excludeTypes_, optionalTypes_);
        for (size_t matchIndex = 0; matchIndex < plan.matchingArchetypes.size(); ++matchIndex) {
            const uint32_t archetypeId = plan.matchingArchetypes[matchIndex];
            const auto& archetype = world_.archetypes_[archetypeId];
            const auto& remaps = plan.remapsByArchetype[matchIndex];

            std::array<size_t, sizeof...(Ts)> columns{};
            std::array<size_t, sizeof...(Ts)> sizes{};
            loadLayout(remaps, columns, sizes, std::index_sequence_for<Ts...>{});

            for (uint32_t chunkIndex = 0; chunkIndex < archetype.chunks.size(); ++chunkIndex) {
                const auto& chunk = archetype.chunks[chunkIndex];
                if (chunk.count == 0) {
                    continue;
                }
                invokeChunk(std::forward<Fn>(fn), chunk, columns, sizes, std::index_sequence_for<Ts...>{});
            }
        }
    }

private:
    template <size_t... Is>
    static void loadLayout(const std::vector<World::QueryPlan::ColumnRemap>& remaps,
        std::array<size_t, sizeof...(Ts)>& columns,
        std::array<size_t, sizeof...(Ts)>& sizes,
        std::index_sequence<Is...>)
    {
        ((columns[Is] = remaps[Is].columnIndex, sizes[Is] = remaps[Is].componentSize), ...);
    }

    template <typename Fn, size_t... Is>
    static void invokeEach(Fn&& fn,
        Entity entity,
        const auto& chunk,
        uint32_t row,
        const std::array<size_t, sizeof...(Ts)>& columns,
        const std::array<size_t, sizeof...(Ts)>& sizes,
        std::index_sequence<Is...>)
    {
        std::forward<Fn>(fn)(entity,
            *reinterpret_cast<const Ts*>(World::componentPtr(chunk, columns[Is], row, sizes[Is]))...);
    }

    template <typename Fn, size_t... Is>
    static void invokeChunk(Fn&& fn,
        const auto& chunk,
        const std::array<size_t, sizeof...(Ts)>& columns,
        const std::array<size_t, sizeof...(Ts)>& sizes,
        std::index_sequence<Is...>)
    {
        std::forward<Fn>(fn)(
            chunk.entities.data(),
            reinterpret_cast<const Ts*>(World::componentPtr(chunk, columns[Is], 0, sizes[Is]))...,
            chunk.count);
    }

    const World& world_;
    mutable std::vector<uint32_t> excludeTypes_{};
    mutable std::vector<uint32_t> optionalTypes_{};
};
