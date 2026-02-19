#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <typeindex>
#include <unordered_set>
#include <utility>
#include <vector>

#include "World.h"

namespace ecs {

enum class SystemPhase : uint8_t {
    PreSimulation,
    Simulation,
    PostSimulation
};

struct SystemFrameContext {
    float deltaSeconds{ 0.0F };
    uint64_t frameIndex{ 0 };
};

class SystemScheduler {
public:
    struct SystemDesc {
        std::string name{};
        SystemPhase phase{ SystemPhase::Simulation };
        std::vector<std::type_index> reads{};
        std::vector<std::type_index> writes{};
        std::function<void(World&, const SystemFrameContext&)> run{};
    };

    template <typename T>
    static std::type_index typeOf()
    {
        return std::type_index(typeid(T));
    }

    void clear() { systems_.clear(); }

    void setMaxWorkerThreads(uint32_t workers)
    {
        maxWorkerThreads_ = std::max<uint32_t>(1u, workers);
    }

    void addSystem(SystemDesc desc)
    {
        systems_.push_back(std::move(desc));
    }

    void execute(World& world, const SystemFrameContext& context) const
    {
        executePhase(world, context, SystemPhase::PreSimulation);
        executePhase(world, context, SystemPhase::Simulation);
        executePhase(world, context, SystemPhase::PostSimulation);
    }

private:
    static bool anyShared(const std::vector<std::type_index>& lhs, const std::vector<std::type_index>& rhs)
    {
        std::unordered_set<std::type_index> set(lhs.begin(), lhs.end());
        for (const std::type_index type : rhs) {
            if (set.contains(type)) {
                return true;
            }
        }
        return false;
    }

    static bool conflicts(const SystemDesc& a, const SystemDesc& b)
    {
        return anyShared(a.writes, b.writes) || anyShared(a.writes, b.reads) || anyShared(a.reads, b.writes);
    }

    [[nodiscard]] std::vector<std::vector<size_t>> buildIndependentBatches(SystemPhase phase) const
    {
        std::vector<std::vector<size_t>> batches{};
        for (size_t i = 0; i < systems_.size(); ++i) {
            if (systems_[i].phase != phase || !systems_[i].run) {
                continue;
            }

            bool placed = false;
            for (auto& batch : batches) {
                bool hasConflict = false;
                for (const size_t idx : batch) {
                    if (conflicts(systems_[idx], systems_[i])) {
                        hasConflict = true;
                        break;
                    }
                }

                if (!hasConflict) {
                    batch.push_back(i);
                    placed = true;
                    break;
                }
            }

            if (!placed) {
                batches.push_back({ i });
            }
        }
        return batches;
    }

    void executeBatch(World& world, const SystemFrameContext& context, const std::vector<size_t>& batch) const
    {
        const uint32_t hw = std::max<uint32_t>(1u, std::thread::hardware_concurrency());
        const uint32_t workerLimit = std::min<uint32_t>(maxWorkerThreads_, hw);
        const uint32_t workers = std::min<uint32_t>(workerLimit, static_cast<uint32_t>(batch.size()));
        if (workers <= 1u) {
            for (const size_t index : batch) {
                systems_[index].run(world, context);
            }
            return;
        }

        std::vector<std::thread> threads{};
        threads.reserve(workers);
        for (uint32_t worker = 0; worker < workers; ++worker) {
            threads.emplace_back([&, worker]() {
                for (size_t i = worker; i < batch.size(); i += workers) {
                    systems_[batch[i]].run(world, context);
                }
            });
        }

        for (auto& thread : threads) {
            thread.join();
        }
    }

    void executePhase(World& world, const SystemFrameContext& context, SystemPhase phase) const
    {
        const auto batches = buildIndependentBatches(phase);
        for (const auto& batch : batches) {
            executeBatch(world, context, batch);
        }
    }

    std::vector<SystemDesc> systems_{};
    uint32_t maxWorkerThreads_{ std::max<uint32_t>(1u, std::thread::hardware_concurrency()) };
};

} // namespace ecs
