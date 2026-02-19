#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <tuple>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ecs {

struct Entity {
    uint32_t index{ 0 };
    uint32_t generation{ 0 };

    [[nodiscard]] bool operator==(const Entity&) const = default;
};

class World {
public:
    class ComponentStoreBase {
    public:
        virtual ~ComponentStoreBase() = default;
        virtual void erase(Entity entity) = 0;
        [[nodiscard]] virtual size_t size() const = 0;
    };

    template <typename T>
    class ComponentStore final : public ComponentStoreBase {
    public:
        static constexpr uint32_t kInvalidIndex = std::numeric_limits<uint32_t>::max();

        bool add(Entity entity, T value)
        {
            ensureSparseSize(entity.index + 1);
            const uint32_t sparseIndex = sparse_[entity.index];
            if (sparseIndex != kInvalidIndex) {
                denseData_[sparseIndex] = std::move(value);
                denseEntities_[sparseIndex] = entity;
                return false;
            }

            const uint32_t denseIndex = static_cast<uint32_t>(denseData_.size());
            denseEntities_.push_back(entity);
            denseData_.push_back(std::move(value));
            sparse_[entity.index] = denseIndex;
            return true;
        }

        bool remove(Entity entity)
        {
            if (entity.index >= sparse_.size()) {
                return false;
            }

            const uint32_t denseIndex = sparse_[entity.index];
            if (denseIndex == kInvalidIndex) {
                return false;
            }

            const uint32_t lastDense = static_cast<uint32_t>(denseData_.size() - 1);
            if (denseIndex != lastDense) {
                denseEntities_[denseIndex] = denseEntities_[lastDense];
                denseData_[denseIndex] = std::move(denseData_[lastDense]);
                sparse_[denseEntities_[denseIndex].index] = denseIndex;
            }

            denseEntities_.pop_back();
            denseData_.pop_back();
            sparse_[entity.index] = kInvalidIndex;
            return true;
        }

        [[nodiscard]] bool has(Entity entity) const
        {
            if (entity.index >= sparse_.size()) {
                return false;
            }

            const uint32_t denseIndex = sparse_[entity.index];
            if (denseIndex == kInvalidIndex || denseIndex >= denseEntities_.size()) {
                return false;
            }

            return denseEntities_[denseIndex].generation == entity.generation;
        }

        [[nodiscard]] T* get(Entity entity)
        {
            return has(entity) ? &denseData_[sparse_[entity.index]] : nullptr;
        }

        [[nodiscard]] const T* get(Entity entity) const
        {
            return has(entity) ? &denseData_[sparse_[entity.index]] : nullptr;
        }

        [[nodiscard]] const std::vector<Entity>& entities() const
        {
            return denseEntities_;
        }

        [[nodiscard]] size_t size() const override
        {
            return denseData_.size();
        }

        void reserve(size_t capacity)
        {
            denseEntities_.reserve(capacity);
            denseData_.reserve(capacity);
        }

        void erase(Entity entity) override
        {
            (void)remove(entity);
        }

    private:
        void ensureSparseSize(size_t size)
        {
            if (sparse_.size() < size) {
                sparse_.resize(size, kInvalidIndex);
            }
        }

        std::vector<Entity> denseEntities_{};
        std::vector<T> denseData_{};
        std::vector<uint32_t> sparse_{};
    };

    template <typename... Components>
    class View {
    public:
        explicit View(World* world)
            : world_(world)
        {
        }

        template <typename Fn>
        void each(Fn&& fn)
        {
            if (world_ == nullptr) {
                return;
            }
            world_->forEachSmallest<Components...>(std::forward<Fn>(fn));
        }

    private:
        World* world_{ nullptr };
    };

    template <typename... Components>
    class ConstView {
    public:
        explicit ConstView(const World* world)
            : world_(world)
        {
        }

        template <typename Fn>
        void each(Fn&& fn) const
        {
            if (world_ == nullptr) {
                return;
            }
            world_->forEachSmallestConst<Components...>(std::forward<Fn>(fn));
        }

    private:
        const World* world_{ nullptr };
    };

    [[nodiscard]] Entity create()
    {
        if (!freeList_.empty()) {
            const uint32_t index = freeList_.back();
            freeList_.pop_back();
            alive_[index] = true;
            return Entity{ .index = index, .generation = generations_[index] };
        }

        const uint32_t index = static_cast<uint32_t>(generations_.size());
        generations_.push_back(1);
        alive_.push_back(true);
        return Entity{ .index = index, .generation = generations_[index] };
    }

    void reserveEntities(size_t count)
    {
        generations_.reserve(count);
        alive_.reserve(count);
        freeList_.reserve(count);
    }

    [[nodiscard]] size_t aliveCount() const
    {
        size_t count = 0;
        for (bool alive : alive_) {
            if (alive) {
                count += 1;
            }
        }
        return count;
    }

    bool destroy(Entity entity)
    {
        if (!isAlive(entity)) {
            return false;
        }

        alive_[entity.index] = false;
        generations_[entity.index] += 1;
        freeList_.push_back(entity.index);

        for (auto& [_, store] : componentStores_) {
            store->erase(entity);
        }

        return true;
    }

    [[nodiscard]] bool isAlive(Entity entity) const
    {
        return entity.index < alive_.size() && alive_[entity.index] && generations_[entity.index] == entity.generation;
    }

    template <typename T>
    bool add(Entity entity, T value)
    {
        if (!isAlive(entity)) {
            return false;
        }
        return componentStore<T>().add(entity, std::move(value));
    }

    template <typename T>
    bool remove(Entity entity)
    {
        if (!isAlive(entity)) {
            return false;
        }
        return componentStore<T>().remove(entity);
    }

    template <typename T>
    [[nodiscard]] bool has(Entity entity) const
    {
        if (!isAlive(entity)) {
            return false;
        }
        const auto* store = tryGetStore<T>();
        return store != nullptr && store->has(entity);
    }

    template <typename T>
    [[nodiscard]] T* get(Entity entity)
    {
        if (!isAlive(entity)) {
            return nullptr;
        }
        auto* store = tryGetStore<T>();
        return store != nullptr ? store->get(entity) : nullptr;
    }

    template <typename T>
    [[nodiscard]] const T* get(Entity entity) const
    {
        if (!isAlive(entity)) {
            return nullptr;
        }
        const auto* store = tryGetStore<T>();
        return store != nullptr ? store->get(entity) : nullptr;
    }

    template <typename T>
    void reserve(size_t capacity)
    {
        componentStore<T>().reserve(capacity);
    }

    template <typename... Components>
    [[nodiscard]] View<Components...> view()
    {
        return View<Components...>(this);
    }

    template <typename... Components>
    [[nodiscard]] ConstView<Components...> view() const
    {
        return ConstView<Components...>(this);
    }

    template <typename... Components, typename Fn>
    void each(Fn&& fn)
    {
        view<Components...>().each(std::forward<Fn>(fn));
    }

    template <typename... Components, typename Fn>
    void each(Fn&& fn) const
    {
        view<Components...>().each(std::forward<Fn>(fn));
    }

private:
    template <typename T>
    ComponentStore<T>& componentStore()
    {
        const std::type_index key(typeid(T));
        auto it = componentStores_.find(key);
        if (it == componentStores_.end()) {
            auto inserted = componentStores_.insert_or_assign(key, std::make_unique<ComponentStore<T>>());
            it = inserted.first;
        }
        return *static_cast<ComponentStore<T>*>(it->second.get());
    }

    template <typename T>
    [[nodiscard]] ComponentStore<T>* tryGetStore()
    {
        const std::type_index key(typeid(T));
        auto it = componentStores_.find(key);
        if (it == componentStores_.end()) {
            return nullptr;
        }
        return static_cast<ComponentStore<T>*>(it->second.get());
    }

    template <typename T>
    [[nodiscard]] const ComponentStore<T>* tryGetStore() const
    {
        const std::type_index key(typeid(T));
        auto it = componentStores_.find(key);
        if (it == componentStores_.end()) {
            return nullptr;
        }
        return static_cast<const ComponentStore<T>*>(it->second.get());
    }

    template <typename... Components>
    [[nodiscard]] const ComponentStoreBase* smallestStore() const
    {
        std::array<const ComponentStoreBase*, sizeof...(Components)> stores{
            static_cast<const ComponentStoreBase*>(tryGetStore<Components>())...
        };

        const ComponentStoreBase* smallest = nullptr;
        for (const ComponentStoreBase* store : stores) {
            if (store == nullptr) {
                return nullptr;
            }
            if (smallest == nullptr || store->size() < smallest->size()) {
                smallest = store;
            }
        }
        return smallest;
    }

    template <typename... Components, typename Fn>
    void forEachSmallest(Fn&& fn)
    {
        static_assert(sizeof...(Components) > 0, "forEachSmallest requires components");
        const ComponentStoreBase* smallest = smallestStore<Components...>();
        if (smallest == nullptr) {
            return;
        }

        bool executed = false;
        auto tryExecute = [&](auto tag) {
            using C = typename decltype(tag)::type;
            if (!executed && smallest == static_cast<const ComponentStoreBase*>(tryGetStore<C>())) {
                forEachFromStore<C, Components...>(std::forward<Fn>(fn));
                executed = true;
            }
        };
        (tryExecute(std::type_identity<Components>{}), ...);
    }

    template <typename... Components, typename Fn>
    void forEachSmallestConst(Fn&& fn) const
    {
        static_assert(sizeof...(Components) > 0, "forEachSmallestConst requires components");
        const ComponentStoreBase* smallest = smallestStore<Components...>();
        if (smallest == nullptr) {
            return;
        }

        bool executed = false;
        auto tryExecute = [&](auto tag) {
            using C = typename decltype(tag)::type;
            if (!executed && smallest == static_cast<const ComponentStoreBase*>(tryGetStore<C>())) {
                forEachFromStoreConst<C, Components...>(std::forward<Fn>(fn));
                executed = true;
            }
        };
        (tryExecute(std::type_identity<Components>{}), ...);
    }

    template <typename Primary, typename... Components, typename Fn>
    void forEachFromStore(Fn&& fn)
    {
        const auto* primaryStore = tryGetStore<Primary>();
        if (primaryStore == nullptr) {
            return;
        }

        for (const Entity entity : primaryStore->entities()) {
            if (!isAlive(entity)) {
                continue;
            }
            if ((has<Components>(entity) && ...)) {
                fn(entity, *get<Components>(entity)...);
            }
        }
    }

    template <typename Primary, typename... Components, typename Fn>
    void forEachFromStoreConst(Fn&& fn) const
    {
        const auto* primaryStore = tryGetStore<Primary>();
        if (primaryStore == nullptr) {
            return;
        }

        for (const Entity entity : primaryStore->entities()) {
            if (!isAlive(entity)) {
                continue;
            }
            if ((has<Components>(entity) && ...)) {
                fn(entity, *get<Components>(entity)...);
            }
        }
    }

    std::vector<uint32_t> generations_{};
    std::vector<bool> alive_{};
    std::vector<uint32_t> freeList_{};
    std::unordered_map<std::type_index, std::unique_ptr<ComponentStoreBase>> componentStores_{};
};

} // namespace ecs
