#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
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

enum class StructuralWrites : uint8_t {
    No,
    Yes
};

class SystemScheduler {
public:
    template <typename T>
    static std::type_index typeOf()
    {
        return std::type_index(typeid(std::remove_cv_t<T>));
    }

    template <typename ReadList, typename WriteList, typename Fn>
    void addSystem(std::string name, SystemPhase phase, StructuralWrites structuralWrites, Fn&& fn)
    {
        SystemDesc desc{};
        desc.name = std::move(name);
        desc.phase = phase;
        desc.writesStructure = (structuralWrites == StructuralWrites::Yes);
        appendTypeList<ReadList>(desc.reads);
        appendTypeList<WriteList>(desc.writes);
        auto runner = std::make_unique<SystemRunnerModel<ReadList, WriteList, std::decay_t<Fn>>>(std::forward<Fn>(fn));
        runner->setWritesStructure(desc.writesStructure);
        desc.runner = std::move(runner);
        systems_.push_back(std::move(desc));
    }

    void clear() { systems_.clear(); }

    void setMaxWorkerThreads(uint32_t workers)
    {
        maxWorkerThreads_ = std::max<uint32_t>(1u, workers);
    }

    void execute(World& world, const SystemFrameContext& context) const
    {
        executePhase(world, context, SystemPhase::PreSimulation);
        executePhase(world, context, SystemPhase::Simulation);
        executePhase(world, context, SystemPhase::PostSimulation);
    }

private:
    struct SystemRunner {
        virtual ~SystemRunner() = default;
        virtual void run(World& world, WorldCommandBuffer& commandBuffer, const SystemFrameContext& context) = 0;
    };

    template <typename ReadList, typename WriteList, typename Fn>
    class SystemRunnerModel final : public SystemRunner {
    public:
        explicit SystemRunnerModel(Fn fn)
            : fn_(std::move(fn))
        {
        }

        void run(World& world, WorldCommandBuffer& commandBuffer, const SystemFrameContext& context) override
        {
            RestrictedWorld<ReadList, WriteList> access(world, commandBuffer, writesStructure_);
            fn_(access, context);
        }

        void setWritesStructure(bool writesStructure)
        {
            writesStructure_ = writesStructure;
        }

    private:
        Fn fn_;
        bool writesStructure_{ false };
    };

    template <typename List>
    struct TypeListAppender;

    template <typename... Ts>
    struct TypeListAppender<TypeList<Ts...>> {
        static void append(std::vector<std::type_index>& out)
        {
            (out.push_back(typeOf<Ts>()), ...);
        }
    };

    template <typename List>
    static void appendTypeList(std::vector<std::type_index>& out)
    {
        TypeListAppender<List>::append(out);
    }

    struct SystemDesc {
        std::string name{};
        SystemPhase phase{ SystemPhase::Simulation };
        std::vector<std::type_index> reads{};
        std::vector<std::type_index> writes{};
        bool writesStructure{ false };
        std::unique_ptr<SystemRunner> runner{};
    };

    class SystemAccessScope {
    public:
        SystemAccessScope(World& world, const SystemDesc& system)
            : world_(world)
        {
            world_.installSystemAccessContext(World::SystemAccessDeclaration{
                .systemName = system.name,
                .reads = &system.reads,
                .writes = &system.writes,
            });
        }

        ~SystemAccessScope()
        {
            world_.clearSystemAccessContext();
        }

        SystemAccessScope(const SystemAccessScope&) = delete;
        SystemAccessScope& operator=(const SystemAccessScope&) = delete;

    private:
        World& world_;
    };

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
        if (a.writesStructure && b.writesStructure) {
            return true;
        }
        return anyShared(a.writes, b.writes) || anyShared(a.writes, b.reads) || anyShared(a.reads, b.writes);
    }

    [[nodiscard]] std::vector<std::vector<size_t>> buildIndependentBatches(SystemPhase phase) const
    {
        std::vector<std::vector<size_t>> batches{};
        for (size_t i = 0; i < systems_.size(); ++i) {
            if (systems_[i].phase != phase || systems_[i].runner == nullptr) {
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

    static void runSystem(World& world, WorldCommandBuffer& commandBuffer, const SystemFrameContext& context, const SystemDesc& system)
    {
        SystemAccessScope scope(world, system);
        system.runner->run(world, commandBuffer, context);
    }

    void applyBatchCommandBuffers(World& world, std::vector<WorldCommandBuffer>& commandBuffers) const
    {
        // Deterministic replay contract:
        // 1) buffers are indexed by batch slot (stable system order in `batch`)
        // 2) merge in ascending slot order
        // 3) each buffer preserves command insertion order
        WorldCommandBuffer merged{};
        for (auto& commandBuffer : commandBuffers) {
            merged.appendFrom(commandBuffer);
        }
        // Barrier apply point for all structural mutations emitted in this batch.
        world.applyDeferredCommands(merged);
    }

    void executeBatch(World& world, const SystemFrameContext& context, const std::vector<size_t>& batch) const
    {
        std::vector<WorldCommandBuffer> commandBuffers(batch.size());
        const uint32_t hw = std::max<uint32_t>(1u, std::thread::hardware_concurrency());
        const uint32_t workerLimit = std::min<uint32_t>(maxWorkerThreads_, hw);
        const uint32_t workers = std::min<uint32_t>(workerLimit, static_cast<uint32_t>(batch.size()));
        if (workers <= 1u) {
            for (size_t i = 0; i < batch.size(); ++i) {
                runSystem(world, commandBuffers[i], context, systems_[batch[i]]);
            }
            applyBatchCommandBuffers(world, commandBuffers);
            return;
        }

        std::vector<std::thread> threads{};
        threads.reserve(workers);
        for (uint32_t worker = 0; worker < workers; ++worker) {
            threads.emplace_back([&, worker]() {
                for (size_t i = worker; i < batch.size(); i += workers) {
                    runSystem(world, commandBuffers[i], context, systems_[batch[i]]);
                }
            });
        }

        for (auto& thread : threads) {
            thread.join();
        }

        applyBatchCommandBuffers(world, commandBuffers);
    }

    void executePhase(World& world, const SystemFrameContext& context, SystemPhase phase) const
    {
        // Deterministic order across phase/batch boundaries:
        // phase order is fixed by execute(); batches and systems are replayed in build order.
        const auto batches = buildIndependentBatches(phase);
        for (const auto& batch : batches) {
            executeBatch(world, context, batch);
        }
    }

    std::vector<SystemDesc> systems_{};
    uint32_t maxWorkerThreads_{ std::max<uint32_t>(1u, std::thread::hardware_concurrency()) };
};

} // namespace ecs
