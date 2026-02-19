#pragma once

#include <engine/Engine.h>
#include <engine/ecs/World.h>

class RenderExtractSys final {
public:
    [[nodiscard]] FrameGraphInput build(const World& world) const;
};
