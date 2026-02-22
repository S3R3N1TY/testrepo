#include "RenderExtractSys.h"

#include "../components/RenderComp.h"
#include "../components/RotationComp.h"
#include "../components/LocalToWorldComp.h"
#include "../components/VisibilityComp.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
class PersistentExtractWorkers {
public:
    explicit PersistentExtractWorkers(size_t workerCount)
        : workerCount_(std::max<size_t>(1, workerCount))
    {
        workers_.reserve(workerCount_);
        for (size_t i = 0; i < workerCount_; ++i) {
            workers_.emplace_back([this, i] { loop(i); });
        }
    }

    ~PersistentExtractWorkers()
    {
        {
            std::unique_lock lock(mutex_);
            stop_ = true;
            generation_++;
        }
        cvStart_.notify_all();
        for (auto& w : workers_) {
            if (w.joinable()) w.join();
        }
    }

    template <typename Fn>
    void run(Fn&& fn)
    {
        {
            std::unique_lock lock(mutex_);
            task_ = std::forward<Fn>(fn);
            completed_ = 0;
            generation_++;
        }
        cvStart_.notify_all();
        std::unique_lock lock(mutex_);
        cvDone_.wait(lock, [&] { return completed_ == workerCount_; });
    }

    [[nodiscard]] size_t workerCount() const noexcept { return workerCount_; }

private:
    void loop(size_t idx)
    {
        uint64_t seen = 0;
        while (true) {
            std::function<void(size_t)> fn;
            {
                std::unique_lock lock(mutex_);
                cvStart_.wait(lock, [&] { return stop_ || generation_ != seen; });
                if (stop_) {
                    return;
                }
                seen = generation_;
                fn = task_;
            }

            if (fn) {
                fn(idx);
            }

            {
                std::unique_lock lock(mutex_);
                completed_++;
                if (completed_ == workerCount_) {
                    cvDone_.notify_one();
                }
            }
        }
    }

    size_t workerCount_{ 1 };
    std::vector<std::thread> workers_{};
    std::mutex mutex_{};
    std::condition_variable cvStart_{};
    std::condition_variable cvDone_{};
    bool stop_{ false };
    uint64_t generation_{ 0 };
    size_t completed_{ 0 };
    std::function<void(size_t)> task_{};
};

PersistentExtractWorkers& extractWorkers()
{
    static PersistentExtractWorkers workers{ std::max<unsigned>(1, std::thread::hardware_concurrency()) };
    return workers;
}
}



namespace {
struct DrawBuildPacket {
    Entity entity{};
    DrawPacket draw{};
};

struct ViewBuildPacket {
    uint32_t viewId{ 0 };
    bool hasOverride{ false };
    std::array<float, 4> clearColor{ 0.02F, 0.02F, 0.08F, 1.0F };
};

struct ChunkInput {
    const Entity* entities{ nullptr };
    const RenderComp* render{ nullptr };
    const RotationComp* rotation{ nullptr };
    const VisibilityComp* visibility{ nullptr };
    const LocalToWorldComp* localToWorld{ nullptr };
    size_t count{ 0 };
};

struct WorkerOutput {
    std::vector<ViewBuildPacket> views{};
    std::vector<DrawBuildPacket> draws{};
};

void cullAndEmitChunk(const ChunkInput& input, WorkerOutput& out)
{
    for (size_t row = 0; row < input.count; ++row) {
        const RenderComp& render = input.render[row];
        const RotationComp& rotation = input.rotation[row];
        const VisibilityComp& visibility = input.visibility[row];
        const LocalToWorldComp& l2w = input.localToWorld[row];
        if (!visibility.visible || !render.visible) {
            continue;
        }

        out.views.push_back(ViewBuildPacket{
            .viewId = render.viewId,
            .hasOverride = render.overrideClearColor,
            .clearColor = render.clearColor
        });

        out.draws.push_back(DrawBuildPacket{
            .entity = input.entities[row],
            .draw = DrawPacket{
                .viewId = render.viewId,
                .materialId = render.materialId,
                .vertexCount = render.vertexCount,
                .firstVertex = render.firstVertex,
                .angleRadians = rotation.angleRadians + (l2w.m[12] * 0.001F) }
        });
    }
}

void binMaterials(std::vector<DrawBuildPacket>& pendingDraws, FrameGraphInput& output)
{
    std::ranges::stable_sort(pendingDraws, [](const DrawBuildPacket& a, const DrawBuildPacket& b) {
        if (a.draw.materialId != b.draw.materialId) {
            return a.draw.materialId < b.draw.materialId;
        }
        return a.entity.id < b.entity.id;
    });

    output.drawPackets.reserve(pendingDraws.size());
    output.materialBatches.reserve(pendingDraws.size());

    uint32_t currentMaterial = 0;
    bool hasMaterial = false;
    uint32_t firstDrawIndex = 0;

    for (const DrawBuildPacket& pending : pendingDraws) {
        const uint32_t drawIndex = static_cast<uint32_t>(output.drawPackets.size());
        output.drawPackets.push_back(pending.draw);

        if (!hasMaterial) {
            currentMaterial = pending.draw.materialId;
            hasMaterial = true;
            firstDrawIndex = drawIndex;
            continue;
        }

        if (pending.draw.materialId != currentMaterial) {
            output.materialBatches.push_back(MaterialBatchPacket{
                .materialId = currentMaterial,
                .firstDrawPacket = firstDrawIndex,
                .drawPacketCount = drawIndex - firstDrawIndex });
            currentMaterial = pending.draw.materialId;
            firstDrawIndex = drawIndex;
        }
    }

    if (hasMaterial) {
        output.materialBatches.push_back(MaterialBatchPacket{
            .materialId = currentMaterial,
            .firstDrawPacket = firstDrawIndex,
            .drawPacketCount = static_cast<uint32_t>(output.drawPackets.size()) - firstDrawIndex });
    }
}
} // namespace
FrameGraphInput RenderExtractSys::build(const World& world) const
{
    const auto renderType = world.componentTypeId<RenderComp>();
    const auto rotationType = world.componentTypeId<RotationComp>();
    const auto visibilityType = world.componentTypeId<VisibilityComp>();
    const auto l2wType = world.componentTypeId<LocalToWorldComp>();
    if (renderType && rotationType && visibilityType && l2wType) {
        const uint64_t renderVersion = world.componentVersion(*renderType);
        const uint64_t rotationVersion = world.componentVersion(*rotationType);
        const uint64_t visibilityVersion = world.componentVersion(*visibilityType);
        const uint64_t localToWorldVersion = world.componentVersion(*l2wType);
        if (cached_.runComputeStage &&
            renderVersion == lastRenderVersion_ &&
            rotationVersion == lastRotationVersion_ &&
            visibilityVersion == lastVisibilityVersion_ &&
            localToWorldVersion == lastLocalToWorldVersion_) {
            return cached_;
        }
        lastRenderVersion_ = renderVersion;
        lastRotationVersion_ = rotationVersion;
        lastVisibilityVersion_ = visibilityVersion;
        lastLocalToWorldVersion_ = localToWorldVersion;
    }

    FrameGraphInput output{};
    output.runTransferStage = true;
    output.runComputeStage = true;

    std::vector<ChunkInput> chunkInputs{};
    world.query<const RenderComp, const RotationComp, const VisibilityComp, const LocalToWorldComp>().eachChunk(
        [&](const Entity* entities, const RenderComp* render, const RotationComp* rotation, const VisibilityComp* visibility, const LocalToWorldComp* localToWorld, size_t count) {
            chunkInputs.push_back(ChunkInput{ .entities = entities, .render = render, .rotation = rotation, .visibility = visibility, .localToWorld = localToWorld, .count = count });
        });

    PersistentExtractWorkers& workers = extractWorkers();
    std::vector<WorkerOutput> workerOutputs(workers.workerCount());

    workers.run([&](size_t worker) {
        WorkerOutput& local = workerOutputs[worker];
        for (size_t i = worker; i < chunkInputs.size(); i += workers.workerCount()) {
            cullAndEmitChunk(chunkInputs[i], local);
        }
    });

    std::unordered_map<uint32_t, RenderViewPacket> viewMap{};
    std::vector<DrawBuildPacket> pendingDraws{};
    for (const WorkerOutput& payload : workerOutputs) {
        for (const ViewBuildPacket& view : payload.views) {
            if (view.hasOverride) {
                viewMap.insert_or_assign(view.viewId, RenderViewPacket{ .viewId = view.viewId, .clearColor = view.clearColor });
            }
            else if (!viewMap.contains(view.viewId)) {
                viewMap.emplace(view.viewId, RenderViewPacket{ .viewId = view.viewId });
            }
        }
        pendingDraws.insert(pendingDraws.end(), payload.draws.begin(), payload.draws.end());
    }

    output.views.reserve(viewMap.size());
    for (const auto& [_, view] : viewMap) {
        output.views.push_back(view);
    }
    std::ranges::sort(output.views, {}, &RenderViewPacket::viewId);

    binMaterials(pendingDraws, output);

    cached_ = output;
    return output;
}
