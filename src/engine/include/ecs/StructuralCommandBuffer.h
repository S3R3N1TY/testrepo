#pragma once

#include <ecs/Entity.h>
#include <ecs/StructuralPhase.h>
#include <ecs/World.h>

#include <array>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>
#include <optional>
#include <memory>

class StructuralCommandBuffer {
public:
    struct Command {
        std::function<bool(World&)> validate{};
        std::function<void(World&)> apply{};
        std::function<void(World&)> rollback{};
        bool deferUntilCommit{ false };
    };

    template <typename T, typename... Args>
    void emplaceComponent(Entity entity, Args&&... args)
    {
        static_assert(std::is_copy_constructible_v<T>, "Structural transactional commands require copy-constructible component types");
        auto state = std::make_shared<std::optional<T>>();
        enqueue(StructuralPlaybackPhase::PostSim, Command{
            .validate = [=](World& world) { return world.isAlive(entity); },
            .apply = [=, tup = std::make_tuple(std::forward<Args>(args)...)](World& world) mutable {
                if (auto* existing = world.template getComponent<T>(entity); existing != nullptr) {
                    *state = *existing;
                }
                std::apply([&](auto&&... inner) {
                    world.template emplaceComponent<T>(entity, std::forward<decltype(inner)>(inner)...);
                }, std::move(tup));
            },
            .rollback = [=](World& world) mutable {
                if (state->has_value()) {
                    world.template emplaceComponent<T>(entity, **state);
                }
                else {
                    world.template removeComponent<T>(entity);
                }
            }
        });
    }

    template <typename T, typename... Args>
    void setComponent(Entity entity, Args&&... args)
    {
        static_assert(std::is_copy_constructible_v<T>, "Structural transactional commands require copy-constructible component types");
        auto state = std::make_shared<std::optional<T>>();
        enqueue(StructuralPlaybackPhase::PostSim, Command{
            .validate = [=](World& world) { return world.isAlive(entity); },
            .apply = [=, tup = std::make_tuple(std::forward<Args>(args)...)](World& world) mutable {
                if (auto* existing = world.template getComponent<T>(entity); existing != nullptr) {
                    *state = *existing;
                }
                std::apply([&](auto&&... inner) {
                    world.template emplaceComponent<T>(entity, std::forward<decltype(inner)>(inner)...);
                }, std::move(tup));
            },
            .rollback = [=](World& world) mutable {
                if (state->has_value()) {
                    world.template emplaceComponent<T>(entity, **state);
                }
                else {
                    world.template removeComponent<T>(entity);
                }
            }
        });
    }

    template <typename T>
    void removeComponent(Entity entity)
    {
        static_assert(std::is_copy_constructible_v<T>, "Structural transactional commands require copy-constructible component types");
        auto state = std::make_shared<std::optional<T>>();
        enqueue(StructuralPlaybackPhase::PostSim, Command{
            .validate = [=](World& world) { return world.isAlive(entity); },
            .apply = [=](World& world) mutable {
                if (auto* existing = world.template getComponent<T>(entity); existing != nullptr) {
                    *state = *existing;
                }
                world.template removeComponent<T>(entity);
            },
            .rollback = [=](World& world) mutable {
                if (state->has_value()) {
                    world.template emplaceComponent<T>(entity, **state);
                }
            }
        });
    }

    void destroyEntity(Entity entity)
    {
        auto snapshot = std::make_shared<std::optional<World::EntitySnapshot>>();
        enqueue(StructuralPlaybackPhase::EndFrame, Command{
            .validate = [=](World& world) { return world.isAlive(entity); },
            .apply = [=](World& world) {
                if (auto snap = world.snapshotEntity(entity); snap.has_value()) {
                    *snapshot = std::move(*snap);
                }
                world.destroyEntity(entity);
            },
            .rollback = [=](World& world) {
                if (snapshot->has_value()) {
                    world.restoreEntity(**snapshot);
                }
            },
            .deferUntilCommit = true
        });
    }

    void playback(World& world, StructuralPlaybackPhase phase)
    {
        std::vector<Command> local{};
        {
            std::scoped_lock lock(mutex_);
            local.swap(commands_[static_cast<size_t>(phase)]);
        }

        for (const Command& command : local) {
            if (command.validate && !command.validate(world)) {
                throw std::runtime_error("StructuralCommandBuffer::playback validation failed");
            }
        }

        std::vector<std::function<void(World&)>> undo{};
        undo.reserve(local.size());
        std::vector<const Command*> deferred{};
        deferred.reserve(local.size());
        try {
            for (const Command& command : local) {
                if (command.deferUntilCommit) {
                    deferred.push_back(&command);
                    continue;
                }

                if (command.apply) {
                    command.apply(world);
                }
                if (command.rollback) {
                    undo.push_back(command.rollback);
                }
            }

            for (const Command* command : deferred) {
                if (command->apply) {
                    command->apply(world);
                }
                if (command->rollback) {
                    undo.push_back(command->rollback);
                }
            }
        }
        catch (...) {
            for (auto it = undo.rbegin(); it != undo.rend(); ++it) {
                (*it)(world);
            }
            throw;
        }
    }

    [[nodiscard]] bool empty() const
    {
        std::scoped_lock lock(mutex_);
        for (const auto& queue : commands_) {
            if (!queue.empty()) {
                return false;
            }
        }
        return true;
    }

private:
    void enqueue(StructuralPlaybackPhase phase, Command cmd)
    {
        std::scoped_lock lock(mutex_);
        commands_[static_cast<size_t>(phase)].push_back(std::move(cmd));
    }

    mutable std::mutex mutex_{};
    std::array<std::vector<Command>, static_cast<size_t>(StructuralPlaybackPhase::Count)> commands_{};
};
