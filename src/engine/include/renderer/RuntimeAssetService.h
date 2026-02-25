#pragma once

#include <Engine.h>
#include <renderer/RuntimeAssetBackend.h>

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

class RuntimeAssetService {
public:
    struct MeshRecord {
        uint32_t id{ 0 };
        uint32_t vertexCount{ 0 };
        uint32_t firstVertex{ 0 };
        uint64_t generation{ 0 };
        RuntimeAssetBackend::Residency residency{ RuntimeAssetBackend::Residency::Ready };
        std::string error{};
    };

    struct MaterialRecord {
        uint32_t id{ 0 };
        uint64_t generation{ 0 };
        RuntimeAssetBackend::Residency residency{ RuntimeAssetBackend::Residency::Ready };
        std::string error{};
    };

    static RuntimeAssetService& instance();

    void configureBackend(std::shared_ptr<RuntimeAssetBackend> backend);
    [[nodiscard]] bool initializeDefaultBackend();
    [[nodiscard]] bool refreshFromBackend();

    [[nodiscard]] std::optional<MeshRecord> resolveMesh(uint32_t id) const;
    [[nodiscard]] std::optional<MaterialRecord> resolveMaterial(uint32_t id) const;

    [[nodiscard]] RenderAssetCatalogSnapshot snapshot(uint64_t simulationFrameIndex) const;

private:
    RuntimeAssetService() = default;
    [[nodiscard]] bool tryBootstrapLocked(const std::shared_ptr<RuntimeAssetBackend>& backend);

    mutable std::mutex mutex_{};
    std::shared_ptr<RuntimeAssetBackend> backend_{};
    uint64_t backendRevision_{ 0 };
    std::unordered_map<uint32_t, MeshRecord> meshes_{};
    std::unordered_map<uint32_t, MaterialRecord> materials_{};
};
