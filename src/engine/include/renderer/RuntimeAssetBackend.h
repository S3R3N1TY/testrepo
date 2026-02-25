#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class RuntimeAssetBackend {
public:
    enum class Residency : uint8_t {
        Ready,
        Loading,
        Failed,
        Evicted
    };

    struct MeshRecord {
        uint32_t id{ 0 };
        uint32_t vertexCount{ 0 };
        uint32_t firstVertex{ 0 };
        uint64_t generation{ 0 };
        Residency residency{ Residency::Ready };
        std::string error{};
    };

    struct MaterialRecord {
        uint32_t id{ 0 };
        uint64_t generation{ 0 };
        Residency residency{ Residency::Ready };
        std::string error{};
    };

    struct BootstrapSnapshot {
        uint64_t contentRevision{ 0 };
        std::vector<MeshRecord> meshes{};
        std::vector<MaterialRecord> materials{};
    };

    enum class EventKind : uint8_t {
        UpsertMesh,
        UpsertMaterial,
        RemoveMesh,
        RemoveMaterial,
        UpdateMeshResidency,
        UpdateMaterialResidency
    };

    struct AssetEvent {
        uint64_t revision{ 0 };
        EventKind kind{ EventKind::UpsertMesh };
        uint32_t assetId{ 0 };
        std::optional<MeshRecord> mesh{};
        std::optional<MaterialRecord> material{};
        Residency residency{ Residency::Ready };
        std::string error{};
    };

    virtual ~RuntimeAssetBackend() = default;
    [[nodiscard]] virtual std::optional<BootstrapSnapshot> fetchBootstrapSnapshot() = 0;
    [[nodiscard]] virtual std::vector<AssetEvent> fetchEvents(uint64_t afterRevision, size_t maxEvents) = 0;
};
