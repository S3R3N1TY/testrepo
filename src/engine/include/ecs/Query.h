#pragma once

#include <ecs/Entity.h>

#include <tuple>
#include <type_traits>

class World;

template <typename... Ts>
class Query {
public:
    explicit Query(World& world)
        : world_(world)
    {
    }

    template <typename Fn>
    void each(Fn&& fn)
    {
        for (const Entity entity : world_.entities()) {
            auto tuple = std::tuple<Ts*...>{ world_.template getComponent<Ts>(entity)... };
            if ((... && (std::get<Ts*>(tuple) != nullptr))) {
                std::forward<Fn>(fn)(entity, *std::get<Ts*>(tuple)...);
            }
        }
    }

private:
    World& world_;
};

template <typename... Ts>
class ConstQuery {
public:
    explicit ConstQuery(const World& world)
        : world_(world)
    {
    }

    template <typename Fn>
    void each(Fn&& fn) const
    {
        for (const Entity entity : world_.entities()) {
            auto tuple = std::tuple<const Ts*...>{ world_.template getComponent<Ts>(entity)... };
            if ((... && (std::get<const Ts*>(tuple) != nullptr))) {
                std::forward<Fn>(fn)(entity, *std::get<const Ts*>(tuple)...);
            }
        }
    }

private:
    const World& world_;
};
