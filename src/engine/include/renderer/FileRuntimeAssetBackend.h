#pragma once

#include <renderer/RuntimeAssetBackend.h>

#include <string>
#include <vector>

class FileRuntimeAssetBackend final : public RuntimeAssetBackend {
public:
    explicit FileRuntimeAssetBackend(std::vector<std::string> candidatePaths);

    [[nodiscard]] std::optional<BootstrapSnapshot> fetchBootstrapSnapshot() override;
    [[nodiscard]] std::vector<AssetEvent> fetchEvents(uint64_t afterRevision, size_t maxEvents) override;

private:
    [[nodiscard]] static std::vector<std::string> splitCsv(const std::string& line);

    std::vector<std::string> candidatePaths_{};
    uint64_t contentRevision_{ 0 };
};
