#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <iostream>
#include <exception>
#include <string_view>
#include <optional>
#include <tuple>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ecs {

struct Entity {
    uint32_t index{ 0 };
    uint32_t generation{ 0 };

    [[nodiscard]] bool operator==(const Entity&) const = default;
};

class WorldCommandBuffer;

class World {
public:
    struct SystemAccessDeclaration {
        std::string_view systemName{};
        const std::vector<std::type_index>* reads{ nullptr };
        const std::vector<std::type_index>* writes{ nullptr };
    };

    struct DeferredReplayMetrics {
        uint32_t queuedAdds{ 0 };
        uint32_t queuedRemoves{ 0 };
        uint32_t queuedDestroys{ 0 };
        uint32_t queuedCreates{ 0 };
        uint32_t appliedAdds{ 0 };
        uint32_t appliedRemoves{ 0 };
        uint32_t appliedDestroys{ 0 };
        uint32_t noopCommands{ 0 };
    };

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
        assertStructuralMutationAllowed("destroy");
        return destroyImmediateNoAccessCheck(entity);
    }

    void beginFrame() {}

    void endFrame()
    {
#ifndef NDEBUG
        if (lastDeferredReplayMetrics_.queuedAdds == 0 && lastDeferredReplayMetrics_.queuedRemoves == 0
            && lastDeferredReplayMetrics_.queuedDestroys == 0 && lastDeferredReplayMetrics_.queuedCreates == 0) {
            return;
        }
        std::cout << "ECS deferred replay: queued(add=" << lastDeferredReplayMetrics_.queuedAdds
                  << ", remove=" << lastDeferredReplayMetrics_.queuedRemoves
                  << ", destroy=" << lastDeferredReplayMetrics_.queuedDestroys
                  << ", create=" << lastDeferredReplayMetrics_.queuedCreates
                  << ") applied(add=" << lastDeferredReplayMetrics_.appliedAdds
                  << ", remove=" << lastDeferredReplayMetrics_.appliedRemoves
                  << ", destroy=" << lastDeferredReplayMetrics_.appliedDestroys
                  << ") noops=" << lastDeferredReplayMetrics_.noopCommands << std::endl;
#endif
    }

    void applyDeferredCommands(WorldCommandBuffer& merged);

    template <typename T>
    bool addImmediateNoAccessCheck(Entity entity, T&& value)
    {
        using Component = std::remove_cv_t<T>;
        if (!isAlive(entity)) {
            return false;
        }
        return componentStore<Component>().add(entity, std::forward<T>(value));
    }

    template <typename T>
    bool removeImmediateNoAccessCheck(Entity entity)
    {
        using Component = std::remove_cv_t<T>;
        if (!isAlive(entity)) {
            return false;
        }
        return componentStore<Component>().remove(entity);
    }

    bool destroyImmediateNoAccessCheck(Entity entity)
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
        using Component = std::remove_cv_t<T>;
        assertWriteAccess<Component>();
        assertStructuralMutationAllowed("add");
        if (!isAlive(entity)) {
            return false;
        }
        return addImmediateNoAccessCheck<Component>(entity, std::move(value));
    }

    template <typename T>
    bool remove(Entity entity)
    {
        using Component = std::remove_cv_t<T>;
        assertWriteAccess<Component>();
        assertStructuralMutationAllowed("remove");
        if (!isAlive(entity)) {
            return false;
        }
        return removeImmediateNoAccessCheck<Component>(entity);
    }

    template <typename T>
    [[nodiscard]] bool has(Entity entity) const
    {
        using Component = std::remove_cv_t<T>;
        if (!isAlive(entity)) {
            return false;
        }
        const auto* store = tryGetStore<Component>();
        return store != nullptr && store->has(entity);
    }

    template <typename T>
    [[nodiscard]] T* get(Entity entity)
    {
        using Component = std::remove_cv_t<T>;
        assertWriteAccess<Component>();
        if (!isAlive(entity)) {
            return nullptr;
        }
        auto* store = tryGetStore<Component>();
        return store != nullptr ? store->get(entity) : nullptr;
    }

    template <typename T>
    [[nodiscard]] const T* get(Entity entity) const
    {
        using Component = std::remove_cv_t<T>;
        assertReadAccess<Component>();
        if (!isAlive(entity)) {
            return nullptr;
        }
        const auto* store = tryGetStore<Component>();
        return store != nullptr ? store->get(entity) : nullptr;
    }

    void installSystemAccessContext(const SystemAccessDeclaration& declaration)
    {
        SystemAccessContext context{};
        context.systemName = declaration.systemName;
        if (declaration.reads != nullptr) {
            context.reads.insert(declaration.reads->begin(), declaration.reads->end());
        }
        if (declaration.writes != nullptr) {
            context.writes.insert(declaration.writes->begin(), declaration.writes->end());
        }
        currentSystemAccessContext() = std::move(context);
    }

    void clearSystemAccessContext()
    {
        currentSystemAccessContext().reset();
    }

    template <typename T>
    void reserve(size_t capacity)
    {
        componentStore<std::remove_cv_t<T>>().reserve(capacity);
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
    struct SystemAccessContext {
        std::string_view systemName{};
        std::unordered_set<std::type_index> reads{};
        std::unordered_set<std::type_index> writes{};
    };

    static void assertStructuralMutationAllowed([[maybe_unused]] const char* operation)
    {
        const auto& context = currentSystemAccessContext();
        if (!context.has_value()) {
            return;
        }
        assert(false && "Structural world mutation during system execution is forbidden; queue through WorldCommandBuffer");
        std::terminate();
    }

    static std::optional<SystemAccessContext>& currentSystemAccessContext()
    {
        static thread_local std::optional<SystemAccessContext> context{};
        return context;
    }

    template <typename T>
    static void assertReadAccess()
    {
#ifndef NDEBUG
        auto& context = currentSystemAccessContext();
        if (!context.has_value()) {
            return;
        }
        const std::type_index type(typeid(std::remove_cv_t<T>));
        const bool readable = context->reads.contains(type) || context->writes.contains(type);
        assert(readable && "System read undeclared component type");
#endif
    }

    template <typename T>
    static void assertWriteAccess()
    {
#ifndef NDEBUG
        auto& context = currentSystemAccessContext();
        if (!context.has_value()) {
            return;
        }
        const std::type_index type(typeid(std::remove_cv_t<T>));
        assert(context->writes.contains(type) && "System wrote undeclared component type");
#endif
    }

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
            static_cast<const ComponentStoreBase*>(tryGetStore<std::remove_const_t<Components>>())...
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
            if (!executed && smallest == static_cast<const ComponentStoreBase*>(tryGetStore<std::remove_const_t<C>>())) {
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
            if (!executed && smallest == static_cast<const ComponentStoreBase*>(tryGetStore<std::remove_const_t<C>>())) {
                forEachFromStoreConst<C, Components...>(std::forward<Fn>(fn));
                executed = true;
            }
        };
        (tryExecute(std::type_identity<Components>{}), ...);
    }

    template <typename Primary, typename... Components, typename Fn>
    void forEachFromStore(Fn&& fn)
    {
        const auto* primaryStore = tryGetStore<std::remove_const_t<Primary>>();
        if (primaryStore == nullptr) {
            return;
        }

        for (const Entity entity : primaryStore->entities()) {
            if (!isAlive(entity)) {
                continue;
            }
            if ((has<std::remove_const_t<Components>>(entity) && ...)) {
                fn(entity, *resolveViewComponentPtr<Components>(entity)...);
            }
        }
    }

    template <typename Primary, typename... Components, typename Fn>
    void forEachFromStoreConst(Fn&& fn) const
    {
        const auto* primaryStore = tryGetStore<std::remove_const_t<Primary>>();
        if (primaryStore == nullptr) {
            return;
        }

        for (const Entity entity : primaryStore->entities()) {
            if (!isAlive(entity)) {
                continue;
            }
            if ((has<std::remove_const_t<Components>>(entity) && ...)) {
                fn(entity, *get<std::remove_const_t<Components>>(entity)...);
            }
        }
    }

    template <typename Component>
    auto resolveViewComponentPtr(Entity entity)
    {
        using Base = std::remove_const_t<Component>;
        if constexpr (std::is_const_v<Component>) {
            return static_cast<const Base*>(static_cast<const World*>(this)->get<Base>(entity));
        } else {
            return get<Base>(entity);
        }
    }

    std::vector<uint32_t> generations_{};
    std::vector<bool> alive_{};
    std::vector<uint32_t> freeList_{};
    std::unordered_map<std::type_index, std::unique_ptr<ComponentStoreBase>> componentStores_{};
    DeferredReplayMetrics lastDeferredReplayMetrics_{};

    friend class WorldCommandBuffer;
};

class WorldCommandBuffer {
public:
    enum class Op : uint8_t { Create, Destroy, Add, Remove };

    struct ICommand {
        virtual ~ICommand() = default;
        [[nodiscard]] virtual Op op() const = 0;
        [[nodiscard]] virtual bool apply(World& world) = 0;
    };

    template <typename T>
    struct AddCommand final : public ICommand {
        Entity entity{};
        T value{};

        [[nodiscard]] Op op() const override { return Op::Add; }

        [[nodiscard]] bool apply(World& world) override
        {
            return world.addImmediateNoAccessCheck<T>(entity, std::move(value));
        }
    };

    template <typename T>
    struct RemoveCommand final : public ICommand {
        Entity entity{};

        [[nodiscard]] Op op() const override { return Op::Remove; }

        [[nodiscard]] bool apply(World& world) override
        {
            return world.removeImmediateNoAccessCheck<T>(entity);
        }
    };

    struct DestroyCommand final : public ICommand {
        Entity entity{};

        [[nodiscard]] Op op() const override { return Op::Destroy; }

        [[nodiscard]] bool apply(World& world) override
        {
            return world.destroyImmediateNoAccessCheck(entity);
        }
    };

    template <typename T>
    void queueAdd(Entity entity, T value)
    {
        auto command = std::make_unique<AddCommand<std::remove_cv_t<T>>>();
        command->entity = entity;
        command->value = std::move(value);
        commands_.push_back(std::move(command));
    }

    template <typename T>
    void queueRemove(Entity entity)
    {
        auto command = std::make_unique<RemoveCommand<std::remove_cv_t<T>>>();
        command->entity = entity;
        commands_.push_back(std::move(command));
    }

    void queueDestroy(Entity entity)
    {
        auto command = std::make_unique<DestroyCommand>();
        command->entity = entity;
        commands_.push_back(std::move(command));
    }

    void queueCreate() = delete;

    void appendFrom(WorldCommandBuffer& other)
    {
        commands_.insert(commands_.end(),
            std::make_move_iterator(other.commands_.begin()),
            std::make_move_iterator(other.commands_.end()));
        other.commands_.clear();
    }

    [[nodiscard]] World::DeferredReplayMetrics apply(World& world)
    {
        World::DeferredReplayMetrics metrics{};
        for (auto& command : commands_) {
            switch (command->op()) {
            case Op::Add: metrics.queuedAdds += 1; break;
            case Op::Remove: metrics.queuedRemoves += 1; break;
            case Op::Destroy: metrics.queuedDestroys += 1; break;
            case Op::Create: metrics.queuedCreates += 1; break;
            }

            const bool applied = command->apply(world);
            if (!applied) {
                metrics.noopCommands += 1;
                continue;
            }

            switch (command->op()) {
            case Op::Add: metrics.appliedAdds += 1; break;
            case Op::Remove: metrics.appliedRemoves += 1; break;
            case Op::Destroy: metrics.appliedDestroys += 1; break;
            case Op::Create: break;
            }
        }
        return metrics;
    }

    void clear() { commands_.clear(); }

private:
    std::vector<std::unique_ptr<ICommand>> commands_{};
};

inline void World::applyDeferredCommands(WorldCommandBuffer& merged)
{
    // Structural command replay is intentionally centralized here so extraction
    // always sees a post-barrier, structurally consistent world snapshot.
    lastDeferredReplayMetrics_ = merged.apply(*this);
    merged.clear();
}



template <typename... Components>
struct TypeList {};

template <typename T, typename List>
struct TypeListContains;

template <typename T, typename... Components>
struct TypeListContains<T, TypeList<Components...>> : std::bool_constant<(std::is_same_v<T, Components> || ...)> {};

template <typename ReadList, typename WriteList>
class RestrictedWorld;

template <typename... Reads, typename... Writes>
class RestrictedWorld<TypeList<Reads...>, TypeList<Writes...>> {
public:
    RestrictedWorld(World& world, WorldCommandBuffer& commandBuffer, bool declaredStructuralWriter)
        : world_(world)
        , commandBuffer_(commandBuffer)
        , declaredStructuralWriter_(declaredStructuralWriter)
    {
    }

    [[nodiscard]] bool isAlive(Entity entity) const { return world_.isAlive(entity); }

    template <typename T>
    [[nodiscard]] bool has(Entity entity) const
    {
        using Component = std::remove_cv_t<T>;
        static_assert(canRead<Component>(), "Component type not declared in reads/writes");
        return world_.has<Component>(entity);
    }

    template <typename T>
    [[nodiscard]] const T* get(Entity entity) const
    {
        using Component = std::remove_cv_t<T>;
        static_assert(canRead<Component>(), "Component type not declared in reads/writes");
        return world_.get<Component>(entity);
    }

    template <typename T>
    [[nodiscard]] T* getMutable(Entity entity)
    {
        using Component = std::remove_cv_t<T>;
        static_assert(canWrite<Component>(), "Component type not declared in writes");
        return world_.get<Component>(entity);
    }

    [[nodiscard]] WorldCommandBuffer& commands() { return commandBuffer_; }

    template <typename T>
    void queueAdd(Entity entity, T value)
    {
        using Component = std::remove_cv_t<T>;
        static_assert(canWrite<Component>(), "Component type not declared in writes");
        assertStructuralWriteDeclaration();
        commandBuffer_.queueAdd<Component>(entity, std::move(value));
    }

    template <typename T>
    void queueRemove(Entity entity)
    {
        using Component = std::remove_cv_t<T>;
        static_assert(canWrite<Component>(), "Component type not declared in writes");
        assertStructuralWriteDeclaration();
        commandBuffer_.queueRemove<Component>(entity);
    }

    void queueDestroy(Entity entity)
    {
        assertStructuralWriteDeclaration();
        commandBuffer_.queueDestroy(entity);
    }

    template <typename... Components>
    [[nodiscard]] auto view()
    {
        static_assert((canUseViewComponent<Components>() && ...), "View component not declared in reads/writes");
        return world_.view<Components...>();
    }

    template <typename... Components, typename Fn>
    void each(Fn&& fn)
    {
        static_assert((canUseViewComponent<Components>() && ...), "Each component not declared in reads/writes");
        world_.each<Components...>(std::forward<Fn>(fn));
    }

private:
    void assertStructuralWriteDeclaration() const
    {
        assert(declaredStructuralWriter_ && "System must be registered as structural writer to queue structural commands");
        if (!declaredStructuralWriter_) {
            std::terminate();
        }
    }

    template <typename T>
    static consteval bool canWrite()
    {
        return TypeListContains<T, TypeList<Writes...>>::value;
    }

    template <typename T>
    static consteval bool canRead()
    {
        return TypeListContains<T, TypeList<Reads...>>::value || canWrite<T>();
    }

    template <typename Component>
    static consteval bool canUseViewComponent()
    {
        using Base = std::remove_const_t<Component>;
        if constexpr (std::is_const_v<Component>) {
            return canRead<Base>();
        } else {
            return canWrite<Base>();
        }
    }

    World& world_;
    WorldCommandBuffer& commandBuffer_;
    bool declaredStructuralWriter_{ false };
};

} // namespace ecs
