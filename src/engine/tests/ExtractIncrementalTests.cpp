#include <app/ecs/systems/RenderExtractSys.h>

#include <app/ecs/components/RenderComp.h>
#include <app/ecs/components/RotationComp.h>
#include <app/ecs/components/VisibilityComp.h>
#include <app/ecs/components/LocalToWorldComp.h>

#include <ecs/World.h>

#include <cassert>

int main()
{
    World world{};
    Entity e = world.createEntity();
    world.emplaceComponent<RenderComp>(e);
    world.emplaceComponent<RotationComp>(e);
    world.emplaceComponent<VisibilityComp>(e);
    world.emplaceComponent<LocalToWorldComp>(e);

    RenderExtractSys extract{};
    (void)extract.build(world);
    assert(extract.lastRebuiltChunkCount() >= 1);

    (void)extract.build(world);
    assert(extract.lastReusedChunkCount() >= 1);

    world.beginSystemWriteScope();
    world.query<RotationComp>().each([](Entity, WriteRef<RotationComp> rot) {
        rot.touch();
        rot.get().angleRadians += 1.0F;
    });
    world.endSystemWriteScope();

    (void)extract.build(world);
    assert(extract.lastRebuiltChunkCount() >= 1);

    return 0;
}
