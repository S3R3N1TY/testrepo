#include <ecs/World.h>

#include <algorithm>

Entity World::createEntity()
{
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

    const Entity entity{ .id = id, .generation = record.generation };
    aliveEntities_.push_back(entity);
    return entity;
}

void World::destroyEntity(Entity entity)
{
    if (!isAlive(entity)) {
        return;
    }

    records_[entity.id].alive = false;
    records_[entity.id].generation += 1;
    freeList_.push_back(entity.id);

    aliveEntities_.erase(
        std::remove_if(aliveEntities_.begin(), aliveEntities_.end(), [&](const Entity alive) {
            return alive.id == entity.id;
        }),
        aliveEntities_.end());

    for (auto& [_, storage] : storages_) {
        storage->remove(entity);
    }
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

void World::validateAlive(Entity entity) const
{
    if (!isAlive(entity)) {
        throw std::runtime_error("Operation on invalid entity");
    }
}
