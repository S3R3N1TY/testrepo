#include <ecs/StructuralCommandBuffer.h>
#include <ecs/World.h>

#include <algorithm>

namespace {
struct IncludeTerm {
    uint32_t type{ 0 };
    ComponentAccess access{ ComponentAccess::ReadOnly };
};

uint64_t dirtyKey(uint32_t archetypeId, uint32_t chunkIndex, uint32_t typeId)
{
    return (static_cast<uint64_t>(archetypeId) << 40u)
        ^ (static_cast<uint64_t>(chunkIndex) << 20u)
        ^ static_cast<uint64_t>(typeId);
}
}

Entity World::createEntity()
{
    if (!mutationEnabled_) {
        throw std::runtime_error("World::createEntity mutation is disabled during staged read pass");
    }

    uint32_t id = 0;
    if (!freeList_.empty()) {
        id = freeList_.back();
        freeList_.pop_back();
    }
    else {
        id = static_cast<uint32_t>(records_.size());
        records_.push_back(EntityRecord{});
    }

    EntityRecord& record = records_[id];
    record.alive = true;
    record.archetypeId = kInvalidArchetype;
    record.chunkIndex = kInvalidChunk;
    record.row = 0;

    const Entity entity{ .id = id, .generation = record.generation };
    aliveEntities_.push_back(entity);
    return entity;
}

void World::destroyEntity(Entity entity)
{
    if (!isAlive(entity)) {
        return;
    }
    if (!mutationEnabled_) {
        throw std::runtime_error("World::destroyEntity mutation is disabled during staged read pass");
    }

    EntityRecord& record = records_[entity.id];
    if (record.archetypeId != kInvalidArchetype) {
        eraseFromArchetype(record.archetypeId, record.chunkIndex, record.row);
        record.archetypeId = kInvalidArchetype;
        record.chunkIndex = kInvalidChunk;
    }

    for (auto& [_, pool] : coldPools_) {
        pool->remove(entity);
    }

    record.alive = false;
    record.generation += 1;
    freeList_.push_back(entity.id);

    aliveEntities_.erase(std::remove_if(aliveEntities_.begin(), aliveEntities_.end(), [&](Entity alive) {
        return alive.id == entity.id;
    }), aliveEntities_.end());
}

bool World::isAlive(Entity entity) const
{
    if (entity.id >= records_.size()) {
        return false;
    }
    const EntityRecord& record = records_[entity.id];
    return record.alive && record.generation == entity.generation;
}

const std::vector<Entity>& World::entities() const noexcept
{
    return aliveEntities_;
}

std::optional<World::EntitySnapshot> World::snapshotEntity(Entity entity) const
{
    if (!isAlive(entity)) {
        return std::nullopt;
    }

    EntitySnapshot snapshot{};
    snapshot.entity = entity;

    const EntityRecord& rec = records_[entity.id];
    if (rec.archetypeId != kInvalidArchetype) {
        const Archetype& arch = archetypes_[rec.archetypeId];
        const Chunk& chunk = arch.chunks[rec.chunkIndex];
        snapshot.hot.reserve(arch.key.signature.size());
        for (size_t col = 0; col < arch.key.signature.size(); ++col) {
            const ComponentTypeId typeId = arch.key.signature[col];
            const ComponentMeta& meta = hotComponentMeta_[typeId];
            const std::byte* src = static_cast<const std::byte*>(componentPtr(chunk, col, rec.row, meta.size));
            EntitySnapshot::HotComponentSnapshot comp{};
            comp.typeId = typeId;
            comp.size = meta.size;
            comp.align = meta.align;
            comp.destroy = meta.destroy;
            comp.storage.resize(meta.size + meta.align);
            void* raw = comp.storage.data();
            size_t space = comp.storage.size();
            void* dst = std::align(meta.align, meta.size, raw, space);
            if (dst == nullptr) {
                throw std::runtime_error("World::snapshotEntity failed to align snapshot storage");
            }
            meta.copyConstruct(dst, src);
            snapshot.hot.push_back(std::move(comp));
        }
    }

    for (const auto& [typeKey, pool] : coldPools_) {
        if (auto value = pool->snapshot(entity); value.has_value()) {
            snapshot.cold.insert_or_assign(typeKey, std::move(*value));
        }
    }

    return snapshot;
}

void World::restoreEntity(const EntitySnapshot& snapshot)
{
    if (snapshot.entity.id >= records_.size()) {
        records_.resize(snapshot.entity.id + 1);
    }

    EntityRecord& rec = records_[snapshot.entity.id];
    if (rec.alive) {
        return;
    }

    rec.alive = true;
    rec.generation = snapshot.entity.generation;
    rec.archetypeId = kInvalidArchetype;
    rec.chunkIndex = kInvalidChunk;
    rec.row = 0;

    freeList_.erase(std::remove(freeList_.begin(), freeList_.end(), snapshot.entity.id), freeList_.end());
    aliveEntities_.push_back(snapshot.entity);

    for (const auto& hot : snapshot.hot) {
        const ComponentMeta& meta = hotComponentMeta_[hot.typeId];
        void* raw = const_cast<std::byte*>(hot.storage.data());
        size_t space = hot.storage.size();
        void* src = std::align(meta.align, meta.size, raw, space);
        if (src == nullptr) {
            throw std::runtime_error("World::restoreEntity failed to align snapshot storage");
        }
        emplaceHotComponentCloned(snapshot.entity, hot.typeId, src);
    }

    for (const auto& [typeKey, value] : snapshot.cold) {
        auto it = coldPools_.find(typeKey);
        if (it != coldPools_.end()) {
            it->second->restore(snapshot.entity, value);
        }
    }
}


const World::QueryPlan& World::queryPlanDynamic(
    std::vector<uint32_t> includeTypes,
    std::vector<ComponentAccess> includeAccesses,
    std::vector<uint32_t> excludeTypes,
    std::vector<uint32_t> optionalTypes) const
{
    std::vector<IncludeTerm> includeTerms{};
    includeTerms.reserve(includeTypes.size());
    for (size_t i = 0; i < includeTypes.size(); ++i) {
        includeTerms.push_back(IncludeTerm{ .type = includeTypes[i], .access = includeAccesses[i] });
    }

    std::ranges::sort(includeTerms, {}, &IncludeTerm::type);

    std::vector<IncludeTerm> mergedTerms{};
    mergedTerms.reserve(includeTerms.size());
    for (const IncludeTerm& term : includeTerms) {
        if (!mergedTerms.empty() && mergedTerms.back().type == term.type) {
            if (term.access == ComponentAccess::ReadWrite) {
                mergedTerms.back().access = ComponentAccess::ReadWrite;
            }
            continue;
        }
        mergedTerms.push_back(term);
    }

    includeTypes.clear();
    includeAccesses.clear();
    includeTypes.reserve(mergedTerms.size());
    includeAccesses.reserve(mergedTerms.size());
    for (const IncludeTerm& term : mergedTerms) {
        includeTypes.push_back(term.type);
        includeAccesses.push_back(term.access);
    }

    if (includeTypes.empty()) {
        static QueryPlan empty{};
        return empty;
    }

    canonicalizeTypes(excludeTypes);
    canonicalizeTypes(optionalTypes);

    const QueryKey key{ includeTypes, excludeTypes, optionalTypes, includeAccesses };
    {
        std::shared_lock lock(queryPlansMutex_);
        auto it = queryPlans_.find(key);
        if (it != queryPlans_.end() && it->second.archetypeEpoch == archetypeEpoch_) {
            return it->second;
        }
    }

    QueryPlan rebuilt{};
    rebuilt.includeTypes = includeTypes;
    rebuilt.excludeTypes = excludeTypes;
    rebuilt.optionalTypes = optionalTypes;
    rebuilt.includeAccesses = includeAccesses;
    rebuilt.archetypeEpoch = archetypeEpoch_;

    for (uint32_t archetypeId = 0; archetypeId < archetypes_.size(); ++archetypeId) {
        const Archetype& archetype = archetypes_[archetypeId];
        if (!archetype.matchesAll(includeTypes)) {
            continue;
        }
        if (!excludeTypes.empty() && archetype.matchesAny(excludeTypes)) {
            continue;
        }

        std::vector<QueryPlan::ColumnRemap> remap{};
        remap.reserve(includeTypes.size());
        for (size_t i = 0; i < includeTypes.size(); ++i) {
            const ComponentTypeId typeId = includeTypes[i];
            const auto colIt = archetype.columnByType.find(typeId);
            assert(colIt != archetype.columnByType.end());
            remap.push_back(QueryPlan::ColumnRemap{
                .componentType = typeId,
                .access = includeAccesses[i],
                .columnIndex = colIt->second,
                .componentSize = hotComponentMeta_[typeId].size
            });
        }

        rebuilt.matchingArchetypes.push_back(archetypeId);
        rebuilt.remapsByArchetype.push_back(std::move(remap));

        std::vector<std::optional<QueryPlan::ColumnRemap>> optionalRemaps{};
        optionalRemaps.reserve(optionalTypes.size());
        for (uint32_t optionalType : optionalTypes) {
            const auto colIt = archetype.columnByType.find(optionalType);
            if (colIt == archetype.columnByType.end()) {
                optionalRemaps.push_back(std::nullopt);
                continue;
            }
            optionalRemaps.push_back(QueryPlan::ColumnRemap{
                .componentType = optionalType,
                .access = ComponentAccess::ReadOnly,
                .columnIndex = colIt->second,
                .componentSize = hotComponentMeta_[optionalType].size
            });
        }
        rebuilt.optionalRemapsByArchetype.push_back(std::move(optionalRemaps));
    }

    std::unique_lock lock(queryPlansMutex_);
    auto [newIt, _] = queryPlans_.insert_or_assign(key, std::move(rebuilt));
    return newIt->second;
}

void World::removeHotComponent(Entity entity, ComponentTypeId typeId)
{
    if (!mutationEnabled_) {
        throw std::runtime_error("World::removeHotComponent mutation is disabled during staged read pass");
    }

    EntityRecord& rec = records_[entity.id];
    if (rec.archetypeId == kInvalidArchetype) {
        return;
    }

    const Archetype& src = archetypes_[rec.archetypeId];
    if (!src.columnByType.contains(typeId)) {
        return;
    }

    std::vector<ComponentTypeId> signature = src.key.signature;
    signature.erase(std::remove(signature.begin(), signature.end(), typeId), signature.end());

    const HotInsertResult dst = moveEntityToArchetype(entity, signature, UINT32_MAX, nullptr);
    rec.archetypeId = dst.archetypeId;
    rec.chunkIndex = dst.chunkIndex;
    rec.row = dst.row;
    bumpVersion(typeId);
}

void* World::getHotComponentRaw(Entity entity, ComponentTypeId typeId)
{
    EntityRecord& rec = records_[entity.id];
    if (rec.archetypeId == kInvalidArchetype) {
        return nullptr;
    }

    Archetype& arch = archetypes_[rec.archetypeId];
    auto colIt = arch.columnByType.find(typeId);
    if (colIt == arch.columnByType.end()) {
        return nullptr;
    }

    Chunk& chunk = arch.chunks[rec.chunkIndex];
    return componentPtr(chunk, colIt->second, rec.row, hotComponentMeta_[typeId].size);
}

World::HotInsertResult World::moveEntityToArchetype(Entity entity,
    const std::vector<ComponentTypeId>& signature,
    ComponentTypeId insertedType,
    const void* insertedComponent)
{
    const EntityRecord oldRecord = records_[entity.id];
    const uint32_t dstArchetypeId = archetypeForSignature(signature);
    Archetype& dstArch = archetypes_[dstArchetypeId];

    constexpr size_t kChunkCapacity = 128;
    if (dstArch.chunks.empty() || dstArch.chunks.back().count >= kChunkCapacity) {
        Chunk newChunk{ kChunkCapacity };
        newChunk.columns.reserve(dstArch.key.signature.size());
        newChunk.columnVersions.reserve(dstArch.key.signature.size());
        const size_t dirtyWords = (kChunkCapacity + 63) / 64;
        for (const ComponentTypeId id : dstArch.key.signature) {
            const ComponentMeta& meta = hotComponentMeta_[id];
            newChunk.columns.push_back(AlignedColumn::create(kChunkCapacity * meta.size, meta.align));
            newChunk.columnVersions.push_back(1);
            newChunk.chunkDirtyEpochByColumn.push_back(0);
            newChunk.dirtyRowsByColumn.push_back(std::vector<uint64_t>(dirtyWords, 0));
            newChunk.rowDirtyEpochByColumn.push_back(std::vector<uint64_t>(kChunkCapacity, 0));
        }
        dstArch.chunks.push_back(std::move(newChunk));
    }

    const uint32_t dstChunkIndex = static_cast<uint32_t>(dstArch.chunks.size() - 1);
    Chunk& dstChunk = dstArch.chunks.back();
    const uint32_t dstRow = static_cast<uint32_t>(dstChunk.count);
    dstChunk.entities[dstRow] = entity;

    const Archetype* oldArch = nullptr;
    const Chunk* oldChunk = nullptr;
    if (oldRecord.archetypeId != kInvalidArchetype) {
        oldArch = &archetypes_[oldRecord.archetypeId];
        oldChunk = &archetypes_[oldRecord.archetypeId].chunks[oldRecord.chunkIndex];
    }

    for (size_t dstCol = 0; dstCol < dstArch.key.signature.size(); ++dstCol) {
        const ComponentTypeId typeId = dstArch.key.signature[dstCol];
        const ComponentMeta& meta = hotComponentMeta_[typeId];
        void* dstPtr = componentPtr(dstChunk, dstCol, dstRow, meta.size);

        bool initialized = false;
        if (oldArch != nullptr) {
            auto oldColIt = oldArch->columnByType.find(typeId);
            if (oldColIt != oldArch->columnByType.end()) {
                const void* srcPtr = componentPtr(*oldChunk, oldColIt->second, oldRecord.row, meta.size);
                meta.copyConstruct(dstPtr, srcPtr);
                initialized = true;
            }
        }

        if (!initialized && typeId == insertedType && insertedComponent != nullptr) {
            meta.copyConstruct(dstPtr, insertedComponent);
            initialized = true;
        }

        if (!initialized) {
            throw std::runtime_error("World::moveEntityToArchetype missing source data");
        }
    }

    dstChunk.count += 1;
    dstChunk.structuralVersion += 1;

    if (oldRecord.archetypeId != kInvalidArchetype) {
        eraseFromArchetype(oldRecord.archetypeId, oldRecord.chunkIndex, oldRecord.row);
    }

    return HotInsertResult{ .archetypeId = dstArchetypeId, .chunkIndex = dstChunkIndex, .row = dstRow };
}

uint32_t World::archetypeForSignature(const std::vector<ComponentTypeId>& signature)
{
    ArchetypeKey key{ .signature = signature };
    auto it = archetypeByKey_.find(key);
    if (it != archetypeByKey_.end()) {
        return it->second;
    }

    Archetype arch{};
    arch.key = key;
    for (size_t i = 0; i < key.signature.size(); ++i) {
        arch.columnByType.insert_or_assign(key.signature[i], i);
    }

    const uint32_t id = static_cast<uint32_t>(archetypes_.size());
    archetypes_.push_back(std::move(arch));
    archetypeByKey_.insert_or_assign(std::move(key), id);
    archetypeEpoch_ += 1;
    {
        std::unique_lock lock(queryPlansMutex_);
        queryPlans_.clear();
    }
    return id;
}

void World::eraseFromArchetype(uint32_t archetypeId, uint32_t chunkIndex, uint32_t row)
{
    Archetype& arch = archetypes_[archetypeId];
    Chunk& chunk = arch.chunks[chunkIndex];
    assert(chunk.count > 0);
    const uint32_t last = static_cast<uint32_t>(chunk.count - 1);

    for (size_t col = 0; col < arch.key.signature.size(); ++col) {
        const ComponentTypeId typeId = arch.key.signature[col];
        const ComponentMeta& meta = hotComponentMeta_[typeId];
        void* rowPtr = componentPtr(chunk, col, row, meta.size);

        if (row != last) {
            void* lastPtr = componentPtr(chunk, col, last, meta.size);
            meta.moveAssignOrConstruct(rowPtr, lastPtr);
        }
        else {
            meta.destroy(rowPtr);
        }
    }

    if (row != last) {
        const Entity moved = chunk.entities[last];
        chunk.entities[row] = moved;
        EntityRecord& movedRec = records_[moved.id];
        movedRec.chunkIndex = chunkIndex;
        movedRec.row = row;
    }

    chunk.count -= 1;
    chunk.structuralVersion += 1;
}

void World::validateAlive(Entity entity) const
{
    if (!isAlive(entity)) {
        throw std::runtime_error("Operation on invalid entity");
    }
}

void World::beginFrame()
{
    frameActive_ = true;
    mutationEnabled_ = true;
    frameEpoch_ += 1;
    playbackPhase(StructuralPlaybackPhase::PreSim);
}

void World::playbackPhase(StructuralPlaybackPhase phase)
{
    if (structuralBuffer_ != nullptr) {
        structuralBuffer_->playback(*this, phase);
    }
}

void World::endFrame()
{
    endSystemWriteScope();
    playbackPhase(StructuralPlaybackPhase::EndFrame);
    mutationEnabled_ = true;
    frameActive_ = false;
}

void World::beginSystemWriteScope()
{
    if (writeScopeDepth_ == 0) {
        pendingDirtyVersionBumps_.clear();
    }
    writeScopeDepth_ += 1;
}

void World::endSystemWriteScope()
{
    if (writeScopeDepth_ == 0) {
        return;
    }

    writeScopeDepth_ -= 1;
    if (writeScopeDepth_ != 0) {
        return;
    }

    for (uint64_t key : pendingDirtyVersionBumps_) {
        const uint32_t archetypeId = static_cast<uint32_t>((key >> 40u) & 0xFFFFFu);
        const uint32_t chunkIndex = static_cast<uint32_t>((key >> 20u) & 0xFFFFFu);
        const uint32_t typeId = static_cast<uint32_t>(key & 0xFFFFFu);
        bumpVersion(typeId);
        bumpChunkVersion(archetypeId, chunkIndex, typeId);
    }
    pendingDirtyVersionBumps_.clear();
}

void World::setStructuralCommandBuffer(StructuralCommandBuffer* commandBuffer)
{
    structuralBuffer_ = commandBuffer;
}

uint64_t World::componentVersion(uint32_t typeId) const noexcept
{
    if (typeId >= hotComponentMeta_.size()) {
        return 0;
    }
    return hotComponentMeta_[typeId].version;
}

void World::bumpVersion(ComponentTypeId typeId)
{
    if (typeId < hotComponentMeta_.size()) {
        hotComponentMeta_[typeId].version += 1;
    }
}

void World::bumpChunkVersion(uint32_t archetypeId, uint32_t chunkIndex, ComponentTypeId typeId)
{
    if (archetypeId == kInvalidArchetype || chunkIndex == kInvalidChunk) {
        return;
    }
    Archetype& arch = archetypes_[archetypeId];
    auto colIt = arch.columnByType.find(typeId);
    if (colIt == arch.columnByType.end()) {
        return;
    }
    Chunk& chunk = arch.chunks[chunkIndex];
    chunk.columnVersions[colIt->second] += 1;
}

void World::emplaceHotComponentCloned(Entity entity, ComponentTypeId typeId, const void* componentData)
{
    EntityRecord& record = records_[entity.id];
    if (record.archetypeId != kInvalidArchetype) {
        Archetype& arch = archetypes_[record.archetypeId];
        auto colIt = arch.columnByType.find(typeId);
        if (colIt != arch.columnByType.end()) {
            Chunk& chunk = arch.chunks[record.chunkIndex];
            void* dst = componentPtr(chunk, colIt->second, record.row, hotComponentMeta_[typeId].size);
            const ComponentMeta& meta = hotComponentMeta_[typeId];
            meta.destroy(dst);
            meta.copyConstruct(dst, componentData);
            bumpVersion(typeId);
            bumpChunkVersion(record.archetypeId, record.chunkIndex, typeId);
            return;
        }
    }

    std::vector<ComponentTypeId> signature{};
    if (record.archetypeId != kInvalidArchetype) {
        signature = archetypes_[record.archetypeId].key.signature;
    }
    signature.push_back(typeId);
    canonicalizeTypes(signature);

    const HotInsertResult inserted = moveEntityToArchetype(entity, signature, typeId, componentData);
    record.archetypeId = inserted.archetypeId;
    record.chunkIndex = inserted.chunkIndex;
    record.row = inserted.row;
    bumpVersion(typeId);
    bumpChunkVersion(inserted.archetypeId, inserted.chunkIndex, typeId);
}


void World::markChunkComponentDirty(uint32_t archetypeId, uint32_t chunkIndex, ComponentTypeId typeId, uint32_t row)
{
    if (archetypeId == kInvalidArchetype || chunkIndex == kInvalidChunk) {
        return;
    }
    Archetype& arch = archetypes_[archetypeId];
    auto colIt = arch.columnByType.find(typeId);
    if (colIt == arch.columnByType.end()) {
        return;
    }
    Chunk& chunk = arch.chunks[chunkIndex];
    const size_t word = row / 64;
    const uint64_t bit = uint64_t{1} << (row % 64);
    if (word < chunk.dirtyRowsByColumn[colIt->second].size()) {
        chunk.dirtyRowsByColumn[colIt->second][word] |= bit;
    }
    if (row < chunk.rowDirtyEpochByColumn[colIt->second].size()) {
        chunk.rowDirtyEpochByColumn[colIt->second][row] = frameEpoch_;
    }

    chunk.chunkDirtyEpochByColumn[colIt->second] = frameEpoch_;
    pendingDirtyVersionBumps_.insert(dirtyKey(archetypeId, chunkIndex, typeId));
}

void World::markComponentDirtyByEntity(Entity entity, ComponentTypeId typeId)
{
    if (!isAlive(entity)) {
        return;
    }
    const EntityRecord& rec = records_[entity.id];
    if (rec.archetypeId == kInvalidArchetype) {
        return;
    }
    markChunkComponentDirty(rec.archetypeId, rec.chunkIndex, typeId, rec.row);
}

uint64_t World::chunkVersion(ChunkHandle handle, uint32_t componentType) const
{
    if (handle.archetypeId >= archetypes_.size()) {
        return 0;
    }
    const Archetype& arch = archetypes_[handle.archetypeId];
    if (handle.chunkIndex >= arch.chunks.size()) {
        return 0;
    }
    const auto colIt = arch.columnByType.find(componentType);
    if (colIt == arch.columnByType.end()) {
        return 0;
    }
    return arch.chunks[handle.chunkIndex].columnVersions[colIt->second];
}

uint64_t World::chunkStructuralVersion(ChunkHandle handle) const
{
    if (handle.archetypeId >= archetypes_.size()) {
        return 0;
    }
    const Archetype& arch = archetypes_[handle.archetypeId];
    if (handle.chunkIndex >= arch.chunks.size()) {
        return 0;
    }
    return arch.chunks[handle.chunkIndex].structuralVersion;
}

uint64_t World::chunkDirtyEpoch(ChunkHandle handle, uint32_t componentType) const
{
    if (handle.archetypeId >= archetypes_.size()) {
        return 0;
    }
    const Archetype& arch = archetypes_[handle.archetypeId];
    if (handle.chunkIndex >= arch.chunks.size()) {
        return 0;
    }
    const auto colIt = arch.columnByType.find(componentType);
    if (colIt == arch.columnByType.end()) {
        return 0;
    }
    return arch.chunks[handle.chunkIndex].chunkDirtyEpochByColumn[colIt->second];
}
