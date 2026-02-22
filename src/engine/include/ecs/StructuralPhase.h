#pragma once

#include <cstdint>

enum class StructuralPlaybackPhase : uint8_t {
    PreSim = 0,
    PostSim,
    EndFrame,
    Count
};
