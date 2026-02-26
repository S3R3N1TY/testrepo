#pragma once

#include <Engine.h>

#include <cstdint>
#include <string>
#include <vector>

struct LoadedMesh {
    uint32_t firstVertex{ 0 };
    uint32_t vertexCount{ 0 };
};

LoadedMesh appendGlbMeshVertices(const std::string& path, std::vector<VertexPacket>& outVertices);
