#pragma once

#include <ecs/AccessDeclaration.h>
#include <ecs/World.h>

#include <stdexcept>

class WorldReadView {
public:
    explicit WorldReadView(const World& world, const AccessDeclaration* access = nullptr)
        : world_(world)
        , access_(access)
    {
    }

    template <typename... Ts>
    [[nodiscard]] auto query() const
    {
        validateRead<Ts...>();
        return world_.template query<Ts...>();
    }

private:
    template <typename T>
    void validateOne() const
    {
        using Decayed = std::remove_cvref_t<T>;
        if constexpr (ComponentResidencyTrait<Decayed>::value == ComponentResidency::ColdSparse) {
            return;
        }
        if (access_ == nullptr) {
            return;
        }
        const auto id = world_.template componentTypeId<Decayed>();
        if (!id.has_value()) {
            return;
        }
        if (!access_->read.contains(*id) && !access_->write.contains(*id)) {
            throw std::runtime_error("WorldReadView: undeclared read access");
        }
    }

    template <typename... Ts>
    void validateRead() const { (validateOne<Ts>(), ...); }

    const World& world_;
    const AccessDeclaration* access_;
};

class WorldWriteView {
public:
    explicit WorldWriteView(World& world, const AccessDeclaration* access = nullptr)
        : world_(world)
        , access_(access)
    {
    }

    template <typename... Ts>
    [[nodiscard]] auto query()
    {
        validateRead<Ts...>();
        return world_.template query<Ts...>();
    }

    template <typename T, typename... Args>
    T& emplaceComponent(Entity entity, Args&&... args)
    {
        validateWrite<T>();
        return world_.template emplaceComponent<T>(entity, std::forward<Args>(args)...);
    }

    template <typename T>
    void removeComponent(Entity entity)
    {
        validateWrite<T>();
        world_.template removeComponent<T>(entity);
    }

private:
    template <typename T>
    void validateWrite() const
    {
        using Decayed = std::remove_cvref_t<T>;
        if constexpr (ComponentResidencyTrait<Decayed>::value == ComponentResidency::ColdSparse) {
            return;
        }
        if (access_ == nullptr) {
            return;
        }
        const auto id = world_.template componentTypeId<Decayed>();
        if (!id.has_value()) {
            return;
        }
        if (!access_->write.contains(*id)) {
            throw std::runtime_error("WorldWriteView: undeclared write access");
        }
    }

    template <typename T>
    void validateOneRead() const
    {
        using Decayed = std::remove_cvref_t<T>;
        if constexpr (ComponentResidencyTrait<Decayed>::value == ComponentResidency::ColdSparse) {
            return;
        }
        if (access_ == nullptr) {
            return;
        }
        const auto id = world_.template componentTypeId<Decayed>();
        if (!id.has_value()) {
            return;
        }
        if (!access_->read.contains(*id) && !access_->write.contains(*id)) {
            throw std::runtime_error("WorldWriteView: undeclared read access");
        }
    }

    template <typename... Ts>
    void validateRead() const { (validateOneRead<Ts>(), ...); }

    World& world_;
    const AccessDeclaration* access_;
};
