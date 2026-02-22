#pragma once

#include <Engine.h>
#include <ecs/World.h>

class RenderExtractSys final {
public:
    [[nodiscard]] FrameGraphInput build(const World& world) const;

private:
    mutable FrameGraphInput cached_{};
    mutable uint64_t lastRenderVersion_{ 0 };
    mutable uint64_t lastRotationVersion_{ 0 };
    mutable uint64_t lastVisibilityVersion_{ 0 };
    mutable uint64_t lastLocalToWorldVersion_{ 0 };
};
