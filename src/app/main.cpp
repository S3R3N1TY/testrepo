#include <engine/Engine.h>

#include <cmath>

namespace {

class SpinningTriangleGame final : public IGameSimulation {
public:
    void tick(const SimulationFrameInput& input) override
    {
        angleRadians_ += input.deltaSeconds;
        angleRadians_ = std::fmod(angleRadians_, 6.283185307F);
    }

    [[nodiscard]] FrameGraphInput buildFrameGraphInput() const override
    {
        FrameGraphInput input{};
        input.runTransferStage = true;
        input.runComputeStage = true;
        input.views.push_back(RenderViewPacket{ .viewId = 0, .clearColor = { 0.02F, 0.02F, 0.08F, 1.0F } });
        input.materialBatches.push_back(MaterialBatchPacket{ .materialId = 1, .firstDrawPacket = 0, .drawPacketCount = 1 });
        input.drawPackets.push_back(DrawPacket{
            .viewId = 0,
            .materialId = 1,
            .vertexCount = 3,
            .firstVertex = 0,
            .angleRadians = angleRadians_
            });
        return input;
    }

private:
    float angleRadians_{ 0.0F };
};

} // namespace

int main()
{
    SpinningTriangleGame game;
    Engine engine;
    engine.run(game);
}
