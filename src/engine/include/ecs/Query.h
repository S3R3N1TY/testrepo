#pragma once

#include <ecs/Entity.h>
#include <ecs/World.h>

#include <array>
#include <cstddef>
#include <tuple>
#include <type_traits>
#include <vector>

class World;

template <typename T>
using QueryStorage = typename QueryArg<std::remove_cvref_t<T>>::StorageType;

template <typename T>
using QueryPointer = std::conditional_t<QueryArg<std::remove_cvref_t<T>>::kConst, const QueryStorage<T>*, QueryStorage<T>*>;

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
            const auto& optionalRemaps = plan.optionalRemapsByArchetype[matchIndex];

            std::array<size_t, sizeof...(Ts)> columns{};
            std::array<size_t, sizeof...(Ts)> sizes{};
            std::array<bool, sizeof...(Ts)> present{};
            loadLayout(remaps, optionalRemaps, columns, sizes, present, std::index_sequence_for<Ts...>{});

            for (uint32_t chunkIndex = 0; chunkIndex < archetype.chunks.size(); ++chunkIndex) {
                auto& chunk = archetype.chunks[chunkIndex];
                for (uint32_t row = 0; row < chunk.count; ++row) {
                    markEntityWriteAccess(world_, archetypeId, chunkIndex, row, remaps, optionalRemaps, std::index_sequence_for<Ts...>{});
                    const Entity entity = chunk.entities[row];
                    invokeEach(std::forward<Fn>(fn), entity, chunk, row, columns, sizes, present, std::index_sequence_for<Ts...>{});
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
            const auto& optionalRemaps = plan.optionalRemapsByArchetype[matchIndex];

            std::array<size_t, sizeof...(Ts)> columns{};
            std::array<size_t, sizeof...(Ts)> sizes{};
            std::array<bool, sizeof...(Ts)> present{};
            loadLayout(remaps, optionalRemaps, columns, sizes, present, std::index_sequence_for<Ts...>{});

            for (uint32_t chunkIndex = 0; chunkIndex < archetype.chunks.size(); ++chunkIndex) {
                auto& chunk = archetype.chunks[chunkIndex];
                if (chunk.count == 0) {
                    continue;
                }
                markChunkWriteAccess(world_, archetypeId, chunkIndex, static_cast<uint32_t>(chunk.count), remaps, optionalRemaps, std::index_sequence_for<Ts...>{});
                invokeChunkOptional(std::forward<Fn>(fn), chunk, columns, sizes, present, std::index_sequence_for<Ts...>{});
            }
        }
    }

private:
    template <size_t... Is>
    static void markEntityWriteAccess(World& world,
        uint32_t archetypeId,
        uint32_t chunkIndex,
        uint32_t row,
        const std::vector<World::QueryPlan::ColumnRemap>& remaps,
        const std::vector<std::optional<World::QueryPlan::ColumnRemap>>& optionalRemaps,
        std::index_sequence<Is...>)
    {
        size_t requiredIdx = 0;
        size_t optionalIdx = 0;
        (([&] {
            using Raw = std::remove_cvref_t<std::tuple_element_t<Is, std::tuple<Ts...>>>;
            if constexpr (QueryArg<Raw>::kOptional) {
                if constexpr (!QueryArg<Raw>::kConst) {
                    const auto& remapOpt = optionalRemaps[optionalIdx];
                    if (remapOpt.has_value()) {
                        world.markChunkComponentDirty(archetypeId, chunkIndex, remapOpt->componentType, row);
                    }
                }
                optionalIdx += 1;
            }
            else {
                if constexpr (!QueryArg<Raw>::kConst) {
                    const auto& remap = remaps[requiredIdx];
                    world.markChunkComponentDirty(archetypeId, chunkIndex, remap.componentType, row);
                }
                requiredIdx += 1;
            }
        }()), ...);
    }

    template <size_t... Is>
    static void markChunkWriteAccess(World& world,
        uint32_t archetypeId,
        uint32_t chunkIndex,
        uint32_t count,
        const std::vector<World::QueryPlan::ColumnRemap>& remaps,
        const std::vector<std::optional<World::QueryPlan::ColumnRemap>>& optionalRemaps,
        std::index_sequence<Is...>)
    {
        for (uint32_t row = 0; row < count; ++row) {
            markEntityWriteAccess(world, archetypeId, chunkIndex, row, remaps, optionalRemaps, std::index_sequence<Is...>{});
        }
    }

    template <size_t... Is>
    static void loadLayout(const std::vector<World::QueryPlan::ColumnRemap>& remaps,
        const std::vector<std::optional<World::QueryPlan::ColumnRemap>>& optionalRemaps,
        std::array<size_t, sizeof...(Ts)>& columns,
        std::array<size_t, sizeof...(Ts)>& sizes,
        std::array<bool, sizeof...(Ts)>& present,
        std::index_sequence<Is...>)
    {
        size_t requiredIdx = 0;
        size_t optionalIdx = 0;
        (([&] {
            using Raw = std::remove_cvref_t<std::tuple_element_t<Is, std::tuple<Ts...>>>;
            if constexpr (QueryArg<Raw>::kOptional) {
                const auto& remapOpt = optionalRemaps[optionalIdx++];
                if (remapOpt.has_value()) {
                    columns[Is] = remapOpt->columnIndex;
                    sizes[Is] = remapOpt->componentSize;
                    present[Is] = true;
                }
                else {
                    columns[Is] = 0;
                    sizes[Is] = sizeof(QueryStorage<Raw>);
                    present[Is] = false;
                }
            }
            else {
                columns[Is] = remaps[requiredIdx].columnIndex;
                sizes[Is] = remaps[requiredIdx].componentSize;
                present[Is] = true;
                requiredIdx += 1;
            }
        }()), ...);
    }

    template <typename T>
    static decltype(auto) argFrom(auto& chunk, uint32_t row, size_t column, size_t size, bool present)
    {
        using Raw = std::remove_cvref_t<T>;
        using Store = QueryStorage<Raw>;
        if constexpr (QueryArg<Raw>::kOptional) {
            if (!present) {
                return static_cast<QueryPointer<Raw>>(nullptr);
            }
            return reinterpret_cast<QueryPointer<Raw>>(World::componentPtr(chunk, column, row, size));
        }
        else {
            return *reinterpret_cast<std::conditional_t<QueryArg<Raw>::kConst, const Store*, Store*>>(World::componentPtr(chunk, column, row, size));
        }
    }

    template <typename Fn, size_t... Is>
    static void invokeEach(Fn&& fn,
        Entity entity,
        auto& chunk,
        uint32_t row,
        const std::array<size_t, sizeof...(Ts)>& columns,
        const std::array<size_t, sizeof...(Ts)>& sizes,
        const std::array<bool, sizeof...(Ts)>& present,
        std::index_sequence<Is...>)
    {
        std::forward<Fn>(fn)(entity, argFrom<Ts>(chunk, row, columns[Is], sizes[Is], present[Is])...);
    }

    template <typename T>
    static auto chunkArgFrom(auto& chunk, size_t column, size_t size, bool present)
    {
        using Raw = std::remove_cvref_t<T>;
        using Store = QueryStorage<Raw>;
        if constexpr (QueryArg<Raw>::kOptional) {
            if (!present) {
                return static_cast<QueryPointer<Raw>>(nullptr);
            }
            return reinterpret_cast<QueryPointer<Raw>>(World::componentPtr(chunk, column, 0, size));
        }
        else {
            return reinterpret_cast<QueryPointer<Raw>>(World::componentPtr(chunk, column, 0, size));
        }
    }

    template <typename Fn, size_t... Is>
    static void invokeChunkOptional(Fn&& fn,
        auto& chunk,
        const std::array<size_t, sizeof...(Ts)>& columns,
        const std::array<size_t, sizeof...(Ts)>& sizes,
        const std::array<bool, sizeof...(Ts)>& present,
        std::index_sequence<Is...>)
    {
        std::forward<Fn>(fn)(
            chunk.entities.data(),
            chunkArgFrom<Ts>(chunk, columns[Is], sizes[Is], present[Is])...,
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
            const auto& optionalRemaps = plan.optionalRemapsByArchetype[matchIndex];

            std::array<size_t, sizeof...(Ts)> columns{};
            std::array<size_t, sizeof...(Ts)> sizes{};
            std::array<bool, sizeof...(Ts)> present{};
            loadLayout(remaps, optionalRemaps, columns, sizes, present, std::index_sequence_for<Ts...>{});

            for (uint32_t chunkIndex = 0; chunkIndex < archetype.chunks.size(); ++chunkIndex) {
                const auto& chunk = archetype.chunks[chunkIndex];
                for (uint32_t row = 0; row < chunk.count; ++row) {
                    const Entity entity = chunk.entities[row];
                    invokeEach(std::forward<Fn>(fn), entity, chunk, row, columns, sizes, present, std::index_sequence_for<Ts...>{});
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
            const auto& optionalRemaps = plan.optionalRemapsByArchetype[matchIndex];

            std::array<size_t, sizeof...(Ts)> columns{};
            std::array<size_t, sizeof...(Ts)> sizes{};
            std::array<bool, sizeof...(Ts)> present{};
            loadLayout(remaps, optionalRemaps, columns, sizes, present, std::index_sequence_for<Ts...>{});

            for (uint32_t chunkIndex = 0; chunkIndex < archetype.chunks.size(); ++chunkIndex) {
                const auto& chunk = archetype.chunks[chunkIndex];
                if (chunk.count == 0) {
                    continue;
                }
                invokeChunkOptional(std::forward<Fn>(fn), chunk, columns, sizes, present, std::index_sequence_for<Ts...>{});
            }
        }
    }

private:
    template <size_t... Is>
    static void markChunkWriteIntents(World& world,
        uint32_t archetypeId,
        uint32_t chunkIndex,
        const std::vector<World::QueryPlan::ColumnRemap>& remaps,
        const std::vector<std::optional<World::QueryPlan::ColumnRemap>>& optionalRemaps,
        std::index_sequence<Is...>)
    {
        size_t requiredIdx = 0;
        size_t optionalIdx = 0;
        (([&] {
            using Raw = std::remove_cvref_t<std::tuple_element_t<Is, std::tuple<Ts...>>>;
            if constexpr (QueryArg<Raw>::kOptional) {
                if constexpr (!QueryArg<Raw>::kConst) {
                    const auto& remapOpt = optionalRemaps[optionalIdx];
                    if (remapOpt.has_value()) {
                        world.bumpVersion(remapOpt->componentType);
                        world.bumpChunkVersion(archetypeId, chunkIndex, remapOpt->componentType);
                    }
                }
                optionalIdx += 1;
            }
            else {
                if constexpr (!QueryArg<Raw>::kConst) {
                    const auto& remap = remaps[requiredIdx];
                    world.bumpVersion(remap.componentType);
                    world.bumpChunkVersion(archetypeId, chunkIndex, remap.componentType);
                }
                requiredIdx += 1;
            }
        }()), ...);
    }

    template <size_t... Is>
    static void loadLayout(const std::vector<World::QueryPlan::ColumnRemap>& remaps,
        const std::vector<std::optional<World::QueryPlan::ColumnRemap>>& optionalRemaps,
        std::array<size_t, sizeof...(Ts)>& columns,
        std::array<size_t, sizeof...(Ts)>& sizes,
        std::array<bool, sizeof...(Ts)>& present,
        std::index_sequence<Is...>)
    {
        size_t requiredIdx = 0;
        size_t optionalIdx = 0;
        (([&] {
            using Raw = std::remove_cvref_t<std::tuple_element_t<Is, std::tuple<Ts...>>>;
            if constexpr (QueryArg<Raw>::kOptional) {
                const auto& remapOpt = optionalRemaps[optionalIdx++];
                if (remapOpt.has_value()) {
                    columns[Is] = remapOpt->columnIndex;
                    sizes[Is] = remapOpt->componentSize;
                    present[Is] = true;
                }
                else {
                    columns[Is] = 0;
                    sizes[Is] = sizeof(QueryStorage<Raw>);
                    present[Is] = false;
                }
            }
            else {
                columns[Is] = remaps[requiredIdx].columnIndex;
                sizes[Is] = remaps[requiredIdx].componentSize;
                present[Is] = true;
                requiredIdx += 1;
            }
        }()), ...);
    }

    template <typename T>
    static decltype(auto) argFrom(const auto& chunk, uint32_t row, size_t column, size_t size, bool present)
    {
        using Raw = std::remove_cvref_t<T>;
        using Store = QueryStorage<Raw>;
        if constexpr (QueryArg<Raw>::kOptional) {
            if (!present) {
                return static_cast<const Store*>(nullptr);
            }
            return reinterpret_cast<const Store*>(World::componentPtr(chunk, column, row, size));
        }
        else {
            return *reinterpret_cast<const Store*>(World::componentPtr(chunk, column, row, size));
        }
    }

    template <typename Fn, size_t... Is>
    static void invokeEach(Fn&& fn,
        Entity entity,
        const auto& chunk,
        uint32_t row,
        const std::array<size_t, sizeof...(Ts)>& columns,
        const std::array<size_t, sizeof...(Ts)>& sizes,
        const std::array<bool, sizeof...(Ts)>& present,
        std::index_sequence<Is...>)
    {
        std::forward<Fn>(fn)(entity, argFrom<Ts>(chunk, row, columns[Is], sizes[Is], present[Is])...);
    }

    template <typename T>
    static auto chunkArgFrom(const auto& chunk, size_t column, size_t size, bool present)
    {
        using Raw = std::remove_cvref_t<T>;
        using Store = QueryStorage<Raw>;
        if constexpr (QueryArg<Raw>::kOptional) {
            if (!present) {
                return static_cast<const Store*>(nullptr);
            }
            return reinterpret_cast<const Store*>(World::componentPtr(chunk, column, 0, size));
        }
        else {
            return reinterpret_cast<const Store*>(World::componentPtr(chunk, column, 0, size));
        }
    }

    template <typename Fn, size_t... Is>
    static void invokeChunkOptional(Fn&& fn,
        const auto& chunk,
        const std::array<size_t, sizeof...(Ts)>& columns,
        const std::array<size_t, sizeof...(Ts)>& sizes,
        const std::array<bool, sizeof...(Ts)>& present,
        std::index_sequence<Is...>)
    {
        std::forward<Fn>(fn)(
            chunk.entities.data(),
            chunkArgFrom<Ts>(chunk, columns[Is], sizes[Is], present[Is])...,
            chunk.count);
    }

    const World& world_;
    mutable std::vector<uint32_t> excludeTypes_{};
    mutable std::vector<uint32_t> optionalTypes_{};
};
