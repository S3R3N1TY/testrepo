#pragma once

#include <Engine.h>
#include <ecs/World.h>

class RenderExtractSys final {
public:
    [[nodiscard]] FrameGraphInput build(const World& world) const;
};
