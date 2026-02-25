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
        std::scoped_lock lock(mutex_);

        const uint64_t token = nextDeferredToken_++;
        auto created = std::make_shared<std::optional<Entity>>();
        const uint64_t createEntryId = nextEntryId_;

        deferredByToken_.insert_or_assign(token, DeferredRecord{
            .createEntryId = createEntryId,
            .resolved = created,
            .pendingEntries = 0
        });

        const uint32_t key = deferredKey(token);
        enqueueLocked(makeEntryLocked(JournalOpType::CreateEntity,
            phase,
            [](World&) { return true; },
            [created](World& world) {
                *created = world.createEntity();
            },
            [created](World& world) {
                if (created->has_value() && world.isAlive(**created)) {
                    world.destroyEntity(**created);
                }
            },
            dependenciesForKeyLocked(key),
            token));

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

        Transaction tx{ entries };
        tx.execute(world, failureInjection_ ? &*failureInjection_ : nullptr);

        std::scoped_lock lock(mutex_);
        for (const JournalEntry& entry : entries) {
            const auto tokenIt = entryToDeferredToken_.find(entry.id);
            if (tokenIt == entryToDeferredToken_.end()) {
                continue;
            }
            const uint64_t token = tokenIt->second;
            entryToDeferredToken_.erase(tokenIt);
            if (auto deferredIt = deferredByToken_.find(token); deferredIt != deferredByToken_.end()) {
                if (deferredIt->second.pendingEntries > 0) {
                    deferredIt->second.pendingEntries -= 1;
                }
            }
        }

        if (phase == StructuralPlaybackPhase::PostSim || phase == StructuralPlaybackPhase::EndFrame) {
            sweepResolvedDeferredLocked();
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
        uint64_t pendingEntries{ 0 };
    };

    struct ResolvedEntity {
        Entity direct{};
        std::shared_ptr<std::optional<Entity>> resolved{};
        uint32_t dependencyKey{ 0 };
        uint64_t createEntryId{ 0 };
        uint64_t deferredToken{ 0 };

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
        return ResolvedEntity{
            .direct = entity,
            .resolved = {},
            .dependencyKey = entity.id,
            .createEntryId = 0,
            .deferredToken = 0
        };
    }

    ResolvedEntity resolveDeferred(DeferredEntityHandle handle)
    {
        std::scoped_lock lock(mutex_);
        const auto it = deferredByToken_.find(handle.token);
        if (it == deferredByToken_.end() || it->second.createEntryId == 0) {
            throw std::runtime_error("Unknown deferred entity handle in current command buffer epoch");
        }
        return ResolvedEntity{
            .direct = {},
            .resolved = it->second.resolved,
            .dependencyKey = deferredKey(handle.token),
            .createEntryId = it->second.createEntryId,
            .deferredToken = handle.token
        };
    }

    template <typename T, typename... Args>
    void enqueueComponentOp(JournalOpType opType, const ResolvedEntity& resolved, Args&&... args)
    {
        static_assert(std::is_copy_constructible_v<T>, "Structural transactional commands require copy-constructible component types");
        auto state = std::make_shared<std::optional<T>>();
        std::scoped_lock lock(mutex_);
        enqueueLocked(makeEntryLocked(opType,
            StructuralPlaybackPhase::PostSim,
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
            dependenciesForResolvedLocked(resolved),
            resolved.deferredToken));
    }

    template <typename T>
    void enqueueRemoveOp(const ResolvedEntity& resolved)
    {
        static_assert(std::is_copy_constructible_v<T>, "Structural transactional commands require copy-constructible component types");
        auto state = std::make_shared<std::optional<T>>();
        std::scoped_lock lock(mutex_);
        enqueueLocked(makeEntryLocked(JournalOpType::RemoveComponent,
            StructuralPlaybackPhase::PostSim,
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
            dependenciesForResolvedLocked(resolved),
            resolved.deferredToken));
    }

    void enqueueDestroy(const ResolvedEntity& resolved)
    {
        auto snapshot = std::make_shared<std::optional<World::EntitySnapshot>>();
        std::scoped_lock lock(mutex_);
        enqueueLocked(makeEntryLocked(JournalOpType::DestroyEntity,
            StructuralPlaybackPhase::EndFrame,
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
            dependenciesForResolvedLocked(resolved),
            resolved.deferredToken));
    }

    JournalEntry makeEntryLocked(JournalOpType type,
        StructuralPlaybackPhase phase,
        std::function<bool(World&)> validate,
        std::function<void(World&)> apply,
        std::function<void(World&)> undo,
        std::vector<uint64_t> dependsOn = {},
        uint64_t deferredToken = 0)
    {
        JournalEntry entry{};
        entry.id = nextEntryId_++;
        entry.type = type;
        entry.phase = phase;
        entry.dependsOn = std::move(dependsOn);
        entry.validate = std::move(validate);
        entry.apply = std::move(apply);
        entry.undo = std::move(undo);

        if (deferredToken != 0) {
            auto it = deferredByToken_.find(deferredToken);
            if (it == deferredByToken_.end() || it->second.createEntryId == 0) {
                throw std::runtime_error("Deferred token has no create entry in this command buffer epoch");
            }
            entryToDeferredToken_.insert_or_assign(entry.id, deferredToken);
            it->second.pendingEntries += 1;
        }

        return entry;
    }

    std::vector<uint64_t> dependenciesForResolvedLocked(const ResolvedEntity& resolved)
    {
        std::vector<uint64_t> deps = dependenciesForKeyLocked(resolved.dependencyKey);
        if (resolved.createEntryId != 0) {
            deps.push_back(resolved.createEntryId);
        }
        return deps;
    }

    std::vector<uint64_t> dependenciesForKeyLocked(uint32_t key)
    {
        std::vector<uint64_t> deps{};
        if (const auto it = lastEntryByKey_.find(key); it != lastEntryByKey_.end()) {
            deps.push_back(it->second);
        }
        lastEntryByKey_[key] = nextEntryId_;
        return deps;
    }

    void sweepResolvedDeferredLocked()
    {
        std::vector<uint64_t> toErase{};
        toErase.reserve(deferredByToken_.size());
        for (const auto& [token, record] : deferredByToken_) {
            if (record.resolved->has_value() && record.pendingEntries == 0) {
                toErase.push_back(token);
            }
        }

        for (uint64_t token : toErase) {
            lastEntryByKey_.erase(deferredKey(token));
            deferredByToken_.erase(token);
        }
    }

    void enqueueLocked(JournalEntry entry)
    {
        entriesByPhase_[static_cast<size_t>(entry.phase)].push_back(std::move(entry));
    }

    mutable std::mutex mutex_{};
    std::array<std::vector<JournalEntry>, static_cast<size_t>(StructuralPlaybackPhase::Count)> entriesByPhase_{};
    uint64_t nextEntryId_{ 1 };
    uint64_t nextDeferredToken_{ 1 };
    std::optional<FailureInjectionConfig> failureInjection_{};
    std::unordered_map<uint32_t, uint64_t> lastEntryByKey_{};
    std::unordered_map<uint64_t, DeferredRecord> deferredByToken_{};
    std::unordered_map<uint64_t, uint64_t> entryToDeferredToken_{};
};
