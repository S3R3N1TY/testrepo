#include <renderer/FileRuntimeAssetBackend.h>

#include <fstream>
#include <sstream>

FileRuntimeAssetBackend::FileRuntimeAssetBackend(std::vector<std::string> candidatePaths)
    : candidatePaths_(std::move(candidatePaths))
{
}

std::vector<std::string> FileRuntimeAssetBackend::splitCsv(const std::string& line)
{
    std::vector<std::string> fields{};
    std::stringstream stream(line);
    std::string field;
    while (std::getline(stream, field, ',')) {
        fields.push_back(field);
    }
    return fields;
}

std::optional<RuntimeAssetBackend::BootstrapSnapshot> FileRuntimeAssetBackend::fetchBootstrapSnapshot()
{
    // Development fallback only: production should use an external runtime authority backend.
    for (const std::string& path : candidatePaths_) {
        std::ifstream file(path);
        if (!file.is_open()) {
            continue;
        }

        BootstrapSnapshot snapshot{};
        std::string line;
        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#') {
                continue;
            }
            const auto fields = splitCsv(line);
            if (fields.empty()) {
                continue;
            }

            if (fields[0] == "mesh" && fields.size() == 5) {
                const auto id = static_cast<uint32_t>(std::stoul(fields[1]));
                const auto vertexCount = static_cast<uint32_t>(std::stoul(fields[2]));
                const auto firstVertex = static_cast<uint32_t>(std::stoul(fields[3]));
                const auto generation = static_cast<uint64_t>(std::stoull(fields[4]));
                if (id != 0 && vertexCount != 0) {
                    snapshot.meshes.push_back(MeshRecord{
                        .id = id,
                        .vertexCount = vertexCount,
                        .firstVertex = firstVertex,
                        .generation = generation,
                        .residency = Residency::Ready
                    });
                }
                continue;
            }

            if (fields[0] == "material" && fields.size() == 3) {
                const auto id = static_cast<uint32_t>(std::stoul(fields[1]));
                const auto generation = static_cast<uint64_t>(std::stoull(fields[2]));
                if (id != 0) {
                    snapshot.materials.push_back(MaterialRecord{
                        .id = id,
                        .generation = generation,
                        .residency = Residency::Ready
                    });
                }
                continue;
            }
        }

        if (snapshot.meshes.empty() || snapshot.materials.empty()) {
            continue;
        }

        contentRevision_ += 1;
        snapshot.contentRevision = contentRevision_;
        return snapshot;
    }

    return std::nullopt;
}

std::vector<RuntimeAssetBackend::AssetEvent> FileRuntimeAssetBackend::fetchEvents(uint64_t, size_t)
{
    return {};
}
