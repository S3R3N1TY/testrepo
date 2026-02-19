#include "SpinningSys.h"

#include "../components/RotationComp.h"

#include <cmath>

namespace {
constexpr float kTwoPi = 6.283185307F;
}

void SpinningSys::update(World& world, const SimulationFrameInput& input) const
{
    world.query<RotationComp>().each([&](Entity, RotationComp& rotation) {
        rotation.angleRadians += input.deltaSeconds * rotation.angularVelocityRadiansPerSecond;
        rotation.angleRadians = std::fmod(rotation.angleRadians, kTwoPi);
        if (rotation.angleRadians < 0.0F) {
            rotation.angleRadians += kTwoPi;
        }
    });
}
