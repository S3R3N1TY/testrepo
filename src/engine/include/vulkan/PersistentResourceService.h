#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <unordered_map>

#include <vulkan/vulkan.h>

class PersistentResourceService {
public:
    using Handle = uint64_t;

    struct ImageBinding {
        VkImage image{ VK_NULL_HANDLE };
        VkImageSubresourceRange subresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    };

    struct BufferBinding {
        VkBuffer buffer{ VK_NULL_HANDLE };
        VkDeviceSize offset{ 0 };
        VkDeviceSize size{ VK_WHOLE_SIZE };
    };

    struct OwnedImageSpec {
        std::function<std::optional<ImageBinding>()> create{};
        std::function<void(const ImageBinding&)> destroy{};
    };

    struct OwnedBufferSpec {
        std::function<std::optional<BufferBinding>()> create{};
        std::function<void(const BufferBinding&)> destroy{};
    };

    void upsertImage(Handle handle, VkImage image, const VkImageSubresourceRange& subresourceRange);
    void removeImage(Handle handle);
    [[nodiscard]] std::optional<ImageBinding> resolveImage(Handle handle) const;

    void upsertBuffer(Handle handle, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size);
    void removeBuffer(Handle handle);
    [[nodiscard]] std::optional<BufferBinding> resolveBuffer(Handle handle) const;

    [[nodiscard]] bool ensureOwnedImage(Handle handle, OwnedImageSpec spec);
    [[nodiscard]] bool ensureOwnedBuffer(Handle handle, OwnedBufferSpec spec);
    [[nodiscard]] bool recreateOwnedImage(Handle handle);
    [[nodiscard]] bool recreateOwnedBuffer(Handle handle);
    void releaseOwnedImage(Handle handle);
    void releaseOwnedBuffer(Handle handle);

    void clear();

private:
    struct OwnedImageEntry {
        OwnedImageSpec spec{};
        ImageBinding binding{};
        uint64_t generation{ 0 };
    };

    struct OwnedBufferEntry {
        OwnedBufferSpec spec{};
        BufferBinding binding{};
        uint64_t generation{ 0 };
    };

    mutable std::mutex mutex_{};
    std::unordered_map<Handle, ImageBinding> images_{};
    std::unordered_map<Handle, BufferBinding> buffers_{};
    std::unordered_map<Handle, OwnedImageEntry> ownedImages_{};
    std::unordered_map<Handle, OwnedBufferEntry> ownedBuffers_{};
};
