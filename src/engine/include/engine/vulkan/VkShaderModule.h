#pragma once

#include <vector>
#include <stdexcept>
#include <cstddef> // std::size_t

// parasoft-begin-suppress ALL "suppress all violations"
#include <vulkan/vulkan.h>
// parasoft-end-suppress ALL "suppress all violations"

 #include "UniqueHandle.h"
// Owns VkShaderModule (device-owned)
class VulkanShaderModule {
public:
    VulkanShaderModule() noexcept = default;

    VulkanShaderModule(VkDevice device, const std::vector<char>& spirv);

    VulkanShaderModule(const VulkanShaderModule&) = delete;
    VulkanShaderModule& operator=(const VulkanShaderModule&) = delete;

    VulkanShaderModule(VulkanShaderModule&&) noexcept = default;
    VulkanShaderModule& operator=(VulkanShaderModule&&) noexcept = default;

    ~VulkanShaderModule() = default;

    [[nodiscard]] VkShaderModule get() const noexcept { return handle.get(); }
    [[nodiscard]] VkDevice       getDevice() const noexcept { return handle.getDevice(); }
    [[nodiscard]] bool           valid() const noexcept { return static_cast<bool>(handle); }

private:
    vkhandle::DeviceUniqueHandle<VkShaderModule, PFN_vkDestroyShaderModule> handle;
};
