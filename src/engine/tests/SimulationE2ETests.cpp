#include <Engine.h>

#include <app/Simulation.h>
#include <renderer/RuntimeAssetService.h>

#include <algorithm>
#include <cassert>

int main()
{
    auto& assetService = RuntimeAssetService::instance();
    assert(assetService.initializeDefaultBackend());
    assert(assetService.refreshFromBackend());

    Simulation sim{};

    SimulationFrameInput in{};
    in.deltaSeconds = 0.016F;
    in.frameIndex = 1;
    sim.tick(in);

    const RenderWorldSnapshot frameA1 = sim.buildRenderSnapshot();
    const RenderWorldSnapshot frameA2 = sim.buildRenderSnapshot();

    assert(frameA1.instances.size() == frameA2.instances.size());
    assert(frameA1.views.size() == frameA2.views.size());
    assert(!frameA1.lights.empty());

    const RenderAssetCatalogSnapshot catalogA = assetService.snapshot(in.frameIndex);
    assert(catalogA.simulationFrameIndex == in.frameIndex);
    assert(!catalogA.meshes.empty());
    assert(!catalogA.materials.empty());
    in.frameIndex = 2;
    sim.tick(in);
    const RenderWorldSnapshot frameB = sim.buildRenderSnapshot();
    const RenderAssetCatalogSnapshot catalogB = assetService.snapshot(in.frameIndex);
    assert(frameB.views.size() == frameA1.views.size());
    assert(catalogB.simulationFrameIndex == in.frameIndex);
    assert(catalogB.meshes.size() == catalogA.meshes.size());
    assert(catalogB.materials.size() == catalogA.materials.size());

    bool sawTransformAdvance = false;
    const size_t minDraws = std::min(frameA1.instances.size(), frameB.instances.size());
    for (size_t i = 0; i < minDraws; ++i) {
        if (frameB.instances[i].localToWorld != frameA1.instances[i].localToWorld) {
            sawTransformAdvance = true;
            break;
        }
    }

    if (minDraws > 0) {
        assert(sawTransformAdvance);
    }

    return 0;
}
