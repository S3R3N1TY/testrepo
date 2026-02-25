#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

struct SimulationFrameInput {
    float deltaSeconds{ 0.0F };
    uint64_t frameIndex{ 0 };
};

struct RenderMaterialRef {
    uint32_t id{ 0 };
};

struct RenderMeshRef {
    uint32_t id{ 0 };
    uint32_t vertexCount{ 3 };
    uint32_t firstVertex{ 0 };
};

struct RenderViewSnapshot {
    uint32_t viewId{ 0 };
    std::array<float, 4> clearColor{ 0.02F, 0.02F, 0.08F, 1.0F };
};

struct RenderInstanceSnapshot {
    uint64_t instanceId{ 0 };
    uint32_t viewId{ 0 };
    uint32_t entityId{ 0 };
    std::array<float, 16> localToWorld{
        1.0F, 0.0F, 0.0F, 0.0F,
        0.0F, 1.0F, 0.0F, 0.0F,
        0.0F, 0.0F, 1.0F, 0.0F,
        0.0F, 0.0F, 0.0F, 1.0F
    };
    RenderMeshRef mesh{};
    RenderMaterialRef material{};
    uint32_t visibilityMask{ 0xFFFFFFFFu };
    struct Bounds {
        std::array<float, 3> center{ 0.0F, 0.0F, 0.0F };
        float radius{ 0.0F };
    };
    std::optional<Bounds> worldBounds{};
};


struct RenderMaterialGroupSnapshot {
    uint32_t materialId{ 0 };
    uint32_t firstInstance{ 0 };
    uint32_t instanceCount{ 0 };
};

struct RenderLightSnapshot {
    uint32_t lightId{ 0 };
    std::array<float, 3> worldPosition{ 0.0F, 0.0F, 0.0F };
    float intensity{ 1.0F };
};

struct RenderMeshCatalogEntry {
    uint32_t id{ 0 };
    uint32_t vertexCount{ 0 };
    uint32_t firstVertex{ 0 };
    uint64_t generation{ 0 };
};

struct RenderMaterialCatalogEntry {
    uint32_t id{ 0 };
    uint64_t generation{ 0 };
};

struct RenderAssetCatalogSnapshot {
    uint64_t simulationFrameIndex{ 0 };
    std::vector<RenderMeshCatalogEntry> meshes{};
    std::vector<RenderMaterialCatalogEntry> materials{};
};

struct RenderWorldSnapshot {
    uint64_t simulationFrameIndex{ 0 };
    std::vector<RenderViewSnapshot> views{};
    std::vector<RenderInstanceSnapshot> instances{};
    std::vector<RenderMaterialGroupSnapshot> materialGroups{};
    std::vector<RenderLightSnapshot> lights{};
};

class IGameSimulation {
public:
    virtual ~IGameSimulation() = default;
    virtual void tick(const SimulationFrameInput& input) = 0;
    [[nodiscard]] virtual RenderWorldSnapshot buildRenderSnapshot() const = 0;
};

class Engine
{
public:
    struct RunConfig {
        uint32_t windowWidth{ 800 };
        uint32_t windowHeight{ 600 };
        const char* windowTitle{ "Wrapper Vulkan Triangle" };
        bool enableValidation{ true };
        const char* vertexShaderPath{ nullptr };
        const char* fragmentShaderPath{ nullptr };
    };

    void run(IGameSimulation& game, const RunConfig& config);
    void run(IGameSimulation& game);
};
