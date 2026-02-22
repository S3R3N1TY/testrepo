#include <Engine.h>

#include <app/Simulation.h>

#include <cassert>

int main()
{
    Simulation sim{};

    SimulationFrameInput in{};
    in.deltaSeconds = 0.016F;
    in.frameIndex = 1;
    sim.tick(in);

    const FrameGraphInput frameA1 = sim.buildFrameGraphInput();
    const FrameGraphInput frameA2 = sim.buildFrameGraphInput();

    assert(frameA1.drawPackets.size() == frameA2.drawPackets.size());
    assert(frameA1.materialBatches.size() == frameA2.materialBatches.size());
    assert(frameA1.views.size() == frameA2.views.size());

    in.frameIndex = 2;
    sim.tick(in);
    const FrameGraphInput frameB = sim.buildFrameGraphInput();
    assert(frameB.views.size() == frameA1.views.size());

    bool sawRotationAdvance = false;
    const size_t minDraws = std::min(frameA1.drawPackets.size(), frameB.drawPackets.size());
    for (size_t i = 0; i < minDraws; ++i) {
        if (frameB.drawPackets[i].angleRadians != frameA1.drawPackets[i].angleRadians) {
            sawRotationAdvance = true;
            break;
        }
    }

    if (minDraws > 0) {
        assert(sawRotationAdvance);
    }

    return 0;
}
