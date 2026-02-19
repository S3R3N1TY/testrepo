#include <cstring>
#include <vector>

#include "VkShaderModule.h"
#include "VkUtils.h"
 #include "DeferredDeletionService.h"
VulkanShaderModule::VulkanShaderModule(VkDevice device, const std::vector<char>& spirv)
    : handle()
{
    if (device == VK_NULL_HANDLE) {
        throw std::runtime_error("VulkanShaderModule: device is VK_NULL_HANDLE");
    }
    if (spirv.empty()) {
        throw std::runtime_error("VulkanShaderModule: empty SPIR-V bytecode");
    }
    if ((spirv.size() % 4u) != 0u) {
        throw std::runtime_error("VulkanShaderModule: SPIR-V size is not a multiple of 4 bytes");
    }

    std::vector<uint32_t> alignedSpirv(spirv.size() / sizeof(uint32_t));
    std::memcpy(alignedSpirv.data(), spirv.data(), spirv.size());

    VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = spirv.size();
    ci.pCode = alignedSpirv.data();

    VkShaderModule mod = VK_NULL_HANDLE;
    const VkResult res = vkCreateShaderModule(device, &ci, nullptr, &mod);
    if (res != VK_SUCCESS) {
        vkutil::throwVkError("vkCreateShaderModule", res);
    }

    handle = DeferredDeletionService::instance().makeDeferredHandle<VkShaderModule, PFN_vkDestroyShaderModule>(device, mod, vkDestroyShaderModule);
}
