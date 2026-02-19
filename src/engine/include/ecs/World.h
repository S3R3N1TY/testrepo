#pragma once

#include <ecs/ComponentStorage.h>
#include <ecs/Entity.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <typeindex>
#include <unordered_map>
#include <vector>

template <typename... Ts>
class Query;
template <typename... Ts>
class ConstQuery;

class World {
public:
    World() = default;

    [[nodiscard]] Entity createEntity();
    void destroyEntity(Entity entity);

    [[nodiscard]] bool isAlive(Entity entity) const;
    [[nodiscard]] const std::vector<Entity>& entities() const noexcept;

    template <typename T, typename... Args>
    T& emplaceComponent(Entity entity, Args&&... args)
    {
        validateAlive(entity);
        auto& storage = storageFor<T>();
        return storage.emplace(entity, std::forward<Args>(args)...);
    }

    template <typename T>
    bool hasComponent(Entity entity) const
    {
        if (!isAlive(entity)) {
            return false;
        }
        const auto* storage = tryStorageFor<T>();
        return storage != nullptr && storage->has(entity);
    }

    template <typename T>
    T* getComponent(Entity entity)
    {
        if (!isAlive(entity)) {
            return nullptr;
        }
        auto* storage = tryStorageFor<T>();
        return storage == nullptr ? nullptr : storage->get(entity);
    }

    template <typename T>
    const T* getComponent(Entity entity) const
    {
        if (!isAlive(entity)) {
            return nullptr;
        }
        const auto* storage = tryStorageFor<T>();
        return storage == nullptr ? nullptr : storage->get(entity);
    }

    template <typename... Ts>
    [[nodiscard]] Query<Ts...> query();

    template <typename... Ts>
    [[nodiscard]] ConstQuery<Ts...> query() const;

private:
    struct EntityRecord {
        uint32_t generation{ 0 };
        bool alive{ false };
    };

    void validateAlive(Entity entity) const;

    template <typename T>
    ComponentStorage<T>& storageFor()
    {
        const std::type_index key{ typeid(T) };
        auto it = storages_.find(key);
        if (it == storages_.end()) {
            auto storage = std::make_unique<ComponentStorage<T>>();
            auto [newIt, inserted] = storages_.emplace(key, std::move(storage));
            (void)inserted;
            it = newIt;
        }
        return *static_cast<ComponentStorage<T>*>(it->second.get());
    }

    template <typename T>
    ComponentStorage<T>* tryStorageFor()
    {
        const std::type_index key{ typeid(T) };
        auto it = storages_.find(key);
        return it == storages_.end() ? nullptr : static_cast<ComponentStorage<T>*>(it->second.get());
    }

    template <typename T>
    const ComponentStorage<T>* tryStorageFor() const
    {
        const std::type_index key{ typeid(T) };
        auto it = storages_.find(key);
        return it == storages_.end() ? nullptr : static_cast<const ComponentStorage<T>*>(it->second.get());
    }

    std::vector<EntityRecord> records_{};
    std::vector<uint32_t> freeList_{};
    std::vector<Entity> aliveEntities_{};
    std::unordered_map<std::type_index, std::unique_ptr<IComponentStorage>> storages_{};
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
