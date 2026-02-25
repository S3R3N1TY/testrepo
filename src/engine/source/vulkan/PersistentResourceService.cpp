#include "PersistentResourceService.h"

namespace {
template <typename TDestroyFn, typename TBinding>
void destroyIfValid(const TDestroyFn& destroy, const TBinding& binding)
{
    if (destroy) {
        destroy(binding);
    }
}
}

void PersistentResourceService::upsertImage(Handle handle, VkImage image, const VkImageSubresourceRange& subresourceRange)
{
    std::scoped_lock lock(mutex_);
    images_.insert_or_assign(handle, ImageBinding{ .image = image, .subresourceRange = subresourceRange });
}

void PersistentResourceService::removeImage(Handle handle)
{
    std::scoped_lock lock(mutex_);
    images_.erase(handle);
}

std::optional<PersistentResourceService::ImageBinding> PersistentResourceService::resolveImage(Handle handle) const
{
    std::scoped_lock lock(mutex_);
    const auto it = images_.find(handle);
    if (it == images_.end()) {
        return std::nullopt;
    }
    return it->second;
}

void PersistentResourceService::upsertBuffer(Handle handle, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size)
{
    std::scoped_lock lock(mutex_);
    buffers_.insert_or_assign(handle, BufferBinding{ .buffer = buffer, .offset = offset, .size = size });
}

void PersistentResourceService::removeBuffer(Handle handle)
{
    std::scoped_lock lock(mutex_);
    buffers_.erase(handle);
}

std::optional<PersistentResourceService::BufferBinding> PersistentResourceService::resolveBuffer(Handle handle) const
{
    std::scoped_lock lock(mutex_);
    const auto it = buffers_.find(handle);
    if (it == buffers_.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool PersistentResourceService::ensureOwnedImage(Handle handle, OwnedImageSpec spec)
{
    if (!spec.create) {
        return false;
    }

    const std::optional<ImageBinding> created = spec.create();
    if (!created.has_value() || created->image == VK_NULL_HANDLE) {
        return false;
    }

    std::scoped_lock lock(mutex_);
    if (const auto it = ownedImages_.find(handle); it != ownedImages_.end()) {
        destroyIfValid(it->second.spec.destroy, it->second.binding);
    }

    OwnedImageEntry entry{};
    entry.spec = std::move(spec);
    entry.binding = *created;
    entry.generation = 1;
    ownedImages_.insert_or_assign(handle, entry);
    images_.insert_or_assign(handle, entry.binding);
    return true;
}

bool PersistentResourceService::ensureOwnedBuffer(Handle handle, OwnedBufferSpec spec)
{
    if (!spec.create) {
        return false;
    }

    const std::optional<BufferBinding> created = spec.create();
    if (!created.has_value() || created->buffer == VK_NULL_HANDLE) {
        return false;
    }

    std::scoped_lock lock(mutex_);
    if (const auto it = ownedBuffers_.find(handle); it != ownedBuffers_.end()) {
        destroyIfValid(it->second.spec.destroy, it->second.binding);
    }

    OwnedBufferEntry entry{};
    entry.spec = std::move(spec);
    entry.binding = *created;
    entry.generation = 1;
    ownedBuffers_.insert_or_assign(handle, entry);
    buffers_.insert_or_assign(handle, entry.binding);
    return true;
}

bool PersistentResourceService::recreateOwnedImage(Handle handle)
{
    std::scoped_lock lock(mutex_);
    auto it = ownedImages_.find(handle);
    if (it == ownedImages_.end() || !it->second.spec.create) {
        return false;
    }

    destroyIfValid(it->second.spec.destroy, it->second.binding);
    const std::optional<ImageBinding> created = it->second.spec.create();
    if (!created.has_value() || created->image == VK_NULL_HANDLE) {
        images_.erase(handle);
        return false;
    }

    it->second.binding = *created;
    it->second.generation += 1;
    images_.insert_or_assign(handle, it->second.binding);
    return true;
}

bool PersistentResourceService::recreateOwnedBuffer(Handle handle)
{
    std::scoped_lock lock(mutex_);
    auto it = ownedBuffers_.find(handle);
    if (it == ownedBuffers_.end() || !it->second.spec.create) {
        return false;
    }

    destroyIfValid(it->second.spec.destroy, it->second.binding);
    const std::optional<BufferBinding> created = it->second.spec.create();
    if (!created.has_value() || created->buffer == VK_NULL_HANDLE) {
        buffers_.erase(handle);
        return false;
    }

    it->second.binding = *created;
    it->second.generation += 1;
    buffers_.insert_or_assign(handle, it->second.binding);
    return true;
}

void PersistentResourceService::releaseOwnedImage(Handle handle)
{
    std::scoped_lock lock(mutex_);
    auto it = ownedImages_.find(handle);
    if (it != ownedImages_.end()) {
        destroyIfValid(it->second.spec.destroy, it->second.binding);
        ownedImages_.erase(it);
    }
    images_.erase(handle);
}

void PersistentResourceService::releaseOwnedBuffer(Handle handle)
{
    std::scoped_lock lock(mutex_);
    auto it = ownedBuffers_.find(handle);
    if (it != ownedBuffers_.end()) {
        destroyIfValid(it->second.spec.destroy, it->second.binding);
        ownedBuffers_.erase(it);
    }
    buffers_.erase(handle);
}

void PersistentResourceService::clear()
{
    std::scoped_lock lock(mutex_);

    for (auto& [_, entry] : ownedImages_) {
        destroyIfValid(entry.spec.destroy, entry.binding);
    }
    for (auto& [_, entry] : ownedBuffers_) {
        destroyIfValid(entry.spec.destroy, entry.binding);
    }

    ownedImages_.clear();
    ownedBuffers_.clear();
    images_.clear();
    buffers_.clear();
}
