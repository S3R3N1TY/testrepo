#include "Simulation.h"

#include "assets/GlbLoader.h"

#include <imgui.h>

namespace {
std::vector<VertexPacket> buildBaseVertices()
{
    return {
        VertexPacket{ .position = { 0.0F, -0.5F, 0.0F }, .color = { 1.0F, 0.0F, 0.0F } },
        VertexPacket{ .position = { 0.5F, 0.5F, 0.0F }, .color = { 0.0F, 1.0F, 0.0F } },
        VertexPacket{ .position = { -0.5F, 0.5F, 0.0F }, .color = { 0.0F, 0.0F, 1.0F } },

        VertexPacket{ .position = { -0.5F, -0.5F, 0.0F }, .color = { 1.0F, 0.0F, 0.0F } },
        VertexPacket{ .position = { 0.5F, -0.5F, 0.0F }, .color = { 0.0F, 1.0F, 0.0F } },
        VertexPacket{ .position = { 0.5F, 0.5F, 0.0F }, .color = { 0.0F, 0.0F, 1.0F } },
        VertexPacket{ .position = { 0.5F, 0.5F, 0.0F }, .color = { 0.0F, 0.0F, 1.0F } },
        VertexPacket{ .position = { -0.5F, 0.5F, 0.0F }, .color = { 1.0F, 1.0F, 0.0F } },
        VertexPacket{ .position = { -0.5F, -0.5F, 0.0F }, .color = { 1.0F, 0.0F, 0.0F } },
    };
}
}

Simulation::Simulation()
    : vertexPackets_(buildBaseVertices())
{
    const LoadedMesh sphereMesh = appendGlbMeshVertices("assets/models/Sphere.glb", vertexPackets_);

    scenes_.emplace_back(std::make_unique<SpinningTriangleScene>());
    scenes_.emplace_back(std::make_unique<SpinningSquareScene>());
    scenes_.emplace_back(std::make_unique<SphereScene>(sphereMesh.firstVertex, sphereMesh.vertexCount));
    switchToScene(0);
}

void Simulation::switchToScene(size_t sceneIndex)
{
    if (sceneIndex >= scenes_.size()) {
        return;
    }

    if (hasActiveScene_) {
        scenes_[activeSceneIndex_]->onUnload(world_);
    }
    activeSceneIndex_ = sceneIndex;
    scenes_[activeSceneIndex_]->onLoad(world_);
    hasActiveScene_ = true;
    frameGraphDirty_ = true;
}

void Simulation::drawMainMenuBar()
{
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }

    if (ImGui::BeginMenu("Scenario")) {
        for (size_t i = 0; i < scenes_.size(); ++i) {
            const bool selected = (i == activeSceneIndex_);
            if (ImGui::MenuItem(scenes_[i]->name(), nullptr, selected)) {
                switchToScene(i);
            }
        }
        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();
}

void Simulation::tick(const SimulationFrameInput& input)
{
    scenes_[activeSceneIndex_]->onUpdate(world_, input);
    spinningSys_.update(world_, input);
    scenes_[activeSceneIndex_]->onDraw(world_);
    frameGraphDirty_ = true;
}

FrameGraphInput Simulation::buildFrameGraphInput() const
{
    if (frameGraphDirty_) {
        cachedFrameGraphInput_ = renderExtractSys_.build(world_);
        cachedFrameGraphInput_.vertexPackets = vertexPackets_;
        frameGraphDirty_ = false;
    }
    return cachedFrameGraphInput_;
}
