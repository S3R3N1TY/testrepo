#include <renderer/RuntimeAssetService.h>

#include <renderer/FileRuntimeAssetBackend.h>
#include <renderer/HttpRuntimeAssetBackend.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <thread>
#include <unordered_map>

namespace {
constexpr std::array<const char*, 2> kDefaultAssetDatabaseCandidates{
    "content/render_assets.db",
    "src/content/render_assets.db"
};

constexpr const char* kDefaultHttpBootstrapUrl = "http://127.0.0.1:8080/runtime-assets/bootstrap";
constexpr const char* kDefaultHttpEventsUrl = "http://127.0.0.1:8080/runtime-assets/events";
constexpr size_t kMaxBackendEventsPerRefresh = 1024;
constexpr uint32_t kBackendRetryCount = 3;
constexpr std::chrono::milliseconds kBackendRetryBackoff{ 25 };

[[nodiscard]] std::optional<std::string> readEnvVar(const char* key)
{
#if defined(_MSC_VER)
    char* buffer = nullptr;
    size_t size = 0;
    const errno_t rc = _dupenv_s(&buffer, &size, key);
    if (rc != 0 || buffer == nullptr || size == 0 || buffer[0] == '\0') {
        if (buffer != nullptr) {
            free(buffer);
        }
        return std::nullopt;
    }

    std::string value{ buffer };
    free(buffer);
    return value;
#else
    if (const char* value = std::getenv(key); value != nullptr && value[0] != '\0') {
        return std::string{ value };
    }
    return std::nullopt;
#endif
}

[[nodiscard]] std::string envOrDefault(const char* key, const char* fallback)
{
    if (const auto value = readEnvVar(key); value.has_value()) {
        return *value;
    }
    return fallback;
}

[[nodiscard]] bool envIsTrue(const char* key)
{
    if (const auto value = readEnvVar(key); value.has_value()) {
        const std::string& v = *value;
        return v == "1" || v == "true" || v == "TRUE" || v == "yes" || v == "on";
    }
    return false;
}

[[nodiscard]] bool envIsProductionMode()
{
    if (const auto value = readEnvVar("RUNTIME_ASSET_MODE"); value.has_value()) {
        const std::string& v = *value;
        return v == "prod" || v == "production";
    }
    return false;
}

[[nodiscard]] std::vector<std::string> buildAssetDatabaseCandidates()
{
    std::vector<std::string> paths{};
    paths.reserve(kDefaultAssetDatabaseCandidates.size() + 6);

    if (const auto explicitDbPath = readEnvVar("RUNTIME_ASSET_DB_PATH"); explicitDbPath.has_value()) {
        paths.push_back(*explicitDbPath);
    }

    for (const char* candidate : kDefaultAssetDatabaseCandidates) {
        paths.emplace_back(candidate);
    }

    const std::filesystem::path sourceFile{ __FILE__ };
    const std::filesystem::path sourceTreeDb = sourceFile.parent_path().parent_path().parent_path() / "content" / "render_assets.db";
    paths.push_back(sourceTreeDb.lexically_normal().string());

    const std::filesystem::path cwd = std::filesystem::current_path();
    paths.push_back((cwd / "content" / "render_assets.db").lexically_normal().string());
    paths.push_back((cwd / "src" / "content" / "render_assets.db").lexically_normal().string());

    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    return paths;
}

[[nodiscard]] RuntimeAssetService::MeshRecord toMeshRecord(const RuntimeAssetBackend::MeshRecord& mesh)
{
    return RuntimeAssetService::MeshRecord{
        .id = mesh.id,
        .vertexCount = mesh.vertexCount,
        .firstVertex = mesh.firstVertex,
        .generation = mesh.generation,
        .residency = mesh.residency,
        .error = mesh.error
    };
}

[[nodiscard]] RuntimeAssetService::MaterialRecord toMaterialRecord(const RuntimeAssetBackend::MaterialRecord& material)
{
    return RuntimeAssetService::MaterialRecord{
        .id = material.id,
        .generation = material.generation,
        .residency = material.residency,
        .error = material.error
    };
}
}

RuntimeAssetService& RuntimeAssetService::instance()
{
    static RuntimeAssetService service{};
    return service;
}

void RuntimeAssetService::configureBackend(std::shared_ptr<RuntimeAssetBackend> backend)
{
    std::scoped_lock lock(mutex_);
    backend_ = std::move(backend);
    backendRevision_ = 0;
    meshes_.clear();
    materials_.clear();
}

bool RuntimeAssetService::initializeDefaultBackend()
{
    std::scoped_lock lock(mutex_);
    if (backend_) {
        return true;
    }

    const auto bootstrapUrl = envOrDefault("RUNTIME_ASSET_BOOTSTRAP_URL", kDefaultHttpBootstrapUrl);
    const auto eventsUrl = envOrDefault("RUNTIME_ASSET_EVENTS_URL", kDefaultHttpEventsUrl);

    auto httpBackend = std::make_shared<HttpRuntimeAssetBackend>(bootstrapUrl, eventsUrl);
    if (const auto snap = httpBackend->fetchBootstrapSnapshot(); snap.has_value()) {
        backend_ = std::move(httpBackend);
        return true;
    }

    if (envIsProductionMode() && !envIsTrue("RUNTIME_ASSET_ALLOW_FILE_FALLBACK")) {
        return false;
    }

    auto fileBackend = std::make_shared<FileRuntimeAssetBackend>(buildAssetDatabaseCandidates());
    if (!fileBackend->fetchBootstrapSnapshot().has_value()) {
        return false;
    }

    backend_ = std::move(fileBackend);
    return true;
}

bool RuntimeAssetService::tryBootstrapLocked(const std::shared_ptr<RuntimeAssetBackend>& backend)
{
    for (uint32_t attempt = 0; attempt < kBackendRetryCount; ++attempt) {
        const auto bootstrap = backend->fetchBootstrapSnapshot();
        if (bootstrap.has_value() && !bootstrap->meshes.empty() && !bootstrap->materials.empty() && bootstrap->contentRevision != 0) {
            if (backendRevision_ != 0 && bootstrap->contentRevision < backendRevision_) {
                std::this_thread::sleep_for(kBackendRetryBackoff * (attempt + 1));
                continue;
            }

            std::unordered_map<uint32_t, MeshRecord> newMeshes{};
            std::unordered_map<uint32_t, MaterialRecord> newMaterials{};
            for (const auto& mesh : bootstrap->meshes) {
                newMeshes.insert_or_assign(mesh.id, toMeshRecord(mesh));
            }
            for (const auto& material : bootstrap->materials) {
                newMaterials.insert_or_assign(material.id, toMaterialRecord(material));
            }

            backendRevision_ = bootstrap->contentRevision;
            meshes_ = std::move(newMeshes);
            materials_ = std::move(newMaterials);
            return true;
        }
        std::this_thread::sleep_for(kBackendRetryBackoff * (attempt + 1));
    }
    return false;
}

bool RuntimeAssetService::refreshFromBackend()
{
    std::shared_ptr<RuntimeAssetBackend> backend;
    {
        std::scoped_lock lock(mutex_);
        backend = backend_;
    }

    if (!backend) {
        return false;
    }

    std::scoped_lock lock(mutex_);
    if (backendRevision_ == 0) {
        return tryBootstrapLocked(backend);
    }

    std::vector<RuntimeAssetBackend::AssetEvent> events{};
    for (uint32_t attempt = 0; attempt < kBackendRetryCount; ++attempt) {
        events = backend->fetchEvents(backendRevision_, kMaxBackendEventsPerRefresh);
        if (!events.empty()) {
            break;
        }
        if (attempt + 1 < kBackendRetryCount) {
            std::this_thread::sleep_for(kBackendRetryBackoff * (attempt + 1));
        }
    }

    if (events.empty()) {
        return true;
    }

    uint64_t expectedRevision = backendRevision_;
    for (const auto& event : events) {
        if (event.revision <= expectedRevision) {
            continue;
        }
        if (event.revision != expectedRevision + 1) {
            return tryBootstrapLocked(backend);
        }

        switch (event.kind) {
        case RuntimeAssetBackend::EventKind::UpsertMesh:
            if (event.mesh.has_value()) {
                meshes_.insert_or_assign(event.assetId, toMeshRecord(*event.mesh));
            }
            break;
        case RuntimeAssetBackend::EventKind::UpsertMaterial:
            if (event.material.has_value()) {
                materials_.insert_or_assign(event.assetId, toMaterialRecord(*event.material));
            }
            break;
        case RuntimeAssetBackend::EventKind::RemoveMesh:
            meshes_.erase(event.assetId);
            break;
        case RuntimeAssetBackend::EventKind::RemoveMaterial:
            materials_.erase(event.assetId);
            break;
        case RuntimeAssetBackend::EventKind::UpdateMeshResidency:
            if (auto it = meshes_.find(event.assetId); it != meshes_.end()) {
                it->second.residency = event.residency;
                it->second.error = event.error;
            }
            break;
        case RuntimeAssetBackend::EventKind::UpdateMaterialResidency:
            if (auto it = materials_.find(event.assetId); it != materials_.end()) {
                it->second.residency = event.residency;
                it->second.error = event.error;
            }
            break;
        }

        expectedRevision = event.revision;
    }

    backendRevision_ = expectedRevision;
    return true;
}

std::optional<RuntimeAssetService::MeshRecord> RuntimeAssetService::resolveMesh(uint32_t id) const
{
    std::scoped_lock lock(mutex_);
    const auto it = meshes_.find(id);
    if (it == meshes_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<RuntimeAssetService::MaterialRecord> RuntimeAssetService::resolveMaterial(uint32_t id) const
{
    std::scoped_lock lock(mutex_);
    const auto it = materials_.find(id);
    if (it == materials_.end()) {
        return std::nullopt;
    }
    return it->second;
}

RenderAssetCatalogSnapshot RuntimeAssetService::snapshot(uint64_t simulationFrameIndex) const
{
    std::scoped_lock lock(mutex_);
    RenderAssetCatalogSnapshot catalog{};
    catalog.simulationFrameIndex = simulationFrameIndex;
    catalog.meshes.reserve(meshes_.size());
    catalog.materials.reserve(materials_.size());

    for (const auto& [_, mesh] : meshes_) {
        catalog.meshes.push_back(RenderMeshCatalogEntry{
            .id = mesh.id,
            .vertexCount = mesh.vertexCount,
            .firstVertex = mesh.firstVertex,
            .generation = mesh.generation
        });
    }
    std::ranges::sort(catalog.meshes, [](const RenderMeshCatalogEntry& lhs, const RenderMeshCatalogEntry& rhs) {
        return lhs.id < rhs.id;
    });

    for (const auto& [_, material] : materials_) {
        catalog.materials.push_back(RenderMaterialCatalogEntry{
            .id = material.id,
            .generation = material.generation
        });
    }
    std::ranges::sort(catalog.materials, [](const RenderMaterialCatalogEntry& lhs, const RenderMaterialCatalogEntry& rhs) {
        return lhs.id < rhs.id;
    });

    return catalog;
}
