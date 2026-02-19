#pragma once

#include <engine/ecs/Entity.h>

#include <memory>
#include <unordered_map>
#include <utility>

class IComponentStorage {
public:
    virtual ~IComponentStorage() = default;
    virtual void remove(Entity entity) = 0;
};

template <typename T>
class ComponentStorage final : public IComponentStorage {
public:
    template <typename... Args>
    T& emplace(Entity entity, Args&&... args)
    {
        return components_.insert_or_assign(entity.id, T{ std::forward<Args>(args)... }).first->second;
    }

    void remove(Entity entity) override
    {
        components_.erase(entity.id);
    }

    [[nodiscard]] bool has(Entity entity) const
    {
        return components_.contains(entity.id);
    }

    [[nodiscard]] T* get(Entity entity)
    {
        auto it = components_.find(entity.id);
        return it == components_.end() ? nullptr : &it->second;
    }

    [[nodiscard]] const T* get(Entity entity) const
    {
        auto it = components_.find(entity.id);
        return it == components_.end() ? nullptr : &it->second;
    }

private:
    std::unordered_map<uint32_t, T> components_{};
};
