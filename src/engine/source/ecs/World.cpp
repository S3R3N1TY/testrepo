#include <ecs/StructuralCommandBuffer.h>
#include <ecs/World.h>

#include <algorithm>

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


const World::QueryPlan& World::queryPlanDynamic(
    std::vector<uint32_t> includeTypes,
    std::vector<ComponentAccess> includeAccesses,
    std::vector<uint32_t> excludeTypes,
    std::vector<uint32_t> optionalTypes) const
{
    if (includeTypes.empty()) {
        static QueryPlan empty{};
        return empty;
    }

    canonicalizeTypes(includeTypes);
    canonicalizeTypes(excludeTypes);
    canonicalizeTypes(optionalTypes);

    const QueryKey key{ includeTypes, excludeTypes, optionalTypes, includeAccesses };
    auto it = queryPlans_.find(key);
    if (it != queryPlans_.end() && it->second.archetypeEpoch == archetypeEpoch_) {
        return it->second;
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
    }

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
        for (const ComponentTypeId id : dstArch.key.signature) {
            const ComponentMeta& meta = hotComponentMeta_[id];
            newChunk.columns.push_back(AlignedColumn::create(kChunkCapacity * meta.size, meta.align));
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
    queryPlans_.clear();
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
    playbackPhase(StructuralPlaybackPhase::EndFrame);
    mutationEnabled_ = true;
    frameActive_ = false;
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
