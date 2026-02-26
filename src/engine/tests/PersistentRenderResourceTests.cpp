#include <vulkan/PersistentResourceService.h>
#include <vulkan/RenderGraph.h>

#include <cassert>
#include <cstdint>

int main()
{
    PersistentResourceService service{};
    RenderTaskGraph graph{};

    constexpr PersistentResourceService::Handle kImageHandle = 42u;

    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.baseMipLevel = 0;
    range.levelCount = 1;
    range.baseArrayLayer = 0;
    range.layerCount = 1;

    assert(!service.resolveImage(kImageHandle).has_value());

    service.upsertImage(kImageHandle, reinterpret_cast<VkImage>(0x1), range);
    auto binding = service.resolveImage(kImageHandle);
    assert(binding.has_value());
    assert(binding->image == reinterpret_cast<VkImage>(0x1));
    const auto firstId = graph.createImageResource(binding->image, binding->subresourceRange, VK_IMAGE_LAYOUT_UNDEFINED);
    assert(firstId != 0);

    service.upsertImage(kImageHandle, reinterpret_cast<VkImage>(0x2), range);
    binding = service.resolveImage(kImageHandle);
    assert(binding.has_value());
    assert(binding->image == reinterpret_cast<VkImage>(0x2));

    // Owned persistent lifecycle semantics: create/recreate/release.
    uint32_t imageCreateCount = 0;
    uint32_t imageDestroyCount = 0;
    PersistentResourceService::OwnedImageSpec ownedImageSpec{};
    ownedImageSpec.create = [&]() -> std::optional<PersistentResourceService::ImageBinding> {
        ++imageCreateCount;
        PersistentResourceService::ImageBinding created{};
        created.image = reinterpret_cast<VkImage>(static_cast<uintptr_t>(0x1000u + imageCreateCount));
        created.subresourceRange = range;
        return created;
    };
    ownedImageSpec.destroy = [&](const PersistentResourceService::ImageBinding&) {
        ++imageDestroyCount;
    };

    const bool ownedImageCreated = service.ensureOwnedImage(100u, std::move(ownedImageSpec));
    assert(ownedImageCreated);
    auto ownedImage = service.resolveImage(100u);
    assert(ownedImage.has_value());
    assert(ownedImage->image == reinterpret_cast<VkImage>(0x1001));

    const bool ownedImageRecreated = service.recreateOwnedImage(100u);
    assert(ownedImageRecreated);
    ownedImage = service.resolveImage(100u);
    assert(ownedImage.has_value());
    assert(ownedImage->image == reinterpret_cast<VkImage>(0x1002));
    service.releaseOwnedImage(100u);
    assert(!service.resolveImage(100u).has_value());
    assert(imageCreateCount == 2);
    assert(imageDestroyCount == 2);

    uint32_t bufferCreateCount = 0;
    uint32_t bufferDestroyCount = 0;
    PersistentResourceService::OwnedBufferSpec ownedBufferSpec{};
    ownedBufferSpec.create = [&]() -> std::optional<PersistentResourceService::BufferBinding> {
        ++bufferCreateCount;
        PersistentResourceService::BufferBinding created{};
        created.buffer = reinterpret_cast<VkBuffer>(static_cast<uintptr_t>(0x2000u + bufferCreateCount));
        created.offset = 16;
        created.size = 64;
        return created;
    };
    ownedBufferSpec.destroy = [&](const PersistentResourceService::BufferBinding&) {
        ++bufferDestroyCount;
    };

    const bool ownedBufferCreated = service.ensureOwnedBuffer(200u, std::move(ownedBufferSpec));
    assert(ownedBufferCreated);
    const bool ownedBufferRecreated = service.recreateOwnedBuffer(200u);
    assert(ownedBufferRecreated);
    service.releaseOwnedBuffer(200u);
    assert(bufferCreateCount == 2);
    assert(bufferDestroyCount == 2);

    service.removeImage(kImageHandle);
    assert(!service.resolveImage(kImageHandle).has_value());

    constexpr PersistentResourceService::Handle kBufferHandle = 7u;
    service.upsertBuffer(kBufferHandle, reinterpret_cast<VkBuffer>(0x4), 16, 64);
    const auto bufferBinding = service.resolveBuffer(kBufferHandle);
    assert(bufferBinding.has_value());
    assert(bufferBinding->offset == 16);
    assert(bufferBinding->size == 64);
    service.removeBuffer(kBufferHandle);
    assert(!service.resolveBuffer(kBufferHandle).has_value());

    return 0;
}
