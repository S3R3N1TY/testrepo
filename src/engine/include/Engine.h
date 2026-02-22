#pragma once

#include <array>
#include <cstdint>
#include <vector>

struct SimulationFrameInput {
    float deltaSeconds{ 0.0F };
    uint64_t frameIndex{ 0 };
};

struct RenderViewPacket {
    uint32_t viewId{ 0 };
    std::array<float, 4> clearColor{ 0.02F, 0.02F, 0.08F, 1.0F };
};

struct MaterialBatchPacket {
    uint32_t materialId{ 0 };
    uint32_t firstDrawPacket{ 0 };
    uint32_t drawPacketCount{ 0 };
};

struct DrawPacket {
    uint32_t viewId{ 0 };
    uint32_t materialId{ 0 };
    uint32_t vertexCount{ 3 };
    uint32_t firstVertex{ 0 };
    float angleRadians{ 0.0F };
    std::array<float, 3> worldPosition{ 0.0F, 0.0F, 0.0F };
    uint32_t worldEntityId{ 0 };
};

struct FrameGraphInput {
    std::vector<RenderViewPacket> views{};
    std::vector<MaterialBatchPacket> materialBatches{};
    std::vector<DrawPacket> drawPackets{};
    bool runTransferStage{ true };
    bool runComputeStage{ true };
};

class IGameSimulation {
public:
    virtual ~IGameSimulation() = default;
    virtual void tick(const SimulationFrameInput& input) = 0;
    [[nodiscard]] virtual FrameGraphInput buildFrameGraphInput() const = 0;
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
