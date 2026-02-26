#include "RenderExtractSys.h"

#include "../components/RenderComp.h"
#include "../components/RotationComp.h"

#include <algorithm>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <unordered_map>
#include <utility>
#include <vector>

FrameGraphInput RenderExtractSys::build(const World& world) const
{
    FrameGraphInput output{};
    output.runTransferStage = true;
    output.runComputeStage = true;

    std::unordered_map<uint32_t, RenderViewPacket> viewMap{};

    struct DrawBuildPacket {
        Entity entity{};
        DrawPacket draw{};
    };

    std::vector<DrawBuildPacket> pendingDraws{};

    const glm::mat4 projection = glm::perspective(glm::radians(55.0F), 800.0F / 600.0F, 0.1F, 100.0F);
    const glm::mat4 view3D = glm::lookAt(glm::vec3(0.0F, 0.0F, 3.5F), glm::vec3(0.0F), glm::vec3(0.0F, 1.0F, 0.0F));

    world.query<RenderComp, RotationComp>().each([&](Entity entity, const RenderComp& render, const RotationComp& rotation) {
        if (!render.visible) {
            return;
        }

        if (render.overrideClearColor) {
            viewMap.insert_or_assign(render.viewId, RenderViewPacket{ .viewId = render.viewId, .clearColor = render.clearColor });
        }
        else if (!viewMap.contains(render.viewId)) {
            viewMap.emplace(render.viewId, RenderViewPacket{ .viewId = render.viewId });
        }

        glm::mat4 mvp(1.0F);
        if (render.materialId == 3) {
            const glm::mat4 model = glm::rotate(glm::mat4(1.0F), rotation.angleRadians, glm::vec3(0.1F, 1.0F, 0.0F));
            const glm::mat4 clipFix = glm::scale(glm::mat4(1.0F), glm::vec3(1.0F, -1.0F, 1.0F));
            mvp = clipFix * projection * view3D * model;
        } else {
            mvp = glm::rotate(glm::mat4(1.0F), rotation.angleRadians, glm::vec3(0.0F, 0.0F, 1.0F));
        }

        std::array<float, 16> mvpPacked{};
        const float* mvpData = glm::value_ptr(mvp);
        std::copy(mvpData, mvpData + mvpPacked.size(), mvpPacked.begin());

        pendingDraws.push_back(DrawBuildPacket{
            .entity = entity,
            .draw = DrawPacket{
                .viewId = render.viewId,
                .materialId = render.materialId,
                .vertexCount = render.vertexCount,
                .firstVertex = render.firstVertex,
                .mvp = mvpPacked }
            });
    });

    output.views.reserve(viewMap.size());
    for (const auto& [_, view] : viewMap) {
        output.views.push_back(view);
    }
    std::ranges::sort(output.views, {}, &RenderViewPacket::viewId);

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

    return output;
}
