#include <renderer/HttpRuntimeAssetBackend.h>

#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <utility>

namespace {
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

[[nodiscard]] FILE* openPipe(const std::string& command)
{
#if defined(_WIN32)
    return _popen(command.c_str(), "r");
#else
    return popen(command.c_str(), "r");
#endif
}

[[nodiscard]] int closePipe(FILE* pipe)
{
#if defined(_WIN32)
    return _pclose(pipe);
#else
    return pclose(pipe);
#endif
}

[[nodiscard]] std::vector<std::string> splitCsv(const std::string& line)
{
    std::vector<std::string> fields{};
    std::stringstream stream(line);
    std::string field;
    while (std::getline(stream, field, ',')) {
        fields.push_back(field);
    }
    return fields;
}

[[nodiscard]] std::optional<uint32_t> parseUint32(const std::string& s)
{
    try {
        return static_cast<uint32_t>(std::stoul(s));
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<uint64_t> parseUint64(const std::string& s)
{
    try {
        return static_cast<uint64_t>(std::stoull(s));
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] bool isSafeToken(const std::string& value)
{
    if (value.empty()) {
        return false;
    }

    for (const unsigned char ch : value) {
        if (std::isalnum(ch)) {
            continue;
        }
        switch (ch) {
        case '-':
        case '_':
        case '.':
        case '~':
        case '+':
        case '/':
        case '=':
            continue;
        default:
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool isSafeUrl(const std::string& url)
{
    if (url.empty()) {
        return false;
    }

    for (const unsigned char ch : url) {
        if (std::isalnum(ch)) {
            continue;
        }
        switch (ch) {
        case '-':
        case '_':
        case '.':
        case '~':
        case ':':
        case '/':
        case '?':
        case '#':
        case '[':
        case ']':
        case '@':
        case '!':
        case '$':
        case '&':
        case '\'':
        case '(':
        case ')':
        case '*':
        case '+':
        case ',':
        case ';':
        case '=':
        case '%':
            continue;
        default:
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::optional<RuntimeAssetBackend::EventKind> parseEventKind(const std::string& token)
{
    if (token == "UpsertMesh") {
        return RuntimeAssetBackend::EventKind::UpsertMesh;
    }
    if (token == "UpsertMaterial") {
        return RuntimeAssetBackend::EventKind::UpsertMaterial;
    }
    if (token == "RemoveMesh") {
        return RuntimeAssetBackend::EventKind::RemoveMesh;
    }
    if (token == "RemoveMaterial") {
        return RuntimeAssetBackend::EventKind::RemoveMaterial;
    }
    if (token == "UpdateMeshResidency") {
        return RuntimeAssetBackend::EventKind::UpdateMeshResidency;
    }
    if (token == "UpdateMaterialResidency") {
        return RuntimeAssetBackend::EventKind::UpdateMaterialResidency;
    }

    const auto numeric = parseUint32(token);
    if (!numeric.has_value()) {
        return std::nullopt;
    }

    switch (static_cast<RuntimeAssetBackend::EventKind>(*numeric)) {
    case RuntimeAssetBackend::EventKind::UpsertMesh:
    case RuntimeAssetBackend::EventKind::UpsertMaterial:
    case RuntimeAssetBackend::EventKind::RemoveMesh:
    case RuntimeAssetBackend::EventKind::RemoveMaterial:
    case RuntimeAssetBackend::EventKind::UpdateMeshResidency:
    case RuntimeAssetBackend::EventKind::UpdateMaterialResidency:
        return static_cast<RuntimeAssetBackend::EventKind>(*numeric);
    }

    return std::nullopt;
}
} // namespace

HttpRuntimeAssetBackend::HttpRuntimeAssetBackend(std::string bootstrapUrl, std::string eventsUrl)
    : bootstrapUrl_(std::move(bootstrapUrl))
    , eventsUrl_(std::move(eventsUrl))
{
}

std::optional<std::string> HttpRuntimeAssetBackend::httpGet(const std::string& url) const
{
    if (!isSafeUrl(url)) {
        return std::nullopt;
    }

    std::string headerArgs;
    if (const auto authToken = readEnvVar("RUNTIME_ASSET_BEARER_TOKEN"); authToken.has_value()) {
        const std::string& token = *authToken;
        if (isSafeToken(token)) {
            headerArgs = " -H \"Authorization: Bearer " + token + "\"";
        }
    }

    const std::string cmd =
        "curl -fsSL --retry 3 --retry-delay 1 --retry-all-errors --connect-timeout 2 --max-time 5" + headerArgs +
        " \"" + url + "\"";
    FILE* pipe = openPipe(cmd);
    if (pipe == nullptr) {
        return std::nullopt;
    }

    std::string out{};
    std::array<char, 256> buffer{};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        out.append(buffer.data());
    }

    const int rc = closePipe(pipe);
    if (rc != 0 || out.empty()) {
        return std::nullopt;
    }

    return out;
}

std::optional<RuntimeAssetBackend::BootstrapSnapshot> HttpRuntimeAssetBackend::parseBootstrapPayload(const std::string& payload) const
{
    BootstrapSnapshot snapshot{};
    std::stringstream stream(payload);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }

        const auto fields = splitCsv(line);
        if (fields.empty()) {
            continue;
        }

        if (fields[0] == "revision" && fields.size() == 2) {
            if (const auto revision = parseUint64(fields[1]); revision.has_value()) {
                snapshot.contentRevision = *revision;
            }
            continue;
        }

        if (fields[0] == "mesh" && fields.size() >= 6) {
            const auto id = parseUint32(fields[1]);
            const auto vertexCount = parseUint32(fields[2]);
            const auto firstVertex = parseUint32(fields[3]);
            const auto generation = parseUint64(fields[4]);
            const auto residencyRaw = parseUint32(fields[5]);
            if (!id.has_value() || !vertexCount.has_value() || !firstVertex.has_value() || !generation.has_value() || !residencyRaw.has_value()) {
                continue;
            }

            snapshot.meshes.push_back(MeshRecord{
                .id = *id,
                .vertexCount = *vertexCount,
                .firstVertex = *firstVertex,
                .generation = *generation,
                .residency = static_cast<Residency>(*residencyRaw),
                .error = fields.size() > 6 ? fields[6] : ""
            });
            continue;
        }

        if (fields[0] == "material" && fields.size() >= 4) {
            const auto id = parseUint32(fields[1]);
            const auto generation = parseUint64(fields[2]);
            const auto residencyRaw = parseUint32(fields[3]);
            if (!id.has_value() || !generation.has_value() || !residencyRaw.has_value()) {
                continue;
            }

            snapshot.materials.push_back(MaterialRecord{
                .id = *id,
                .generation = *generation,
                .residency = static_cast<Residency>(*residencyRaw),
                .error = fields.size() > 4 ? fields[4] : ""
            });
            continue;
        }
    }

    if (snapshot.contentRevision == 0 || snapshot.meshes.empty() || snapshot.materials.empty()) {
        return std::nullopt;
    }
    return snapshot;
}

std::vector<RuntimeAssetBackend::AssetEvent> HttpRuntimeAssetBackend::parseEventPayload(const std::string& payload) const
{
    std::vector<AssetEvent> events{};
    std::stringstream stream(payload);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }

        const auto fields = splitCsv(line);
        if (fields.size() < 4 || fields[0] != "event") {
            continue;
        }

        const auto revision = parseUint64(fields[1]);
        const auto kind = parseEventKind(fields[2]);
        const auto assetId = parseUint32(fields[3]);
        if (!revision.has_value() || !kind.has_value() || !assetId.has_value()) {
            continue;
        }

        AssetEvent event{};
        event.revision = *revision;
        event.kind = *kind;
        event.assetId = *assetId;

        switch (event.kind) {
        case EventKind::UpsertMesh:
            if (fields.size() < 8) {
                continue;
            }
            if (auto vertexCount = parseUint32(fields[4]); vertexCount.has_value()) {
                if (auto firstVertex = parseUint32(fields[5]); firstVertex.has_value()) {
                    if (auto generation = parseUint64(fields[6]); generation.has_value()) {
                        if (auto residency = parseUint32(fields[7]); residency.has_value()) {
                            event.mesh = MeshRecord{
                                .id = *assetId,
                                .vertexCount = *vertexCount,
                                .firstVertex = *firstVertex,
                                .generation = *generation,
                                .residency = static_cast<Residency>(*residency),
                                .error = fields.size() > 8 ? fields[8] : ""
                            };
                        }
                    }
                }
            }
            if (!event.mesh.has_value()) {
                continue;
            }
            break;
        case EventKind::UpsertMaterial:
            if (fields.size() < 7) {
                continue;
            }
            if (auto generation = parseUint64(fields[4]); generation.has_value()) {
                if (auto residency = parseUint32(fields[5]); residency.has_value()) {
                    event.material = MaterialRecord{
                        .id = *assetId,
                        .generation = *generation,
                        .residency = static_cast<Residency>(*residency),
                        .error = fields.size() > 6 ? fields[6] : ""
                    };
                }
            }
            if (!event.material.has_value()) {
                continue;
            }
            break;
        case EventKind::UpdateMeshResidency:
        case EventKind::UpdateMaterialResidency:
            if (fields.size() < 5) {
                continue;
            }
            if (const auto residency = parseUint32(fields[4]); residency.has_value()) {
                event.residency = static_cast<Residency>(*residency);
                event.error = fields.size() > 5 ? fields[5] : "";
            } else {
                continue;
            }
            break;
        case EventKind::RemoveMesh:
        case EventKind::RemoveMaterial:
            if (fields.size() > 4) {
                event.error = fields[4];
            }
            break;
        }

        events.push_back(std::move(event));
    }

    return events;
}

std::optional<RuntimeAssetBackend::BootstrapSnapshot> HttpRuntimeAssetBackend::fetchBootstrapSnapshot()
{
    const auto payload = httpGet(bootstrapUrl_);
    if (!payload.has_value()) {
        return std::nullopt;
    }
    return parseBootstrapPayload(*payload);
}

std::vector<RuntimeAssetBackend::AssetEvent> HttpRuntimeAssetBackend::fetchEvents(uint64_t afterRevision, size_t maxEvents)
{
    std::stringstream url{};
    url << eventsUrl_ << "?after=" << afterRevision << "&limit=" << maxEvents;

    const auto payload = httpGet(url.str());
    if (!payload.has_value()) {
        return {};
    }
    return parseEventPayload(*payload);
}
