#pragma once

#include <ecs/ComponentResidency.h>
#include <ecs/Entity.h>
#include <ecs/StructuralPhase.h>

#include <algorithm>
#include <array>
#include <any>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

class StructuralCommandBuffer;

template <typename... Ts>
class Query;
template <typename... Ts>
class ConstQuery;

template <typename T>
struct OptionalComponent {
    using ValueType = T;
};

template <typename T>
using Optional = OptionalComponent<T>;

template <typename T>
struct QueryArg {
    using StorageType = std::remove_cv_t<std::remove_reference_t<T>>;
    static constexpr bool kOptional = false;
    static constexpr bool kConst = std::is_const_v<std::remove_reference_t<T>>;
};

template <typename T>
struct QueryArg<OptionalComponent<T>> {
    using StorageType = std::remove_cv_t<std::remove_reference_t<T>>;
    static constexpr bool kOptional = true;
    static constexpr bool kConst = std::is_const_v<std::remove_reference_t<T>>;
};

enum class ComponentAccess : uint8_t {
    ReadOnly,
    ReadWrite
};

class World {
public:
    World() = default;

    [[nodiscard]] Entity createEntity();
    void destroyEntity(Entity entity);

    [[nodiscard]] bool isAlive(Entity entity) const;
    [[nodiscard]] const std::vector<Entity>& entities() const noexcept;

    struct EntitySnapshot {
        struct HotComponentSnapshot {
            uint32_t typeId{ 0 };
            std::vector<std::byte> storage{};
            size_t size{ 0 };
            size_t align{ 1 };
            std::function<void(void*)> destroy{};

            HotComponentSnapshot() = default;
            HotComponentSnapshot(const HotComponentSnapshot&) = delete;
            HotComponentSnapshot& operator=(const HotComponentSnapshot&) = delete;
            HotComponentSnapshot(HotComponentSnapshot&& other) noexcept = default;
            HotComponentSnapshot& operator=(HotComponentSnapshot&& other) noexcept = default;
            ~HotComponentSnapshot()
            {
                if (destroy && !storage.empty()) {
                    void* raw = storage.data();
                    size_t space = storage.size();
                    if (void* aligned = std::align(align, size, raw, space); aligned != nullptr) {
                        destroy(aligned);
                    }
                }
            }
        };

        Entity entity{};
        std::vector<HotComponentSnapshot> hot{};
        std::unordered_map<std::type_index, std::any> cold{};
    };

    [[nodiscard]] std::optional<EntitySnapshot> snapshotEntity(Entity entity) const;
    void restoreEntity(const EntitySnapshot& snapshot);

    template <typename T, typename... Args>
    T& emplaceComponent(Entity entity, Args&&... args)
    {
        validateAlive(entity);
        if (!mutationEnabled_) {
            throw std::runtime_error("World::emplaceComponent mutation is disabled during read phase");
        }
        if constexpr (ComponentResidencyTrait<T>::value == ComponentResidency::ColdSparse) {
            T value{ std::forward<Args>(args)... };
            return coldPoolFor<T>().emplace(entity, std::move(value));
        }

        T value{ std::forward<Args>(args)... };
        return emplaceHotComponent(entity, std::move(value));
    }

    template <typename T>
    void removeComponent(Entity entity)
    {
        if (!isAlive(entity)) {
            return;
        }
        if (!mutationEnabled_) {
            throw std::runtime_error("World::removeComponent mutation is disabled during read phase");
        }

        if constexpr (ComponentResidencyTrait<T>::value == ComponentResidency::ColdSparse) {
            if (auto* pool = tryColdPoolFor<T>(); pool != nullptr) {
                pool->remove(entity);
            }
            return;
        }

        const auto typeId = ensureComponentRegistered<T>();
        removeHotComponent(entity, typeId);
    }

    template <typename T>
    bool hasComponent(Entity entity) const
    {
        if (!isAlive(entity)) {
            return false;
        }

        if constexpr (ComponentResidencyTrait<T>::value == ComponentResidency::ColdSparse) {
            const auto* pool = tryColdPoolFor<T>();
            return pool != nullptr && pool->has(entity);
        }

        const auto typeId = findHotComponentType<T>();
        if (!typeId.has_value()) {
            return false;
        }

        const EntityRecord& rec = records_[entity.id];
        if (rec.archetypeId == kInvalidArchetype) {
            return false;
        }

        return archetypes_[rec.archetypeId].columnByType.contains(*typeId);
    }

    template <typename T>
    T* getComponent(Entity entity)
    {
        if (!isAlive(entity)) {
            return nullptr;
        }

        if constexpr (ComponentResidencyTrait<T>::value == ComponentResidency::ColdSparse) {
            auto* pool = tryColdPoolFor<T>();
            return pool == nullptr ? nullptr : pool->get(entity);
        }

        const auto typeId = findHotComponentType<T>();
        if (!typeId.has_value()) {
            return nullptr;
        }

        return static_cast<T*>(getHotComponentRaw(entity, *typeId));
    }

    template <typename T>
    const T* getComponent(Entity entity) const
    {
        return const_cast<World*>(this)->template getComponent<T>(entity);
    }

    template <typename... Ts>
    [[nodiscard]] Query<Ts...> query();

    template <typename... Ts>
    [[nodiscard]] ConstQuery<Ts...> query() const;

    struct QueryPlan {
        struct ColumnRemap {
            uint32_t componentType{ 0 };
            ComponentAccess access{ ComponentAccess::ReadOnly };
            size_t columnIndex{ 0 };
            size_t componentSize{ 0 };
        };

        std::vector<uint32_t> includeTypes{};
        std::vector<uint32_t> excludeTypes{};
        std::vector<uint32_t> optionalTypes{};
        std::vector<ComponentAccess> includeAccesses{};
        std::vector<uint32_t> matchingArchetypes{};
        std::vector<std::vector<ColumnRemap>> remapsByArchetype{};
        std::vector<std::vector<std::optional<ColumnRemap>>> optionalRemapsByArchetype{};
        uint64_t archetypeEpoch{ 0 };
    };

    template <typename... Ts>
    [[nodiscard]] const QueryPlan& queryPlan(
        std::vector<uint32_t> excludeTypes = {},
        std::vector<uint32_t> optionalTypes = {}) const
    {
        std::vector<ComponentTypeId> includeTypes{};
        std::vector<ComponentAccess> includeAccesses{};
        includeTypes.reserve(sizeof...(Ts));
        includeAccesses.reserve(sizeof...(Ts));

        (([&] {
            using Decayed = typename QueryArg<std::remove_cvref_t<Ts>>::StorageType;
            if constexpr (ComponentResidencyTrait<Decayed>::value == ComponentResidency::HotArchetype) {
                const auto typeId = findHotComponentType<Decayed>();
                if (typeId.has_value()) {
                    if constexpr (QueryArg<std::remove_cvref_t<Ts>>::kOptional) {
                        optionalTypes.push_back(*typeId);
                    }
                    else {
                        includeTypes.push_back(*typeId);
                        includeAccesses.push_back(QueryArg<std::remove_cvref_t<Ts>>::kConst ? ComponentAccess::ReadOnly : ComponentAccess::ReadWrite);
                    }
                }
            }
        }()), ...);

        return queryPlanDynamic(std::move(includeTypes), std::move(includeAccesses), std::move(excludeTypes), std::move(optionalTypes));
    }

    struct ChunkHandle {
        uint32_t archetypeId{ 0 };
        uint32_t chunkIndex{ 0 };
    };

    struct ChunkView {
        ChunkHandle handle{};
        const Entity* entities{ nullptr };
        size_t count{ 0 };
    };

    template <typename... Ts, typename Fn>
    void forEachChunk(const QueryPlan& plan, Fn&& fn)
    {
        for (size_t planIndex = 0; planIndex < plan.matchingArchetypes.size(); ++planIndex) {
            const uint32_t archetypeId = plan.matchingArchetypes[planIndex];
            Archetype& arch = archetypes_[archetypeId];
            for (uint32_t chunkIndex = 0; chunkIndex < arch.chunks.size(); ++chunkIndex) {
                Chunk& chunk = arch.chunks[chunkIndex];
                if (chunk.count == 0) {
                    continue;
                }
                fn(ChunkView{
                    .handle = ChunkHandle{ .archetypeId = archetypeId, .chunkIndex = chunkIndex },
                    .entities = chunk.entities.data(),
                    .count = chunk.count
                });
            }
        }
    }

    template <typename... Ts, typename Fn>
    void forEachChunk(const QueryPlan& plan, Fn&& fn) const
    {
        for (size_t planIndex = 0; planIndex < plan.matchingArchetypes.size(); ++planIndex) {
            const uint32_t archetypeId = plan.matchingArchetypes[planIndex];
            const Archetype& arch = archetypes_[archetypeId];
            for (uint32_t chunkIndex = 0; chunkIndex < arch.chunks.size(); ++chunkIndex) {
                const Chunk& chunk = arch.chunks[chunkIndex];
                if (chunk.count == 0) {
                    continue;
                }
                fn(ChunkView{
                    .handle = ChunkHandle{ .archetypeId = archetypeId, .chunkIndex = chunkIndex },
                    .entities = chunk.entities.data(),
                    .count = chunk.count
                });
            }
        }
    }

    void beginFrame();
    void playbackPhase(StructuralPlaybackPhase phase);
    void endFrame();
    void setStructuralCommandBuffer(StructuralCommandBuffer* commandBuffer);

    void beginReadPhase() { mutationEnabled_ = false; }
    void endReadPhase() { mutationEnabled_ = true; }
    void beginSystemWriteScope();
    void endSystemWriteScope();

    [[nodiscard]] uint64_t componentVersion(uint32_t typeId) const noexcept;

    template <typename T>
    [[nodiscard]] std::optional<uint32_t> componentTypeId() const
    {
        return findHotComponentType<T>();
    }

    class QueryBuilder {
    public:
        explicit QueryBuilder(const World& world) : world_(world) {}

        template <typename T>
        QueryBuilder& include(ComponentAccess access = ComponentAccess::ReadOnly)
        {
            using Decayed = std::remove_cvref_t<T>;
            if constexpr (ComponentResidencyTrait<Decayed>::value == ComponentResidency::HotArchetype) {
                if (auto id = world_.template findHotComponentType<Decayed>(); id.has_value()) {
                    includeTypes_.push_back(*id);
                    accesses_.push_back(access);
                }
            }
            return *this;
        }

        template <typename T>
        QueryBuilder& exclude()
        {
            using Decayed = std::remove_cvref_t<T>;
            if constexpr (ComponentResidencyTrait<Decayed>::value == ComponentResidency::HotArchetype) {
                if (auto id = world_.template findHotComponentType<Decayed>(); id.has_value()) {
                    excludeTypes_.push_back(*id);
                }
            }
            return *this;
        }

        template <typename T>
        QueryBuilder& optional()
        {
            using Decayed = std::remove_cvref_t<T>;
            if constexpr (ComponentResidencyTrait<Decayed>::value == ComponentResidency::HotArchetype) {
                if (auto id = world_.template findHotComponentType<Decayed>(); id.has_value()) {
                    optionalTypes_.push_back(*id);
                }
            }
            return *this;
        }

        [[nodiscard]] const QueryPlan& plan() const
        {
            return world_.queryPlanDynamic(includeTypes_, accesses_, excludeTypes_, optionalTypes_);
        }

    private:
        const World& world_;
        mutable std::vector<uint32_t> includeTypes_{};
        mutable std::vector<ComponentAccess> accesses_{};
        mutable std::vector<uint32_t> excludeTypes_{};
        mutable std::vector<uint32_t> optionalTypes_{};
    };

    [[nodiscard]] QueryBuilder queryBuilder() const { return QueryBuilder{ *this }; }

private:
    using ComponentTypeId = uint32_t;

    struct QueryKey {
        std::vector<ComponentTypeId> include{};
        std::vector<ComponentTypeId> exclude{};
        std::vector<ComponentTypeId> optional{};
        std::vector<ComponentAccess> access{};

        friend bool operator==(const QueryKey& lhs, const QueryKey& rhs) = default;
    };

    struct QueryKeyHash {
        size_t operator()(const QueryKey& key) const noexcept
        {
            size_t h = 1469598103934665603ull;
            auto mix = [&](uint64_t v) {
                h ^= static_cast<size_t>(v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2));
            };
            for (auto v : key.include) mix(v);
            mix(0xABCDEF);
            for (auto v : key.exclude) mix(v);
            mix(0x123456);
            for (auto v : key.optional) mix(v);
            mix(0x777777);
            for (auto v : key.access) mix(static_cast<uint32_t>(v));
            return h;
        }
    };

    static constexpr uint32_t kInvalidArchetype = UINT32_MAX;
    static constexpr uint32_t kInvalidChunk = UINT32_MAX;

    struct ComponentMeta {
        size_t size{ 0 };
        size_t align{ 1 };
        std::function<void(void*)> destroy{};
        std::function<void(void*, const void*)> copyConstruct{};
        std::function<void(void*, void*)> moveAssignOrConstruct{};
        uint64_t version{ 1 };
    };

    struct EntityRecord {
        uint32_t generation{ 0 };
        bool alive{ false };
        uint32_t archetypeId{ kInvalidArchetype };
        uint32_t chunkIndex{ kInvalidChunk };
        uint32_t row{ 0 };
    };

    struct AlignedColumn {
        std::unique_ptr<std::byte[]> buffer{};
        std::byte* data{ nullptr };
        size_t sizeBytes{ 0 };
        size_t alignment{ 1 };

        static AlignedColumn create(size_t bytes, size_t alignment)
        {
            const size_t allocSize = bytes + alignment;
            auto raw = std::make_unique<std::byte[]>(allocSize);
            void* ptr = raw.get();
            size_t space = allocSize;
            void* aligned = std::align(alignment, bytes, ptr, space);
            if (aligned == nullptr) {
                throw std::runtime_error("Failed to align component column buffer");
            }
            return AlignedColumn{ .buffer = std::move(raw), .data = static_cast<std::byte*>(aligned), .sizeBytes = bytes, .alignment = alignment };
        }
    };

    struct Chunk {
        explicit Chunk(size_t capacity)
            : entities(capacity)
            , count(0)
        {
        }

        std::vector<Entity> entities{};
        std::vector<AlignedColumn> columns{};
        std::vector<uint64_t> columnVersions{};
        std::vector<uint64_t> chunkDirtyEpochByColumn{};
        std::vector<std::vector<uint64_t>> dirtyRowsByColumn{};
        std::vector<std::vector<uint64_t>> rowDirtyEpochByColumn{};
        uint64_t structuralVersion{ 1 };
        size_t count{ 0 };
    };

    struct ArchetypeKey {
        std::vector<ComponentTypeId> signature{};
        friend bool operator==(const ArchetypeKey& lhs, const ArchetypeKey& rhs) = default;
    };

    struct ArchetypeKeyHash {
        size_t operator()(const ArchetypeKey& key) const noexcept
        {
            size_t h = 1469598103934665603ull;
            for (const ComponentTypeId id : key.signature) {
                h ^= static_cast<size_t>(id + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2));
            }
            return h;
        }
    };

    struct Archetype {
        ArchetypeKey key{};
        std::unordered_map<ComponentTypeId, size_t> columnByType{};
        std::vector<Chunk> chunks{};

        [[nodiscard]] bool matchesAll(const std::vector<ComponentTypeId>& types) const
        {
            for (ComponentTypeId id : types) {
                if (!columnByType.contains(id)) {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] bool matchesAny(const std::vector<ComponentTypeId>& types) const
        {
            for (ComponentTypeId id : types) {
                if (columnByType.contains(id)) {
                    return true;
                }
            }
            return false;
        }
    };

    class IColdPool {
    public:
        virtual ~IColdPool() = default;
        virtual void remove(Entity entity) = 0;
        [[nodiscard]] virtual std::optional<std::any> snapshot(Entity entity) const = 0;
        virtual void restore(Entity entity, const std::any& value) = 0;
    };

    template <typename T>
    class ColdPool final : public IColdPool {
    public:
        T& emplace(Entity entity, T value)
        {
            auto it = sparse_.find(entity.id);
            if (it != sparse_.end()) {
                dense_[it->second] = std::move(value);
                return dense_[it->second];
            }

            const size_t index = dense_.size();
            denseEntities_.push_back(entity);
            dense_.push_back(std::move(value));
            sparse_.insert_or_assign(entity.id, index);
            return dense_.back();
        }

        void remove(Entity entity) override
        {
            auto it = sparse_.find(entity.id);
            if (it == sparse_.end()) {
                return;
            }
            const size_t idx = it->second;
            const size_t last = dense_.size() - 1;
            if (idx != last) {
                dense_[idx] = std::move(dense_[last]);
                denseEntities_[idx] = denseEntities_[last];
                sparse_.insert_or_assign(denseEntities_[idx].id, idx);
            }
            dense_.pop_back();
            denseEntities_.pop_back();
            sparse_.erase(it);
        }

        [[nodiscard]] bool has(Entity entity) const
        {
            return sparse_.contains(entity.id);
        }

        [[nodiscard]] T* get(Entity entity)
        {
            auto it = sparse_.find(entity.id);
            return it == sparse_.end() ? nullptr : &dense_[it->second];
        }

        [[nodiscard]] std::optional<std::any> snapshot(Entity entity) const override
        {
            auto it = sparse_.find(entity.id);
            if (it == sparse_.end()) {
                return std::nullopt;
            }
            return std::any{ dense_[it->second] };
        }

        void restore(Entity entity, const std::any& value) override
        {
            emplace(entity, std::any_cast<const T&>(value));
        }

    private:
        std::vector<Entity> denseEntities_{};
        std::vector<T> dense_{};
        std::unordered_map<uint32_t, size_t> sparse_{};
    };

    struct HotInsertResult {
        uint32_t archetypeId{ 0 };
        uint32_t chunkIndex{ 0 };
        uint32_t row{ 0 };
    };

    template <typename T>
    static ComponentMeta makeComponentMeta()
    {
        static_assert(std::is_copy_constructible_v<T>, "Hot archetype components must be copy constructible");
        ComponentMeta meta{};
        meta.size = sizeof(T);
        meta.align = alignof(T);
        meta.destroy = [](void* ptr) { reinterpret_cast<T*>(ptr)->~T(); };
        meta.copyConstruct = [](void* dst, const void* src) { new (dst) T(*reinterpret_cast<const T*>(src)); };
        meta.moveAssignOrConstruct = [](void* dst, void* src) {
            if constexpr (std::is_move_assignable_v<T>) {
                *reinterpret_cast<T*>(dst) = std::move(*reinterpret_cast<T*>(src));
                reinterpret_cast<T*>(src)->~T();
            }
            else {
                reinterpret_cast<T*>(dst)->~T();
                new (dst) T(std::move(*reinterpret_cast<T*>(src)));
                reinterpret_cast<T*>(src)->~T();
            }
        };
        return meta;
    }

    template <typename T>
    ComponentTypeId ensureComponentRegistered()
    {
        const std::type_index key(typeid(T));
        auto it = hotTypeByStdType_.find(key);
        if (it != hotTypeByStdType_.end()) {
            return it->second;
        }

        const ComponentTypeId id = static_cast<ComponentTypeId>(hotComponentMeta_.size());
        hotTypeByStdType_.insert_or_assign(key, id);
        hotComponentMeta_.push_back(makeComponentMeta<T>());
        return id;
    }

    template <typename T>
    std::optional<ComponentTypeId> findHotComponentType() const
    {
        const auto it = hotTypeByStdType_.find(std::type_index(typeid(T)));
        if (it == hotTypeByStdType_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    template <typename T>
    ColdPool<T>& coldPoolFor()
    {
        const std::type_index key(typeid(T));
        auto it = coldPools_.find(key);
        if (it == coldPools_.end()) {
            auto pool = std::make_unique<ColdPool<T>>();
            auto [newIt, _] = coldPools_.emplace(key, std::move(pool));
            it = newIt;
        }
        return *static_cast<ColdPool<T>*>(it->second.get());
    }

    template <typename T>
    ColdPool<T>* tryColdPoolFor()
    {
        const std::type_index key(typeid(T));
        auto it = coldPools_.find(key);
        return it == coldPools_.end() ? nullptr : static_cast<ColdPool<T>*>(it->second.get());
    }

    template <typename T>
    const ColdPool<T>* tryColdPoolFor() const
    {
        return const_cast<World*>(this)->tryColdPoolFor<T>();
    }

    template <typename T>
    T& emplaceHotComponent(Entity entity, T value)
    {
        const ComponentTypeId newType = ensureComponentRegistered<T>();
        EntityRecord& record = records_[entity.id];

        if (record.archetypeId != kInvalidArchetype) {
            Archetype& arch = archetypes_[record.archetypeId];
            auto colIt = arch.columnByType.find(newType);
            if (colIt != arch.columnByType.end()) {
                Chunk& chunk = arch.chunks[record.chunkIndex];
                void* dst = componentPtr(chunk, colIt->second, record.row, hotComponentMeta_[newType].size);
                *reinterpret_cast<T*>(dst) = std::move(value);
                bumpVersion(newType);
                bumpChunkVersion(record.archetypeId, record.chunkIndex, newType);
                return *reinterpret_cast<T*>(dst);
            }
        }

        std::vector<ComponentTypeId> signature{};
        if (record.archetypeId != kInvalidArchetype) {
            signature = archetypes_[record.archetypeId].key.signature;
        }
        signature.push_back(newType);
        canonicalizeTypes(signature);

        const HotInsertResult inserted = moveEntityToArchetype(entity, signature, newType, &value);
        record.archetypeId = inserted.archetypeId;
        record.chunkIndex = inserted.chunkIndex;
        record.row = inserted.row;

        Archetype& dstArch = archetypes_[inserted.archetypeId];
        Chunk& dstChunk = dstArch.chunks[inserted.chunkIndex];
        const size_t col = dstArch.columnByType.at(newType);
        bumpVersion(newType);
        bumpChunkVersion(inserted.archetypeId, inserted.chunkIndex, newType);
        return *reinterpret_cast<T*>(componentPtr(dstChunk, col, inserted.row, hotComponentMeta_[newType].size));
    }

    static void canonicalizeTypes(std::vector<ComponentTypeId>& types)
    {
        std::ranges::sort(types);
        types.erase(std::unique(types.begin(), types.end()), types.end());
    }


    [[nodiscard]] const QueryPlan& queryPlanDynamic(
        std::vector<uint32_t> includeTypes,
        std::vector<ComponentAccess> includeAccesses,
        std::vector<uint32_t> excludeTypes,
        std::vector<uint32_t> optionalTypes) const;
    void removeHotComponent(Entity entity, ComponentTypeId typeId);
    [[nodiscard]] void* getHotComponentRaw(Entity entity, ComponentTypeId typeId);
    [[nodiscard]] HotInsertResult moveEntityToArchetype(Entity entity, const std::vector<ComponentTypeId>& signature, ComponentTypeId insertedType, const void* insertedComponent);

    [[nodiscard]] uint32_t archetypeForSignature(const std::vector<ComponentTypeId>& signature);
    [[nodiscard]] static void* componentPtr(Chunk& chunk, size_t column, size_t row, size_t elemSize)
    {
        return chunk.columns[column].data + (row * elemSize);
    }

    [[nodiscard]] static const void* componentPtr(const Chunk& chunk, size_t column, size_t row, size_t elemSize)
    {
        return chunk.columns[column].data + (row * elemSize);
    }

    void eraseFromArchetype(uint32_t archetypeId, uint32_t chunkIndex, uint32_t row);
    void validateAlive(Entity entity) const;
    void bumpVersion(ComponentTypeId typeId);
    void bumpChunkVersion(uint32_t archetypeId, uint32_t chunkIndex, ComponentTypeId typeId);
    void emplaceHotComponentCloned(Entity entity, ComponentTypeId typeId, const void* componentData);
    void markChunkComponentDirty(uint32_t archetypeId, uint32_t chunkIndex, ComponentTypeId typeId, uint32_t row);
    void markComponentDirtyByEntity(Entity entity, ComponentTypeId typeId);

public:
    void markComponentDirty(Entity entity, uint32_t typeId, uint32_t archetypeId = kInvalidArchetype, uint32_t chunkIndex = kInvalidChunk, uint32_t row = 0)
    {
        if (typeId == 0) {
            return;
        }
        if (archetypeId != kInvalidArchetype && chunkIndex != kInvalidChunk) {
            markChunkComponentDirty(archetypeId, chunkIndex, typeId, row);
            return;
        }
        markComponentDirtyByEntity(entity, typeId);
    }

    [[nodiscard]] uint64_t chunkVersion(ChunkHandle handle, uint32_t componentType) const;
    [[nodiscard]] uint64_t chunkStructuralVersion(ChunkHandle handle) const;
    [[nodiscard]] uint64_t chunkDirtyEpoch(ChunkHandle handle, uint32_t componentType) const;
    [[nodiscard]] uint64_t frameEpoch() const noexcept { return frameEpoch_; }

    template <typename T>
    void markModified(Entity entity)
    {
        if constexpr (ComponentResidencyTrait<T>::value == ComponentResidency::HotArchetype) {
            if (const auto type = findHotComponentType<T>(); type.has_value()) {
                markComponentDirtyByEntity(entity, *type);
            }
        }
    }

    std::vector<EntityRecord> records_{};
    std::vector<uint32_t> freeList_{};
    std::vector<Entity> aliveEntities_{};

    std::unordered_map<std::type_index, ComponentTypeId> hotTypeByStdType_{};
    std::vector<ComponentMeta> hotComponentMeta_{};

    std::vector<Archetype> archetypes_{};
    std::unordered_map<ArchetypeKey, uint32_t, ArchetypeKeyHash> archetypeByKey_{};
    uint64_t archetypeEpoch_{ 0 };

    mutable std::unordered_map<QueryKey, QueryPlan, QueryKeyHash> queryPlans_{};
    mutable std::shared_mutex queryPlansMutex_{};

    std::unordered_map<std::type_index, std::unique_ptr<IColdPool>> coldPools_{};

    StructuralCommandBuffer* structuralBuffer_{ nullptr };
    bool frameActive_{ false };
    bool mutationEnabled_{ true };
    uint64_t frameEpoch_{ 1 };
    std::unordered_set<uint64_t> pendingDirtyVersionBumps_{};
    uint32_t writeScopeDepth_{ 0 };


    template <typename... Ts>
    friend class Query;
    template <typename... Ts>
    friend class ConstQuery;
};

#include <ecs/Query.h>

template <typename... Ts>
[[nodiscard]] Query<Ts...> World::query()
{
    return Query<Ts...>{ *this };
}

template <typename... Ts>
[[nodiscard]] ConstQuery<Ts...> World::query() const
{
    return ConstQuery<Ts...>{ *this };
}
