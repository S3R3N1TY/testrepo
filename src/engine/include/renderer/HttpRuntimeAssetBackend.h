#pragma once

#include <renderer/RuntimeAssetBackend.h>

#include <string>

class HttpRuntimeAssetBackend final : public RuntimeAssetBackend {
public:
    HttpRuntimeAssetBackend(std::string bootstrapUrl, std::string eventsUrl);

    [[nodiscard]] std::optional<BootstrapSnapshot> fetchBootstrapSnapshot() override;
    [[nodiscard]] std::vector<AssetEvent> fetchEvents(uint64_t afterRevision, size_t maxEvents) override;

private:
    [[nodiscard]] std::optional<std::string> httpGet(const std::string& url) const;
    [[nodiscard]] std::optional<BootstrapSnapshot> parseBootstrapPayload(const std::string& payload) const;
    [[nodiscard]] std::vector<AssetEvent> parseEventPayload(const std::string& payload) const;

    std::string bootstrapUrl_{};
    std::string eventsUrl_{};
};
