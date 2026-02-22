#pragma once

#include <ecs/Entity.h>
#include <ecs/StructuralPhase.h>
#include <ecs/TransactionJournal.h>
#include <ecs/World.h>

#include <array>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

class StructuralCommandBuffer {
public:
    struct DeferredEntityHandle {
        uint64_t token{ 0 };
    };

    template <typename T, typename... Args>
    void emplaceComponent(Entity entity, Args&&... args)
    {
        enqueueComponentOp<T>(JournalOpType::EmplaceComponent, resolveEntity(entity), std::forward<Args>(args)...);
    }

    template <typename T, typename... Args>
    void emplaceComponent(DeferredEntityHandle handle, Args&&... args)
    {
        enqueueComponentOp<T>(JournalOpType::EmplaceComponent, resolveDeferred(handle), std::forward<Args>(args)...);
    }

    template <typename T, typename... Args>
    void setComponent(Entity entity, Args&&... args)
    {
        enqueueComponentOp<T>(JournalOpType::SetComponent, resolveEntity(entity), std::forward<Args>(args)...);
    }

    template <typename T, typename... Args>
    void setComponent(DeferredEntityHandle handle, Args&&... args)
    {
        enqueueComponentOp<T>(JournalOpType::SetComponent, resolveDeferred(handle), std::forward<Args>(args)...);
    }

    template <typename T>
    void removeComponent(Entity entity)
    {
        enqueueRemoveOp<T>(resolveEntity(entity));
    }

    template <typename T>
    void removeComponent(DeferredEntityHandle handle)
    {
        enqueueRemoveOp<T>(resolveDeferred(handle));
    }

    DeferredEntityHandle createEntity(StructuralPlaybackPhase phase = StructuralPlaybackPhase::PostSim)
    {
        const uint64_t token = nextDeferredToken_++;
        auto created = std::make_shared<std::optional<Entity>>();
        const uint64_t entryId = nextEntryId_;
        deferredByToken_.insert_or_assign(token, DeferredRecord{ .createEntryId = entryId, .resolved = created });

        enqueue(makeEntry(JournalOpType::CreateEntity, phase,
            [](World&) { return true; },
            [created](World& world) {
                *created = world.createEntity();
            },
            [created](World& world) {
                if (created->has_value() && world.isAlive(**created)) {
                    world.destroyEntity(**created);
                }
            },
            dependenciesForKey(deferredKey(token))));

        return DeferredEntityHandle{ .token = token };
    }

    void destroyEntity(Entity entity)
    {
        enqueueDestroy(resolveEntity(entity));
    }

    void destroyEntity(DeferredEntityHandle handle)
    {
        enqueueDestroy(resolveDeferred(handle));
    }

    void playback(World& world, StructuralPlaybackPhase phase)
    {
        std::vector<JournalEntry> entries{};
        {
            std::scoped_lock lock(mutex_);
            entries.swap(entriesByPhase_[static_cast<size_t>(phase)]);
        }

        if (entries.empty()) {
            return;
        }
        Transaction tx{ std::move(entries) };
        tx.execute(world, failureInjection_ ? &*failureInjection_ : nullptr);

        if (phase == StructuralPlaybackPhase::PostSim || phase == StructuralPlaybackPhase::EndFrame) {
            sweepResolvedDeferred();
        }
    }

    void setFailureInjection(FailureInjectionConfig cfg)
    {
        failureInjection_ = std::move(cfg);
    }

    [[nodiscard]] bool empty() const
    {
        std::scoped_lock lock(mutex_);
        for (const auto& queue : entriesByPhase_) {
            if (!queue.empty()) {
                return false;
            }
        }
        return true;
    }

private:
    struct DeferredRecord {
        uint64_t createEntryId{ 0 };
        std::shared_ptr<std::optional<Entity>> resolved{};
    };

    struct ResolvedEntity {
        Entity direct{};
        std::shared_ptr<std::optional<Entity>> resolved{};
        uint32_t dependencyKey{ 0 };
        uint64_t createEntryId{ 0 };

        [[nodiscard]] Entity require() const
        {
            if (resolved) {
                if (!resolved->has_value()) {
                    throw std::runtime_error("Deferred entity is not resolved yet");
                }
                return **resolved;
            }
            return direct;
        }

        [[nodiscard]] bool exists(World& world) const
        {
            const Entity e = require();
            return world.isAlive(e);
        }
    };

    static uint32_t deferredKey(uint64_t token)
    {
        return static_cast<uint32_t>(0x80000000u ^ static_cast<uint32_t>(token));
    }

    ResolvedEntity resolveEntity(Entity entity)
    {
        return ResolvedEntity{ .direct = entity, .resolved = {}, .dependencyKey = entity.id, .createEntryId = 0 };
    }

    ResolvedEntity resolveDeferred(DeferredEntityHandle handle)
    {
        const auto it = deferredByToken_.find(handle.token);
        if (it == deferredByToken_.end()) {
            throw std::runtime_error("Unknown deferred entity handle");
        }
        return ResolvedEntity{ .direct = {}, .resolved = it->second.resolved, .dependencyKey = deferredKey(handle.token), .createEntryId = it->second.createEntryId };
    }

    template <typename T, typename... Args>
    void enqueueComponentOp(JournalOpType opType, const ResolvedEntity& resolved, Args&&... args)
    {
        static_assert(std::is_copy_constructible_v<T>, "Structural transactional commands require copy-constructible component types");
        auto state = std::make_shared<std::optional<T>>();
        enqueue(makeEntry(opType, StructuralPlaybackPhase::PostSim,
            [resolved](World& world) { return resolved.exists(world); },
            [=, tup = std::make_tuple(std::forward<Args>(args)...)](World& world) mutable {
                const Entity target = resolved.require();
                if (auto* existing = world.template getComponent<T>(target); existing != nullptr) {
                    *state = *existing;
                }
                std::apply([&](auto&&... inner) {
                    world.template emplaceComponent<T>(target, std::forward<decltype(inner)>(inner)...);
                }, std::move(tup));
            },
            [=](World& world) mutable {
                const Entity target = resolved.require();
                if (state->has_value()) {
                    world.template emplaceComponent<T>(target, **state);
                }
                else {
                    world.template removeComponent<T>(target);
                }
            },
            dependenciesForResolved(resolved)));
    }

    template <typename T>
    void enqueueRemoveOp(const ResolvedEntity& resolved)
    {
        static_assert(std::is_copy_constructible_v<T>, "Structural transactional commands require copy-constructible component types");
        auto state = std::make_shared<std::optional<T>>();
        enqueue(makeEntry(JournalOpType::RemoveComponent, StructuralPlaybackPhase::PostSim,
            [resolved](World& world) { return resolved.exists(world); },
            [=](World& world) mutable {
                const Entity target = resolved.require();
                if (auto* existing = world.template getComponent<T>(target); existing != nullptr) {
                    *state = *existing;
                }
                world.template removeComponent<T>(target);
            },
            [=](World& world) mutable {
                const Entity target = resolved.require();
                if (state->has_value()) {
                    world.template emplaceComponent<T>(target, **state);
                }
            },
            dependenciesForResolved(resolved)));
    }

    void enqueueDestroy(const ResolvedEntity& resolved)
    {
        auto snapshot = std::make_shared<std::optional<World::EntitySnapshot>>();
        enqueue(makeEntry(JournalOpType::DestroyEntity, StructuralPlaybackPhase::EndFrame,
            [resolved](World& world) { return resolved.exists(world); },
            [=](World& world) {
                const Entity target = resolved.require();
                if (auto snap = world.snapshotEntity(target); snap.has_value()) {
                    *snapshot = std::move(*snap);
                }
                world.destroyEntity(target);
            },
            [=](World& world) {
                if (snapshot->has_value()) {
                    world.restoreEntity(**snapshot);
                }
            },
            dependenciesForResolved(resolved)));
    }

    JournalEntry makeEntry(JournalOpType type,
        StructuralPlaybackPhase phase,
        std::function<bool(World&)> validate,
        std::function<void(World&)> apply,
        std::function<void(World&)> undo,
        std::vector<uint64_t> dependsOn = {})
    {
        JournalEntry entry{};
        entry.id = nextEntryId_++;
        entry.type = type;
        entry.phase = phase;
        entry.dependsOn = std::move(dependsOn);
        entry.validate = std::move(validate);
        entry.apply = std::move(apply);
        entry.undo = std::move(undo);
        return entry;
    }

    std::vector<uint64_t> dependenciesForResolved(const ResolvedEntity& resolved)
    {
        std::vector<uint64_t> deps = dependenciesForKey(resolved.dependencyKey);
        if (resolved.createEntryId != 0) {
            deps.push_back(resolved.createEntryId);
        }
        return deps;
    }

    std::vector<uint64_t> dependenciesForKey(uint32_t key)
    {
        std::vector<uint64_t> deps{};
        if (const auto it = lastEntryByKey_.find(key); it != lastEntryByKey_.end()) {
            deps.push_back(it->second);
        }
        lastEntryByKey_[key] = nextEntryId_;
        return deps;
    }

    void sweepResolvedDeferred()
    {
        std::erase_if(deferredByToken_, [](const auto& pair) {
            return pair.second.resolved->has_value();
        });
    }

    void enqueue(JournalEntry entry)
    {
        std::scoped_lock lock(mutex_);
        entriesByPhase_[static_cast<size_t>(entry.phase)].push_back(std::move(entry));
    }

    mutable std::mutex mutex_{};
    std::array<std::vector<JournalEntry>, static_cast<size_t>(StructuralPlaybackPhase::Count)> entriesByPhase_{};
    uint64_t nextEntryId_{ 1 };
    uint64_t nextDeferredToken_{ 1 };
    std::optional<FailureInjectionConfig> failureInjection_{};
    std::unordered_map<uint32_t, uint64_t> lastEntryByKey_{};
    std::unordered_map<uint64_t, DeferredRecord> deferredByToken_{};
};
