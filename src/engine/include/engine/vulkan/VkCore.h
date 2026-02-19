#pragma once

#include <vector>
#include <string>
#include <stdexcept>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility> // std::exchange

// parasoft-begin-suppress ALL "suppress all violations"
#include <vulkan/vulkan.h>
// parasoft-end-suppress ALL "suppress all violations"

#include "VkUtils.h"
struct GLFWwindow;

// ===================== Queue families =====================

struct VulkanQueueFamilies {
    uint32_t graphicsFamily = UINT32_MAX;
    uint32_t presentFamily = UINT32_MAX;
    uint32_t computeFamily = UINT32_MAX;
    uint32_t transferFamily = UINT32_MAX;

    [[nodiscard]] bool complete() const noexcept {
        return graphicsFamily != UINT32_MAX &&
            presentFamily != UINT32_MAX;
    }

    [[nodiscard]] bool hasTransfer() const noexcept {
        return transferFamily != UINT32_MAX;
    }

    [[nodiscard]] bool hasCompute() const noexcept {
        return computeFamily != UINT32_MAX;
    }
};


struct DeviceFeaturePolicy {
    enum class Requirement : uint8_t { Disabled = 0, Optional, Required, Experimental };

    Requirement timelineSemaphore{ Requirement::Required };
    Requirement dynamicRendering{ Requirement::Optional };
    Requirement synchronization2{ Requirement::Optional };
    Requirement descriptorIndexing{ Requirement::Optional };
    Requirement bufferDeviceAddress{ Requirement::Optional };

    std::vector<const char*> requiredExtensions{};
    std::vector<const char*> optionalExtensions{};
    std::vector<const char*> experimentalExtensions{};
    std::vector<const char*> disabledExtensions{};

    [[nodiscard]] static DeviceFeaturePolicy engineDefault();
};

struct VulkanDeviceCapabilities {
    enum class FeatureProvisionSource : uint8_t {
        Disabled = 0,
        Core,
        Extension
    };

    struct RuntimeContract {
        uint32_t loaderApiVersion{ VK_API_VERSION_1_0 };
        uint32_t instanceApiVersion{ VK_API_VERSION_1_0 };
        uint32_t physicalDeviceApiVersion{ VK_API_VERSION_1_0 };
        uint32_t negotiatedApiVersion{ VK_API_VERSION_1_0 };

        FeatureProvisionSource timelineSemaphoreSource{ FeatureProvisionSource::Disabled };
        FeatureProvisionSource dynamicRenderingSource{ FeatureProvisionSource::Disabled };
        FeatureProvisionSource synchronization2Source{ FeatureProvisionSource::Disabled };
        FeatureProvisionSource descriptorIndexingSource{ FeatureProvisionSource::Disabled };
        FeatureProvisionSource bufferDeviceAddressSource{ FeatureProvisionSource::Disabled };
    };

    VkPhysicalDeviceFeatures coreFeatures{};

    bool timelineSemaphoreSupported = false;
    bool dynamicRenderingSupported = false;
    bool synchronization2Supported = false;
    bool descriptorIndexingSupported = false;
    bool bufferDeviceAddressSupported = false;

    bool timelineSemaphoreEnabled = false;
    bool dynamicRenderingEnabled = false;
    bool synchronization2Enabled = false;
    bool descriptorIndexingEnabled = false;
    bool bufferDeviceAddressEnabled = false;

    VkPhysicalDeviceFeatures2 enabledFeatures2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES };
    VkPhysicalDeviceDynamicRenderingFeatures dynamicRenderingFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES };
    VkPhysicalDeviceSynchronization2Features synchronization2Features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES };
    VkPhysicalDeviceDescriptorIndexingFeatures descriptorIndexingFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES };
    VkPhysicalDeviceBufferDeviceAddressFeatures bufferDeviceAddressFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES };

    std::vector<const char*> enabledExtensions;
    RuntimeContract runtimeContract{};
};

struct VulkanInstanceCapabilityProfile {
    uint32_t loaderApiVersion{ VK_API_VERSION_1_0 };
    uint32_t requestedApiVersion{ VK_API_VERSION_1_0 };
    uint32_t negotiatedApiVersion{ VK_API_VERSION_1_0 };
    bool portabilityEnumerationEnabled{ false };
};

struct VulkanDeviceDispatch {
    PFN_vkQueueSubmit2 queueSubmit2{ nullptr };
    PFN_vkCmdPipelineBarrier2 cmdPipelineBarrier2{ nullptr };
    PFN_vkCmdWaitEvents2 cmdWaitEvents2{ nullptr };
    PFN_vkCmdWriteTimestamp2 cmdWriteTimestamp2{ nullptr };

    [[nodiscard]] bool hasSynchronization2() const noexcept {
        return queueSubmit2 != nullptr;
    }
};

// ===================== Instance =====================

class VulkanInstance {
public:
    VulkanInstance() noexcept = default;

    explicit VulkanInstance(const std::vector<const char*>& requiredExtensions,
        bool enableValidationLayers = false);

    VulkanInstance(const VulkanInstance&) = delete;
    VulkanInstance& operator=(const VulkanInstance&) = delete;

    VulkanInstance(VulkanInstance&& other) noexcept;
    VulkanInstance& operator=(VulkanInstance&& other) noexcept;

    ~VulkanInstance() noexcept;

    [[nodiscard]] VkInstance get() const noexcept { return instance; }
    [[nodiscard]] bool       valid() const noexcept { return instance != VK_NULL_HANDLE; }
    [[nodiscard]] const VulkanInstanceCapabilityProfile& capabilities() const noexcept { return capabilityProfile; }

    void reset() noexcept;

private:
    VkInstance instance{ VK_NULL_HANDLE };
    VulkanInstanceCapabilityProfile capabilityProfile{};

    [[nodiscard]] static bool layerAvailable(const char* name);
    [[nodiscard]] static bool instanceExtensionAvailable(const char* name);
    [[nodiscard]] static bool listContains(const std::vector<const char*>& lst, const char* needle);

    static void ensureExtensionsAvailable(const std::vector<const char*>& exts);

    void createInstance(const std::vector<const char*>& requiredExtensions,
        bool enableValidationLayers);
};

// ===================== Debug messenger =====================

class VulkanDebugUtilsMessenger {
public:
    VulkanDebugUtilsMessenger() noexcept = default;
    explicit VulkanDebugUtilsMessenger(VkInstance instance);

    VulkanDebugUtilsMessenger(const VulkanDebugUtilsMessenger&) = delete;
    VulkanDebugUtilsMessenger& operator=(const VulkanDebugUtilsMessenger&) = delete;

    VulkanDebugUtilsMessenger(VulkanDebugUtilsMessenger&& other) noexcept;
    VulkanDebugUtilsMessenger& operator=(VulkanDebugUtilsMessenger&& other) noexcept;

    ~VulkanDebugUtilsMessenger() noexcept;

    [[nodiscard]] VkDebugUtilsMessengerEXT get() const noexcept { return debugMessenger; }
    [[nodiscard]] bool                     valid() const noexcept { return debugMessenger != VK_NULL_HANDLE; }

    void reset() noexcept;

private:
    VkInstance               instance{ VK_NULL_HANDLE };
    VkDebugUtilsMessengerEXT debugMessenger{ VK_NULL_HANDLE };

    PFN_vkCreateDebugUtilsMessengerEXT  pfnCreate{ nullptr };
    PFN_vkDestroyDebugUtilsMessengerEXT pfnDestroy{ nullptr };

    static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT type,
        const VkDebugUtilsMessengerCallbackDataEXT* data,
        void* userData);
};

// ===================== Surface =====================

class VulkanSurface {
public:
    VulkanSurface() noexcept = default;
    VulkanSurface(VkInstance instance, GLFWwindow* window);

    VulkanSurface(const VulkanSurface&) = delete;
    VulkanSurface& operator=(const VulkanSurface&) = delete;

    VulkanSurface(VulkanSurface&& other) noexcept;
    VulkanSurface& operator=(VulkanSurface&& other) noexcept;

    ~VulkanSurface() noexcept;

    [[nodiscard]] VkSurfaceKHR get() const noexcept { return surface; }
    [[nodiscard]] bool         valid() const noexcept { return surface != VK_NULL_HANDLE; }

    void reset() noexcept;

private:
    VkInstance   instance{ VK_NULL_HANDLE };
    VkSurfaceKHR surface{ VK_NULL_HANDLE };
};

// ===================== Physical device =====================

class VulkanPhysicalDevice {
public:
    VulkanPhysicalDevice(VkInstance instance,
        VkSurfaceKHR surface,
        const std::vector<const char*>& requiredDeviceExtensions,
        DeviceFeaturePolicy policy = DeviceFeaturePolicy::engineDefault(),
        uint32_t instanceApiVersion = VK_API_VERSION_1_0);

    VulkanPhysicalDevice(const VulkanPhysicalDevice&) = delete;
    VulkanPhysicalDevice& operator=(const VulkanPhysicalDevice&) = delete;

    ~VulkanPhysicalDevice() = default; // non-owning

    [[nodiscard]] VkPhysicalDevice get() const noexcept { return physicalDevice; }
    [[nodiscard]] const VulkanQueueFamilies& queues() const noexcept { return families; }

    [[nodiscard]] bool hasPortabilitySubset() const noexcept { return portabilitySubsetAvailable; }
    [[nodiscard]] const VulkanDeviceCapabilities& capabilities() const noexcept { return deviceCapabilities; }

    void getProperties(VkPhysicalDeviceProperties& out) const;
    void getFeatures(VkPhysicalDeviceFeatures& out) const;

private:
    VkInstance               instance{ VK_NULL_HANDLE };
    VkSurfaceKHR             surface{ VK_NULL_HANDLE };
    std::vector<const char*> requiredExtensions;
    DeviceFeaturePolicy featurePolicy{};
    uint32_t instanceApiVersion{ VK_API_VERSION_1_0 };

    VkPhysicalDevice    physicalDevice{ VK_NULL_HANDLE };
    VulkanQueueFamilies families{};

    bool portabilitySubsetAvailable = false;
    VulkanDeviceCapabilities deviceCapabilities{};

    void                pickPhysicalDevice();
    [[nodiscard]] bool isSuitable(VkPhysicalDevice candidate) const;
    [[nodiscard]] bool evaluatePolicyRequirement(DeviceFeaturePolicy::Requirement requirement, bool supported) const;
    [[nodiscard]] bool checkExtensions(VkPhysicalDevice candidate) const;
    void                findQueueFamilies(VkPhysicalDevice candidate, VulkanQueueFamilies& out) const;
    [[nodiscard]] bool                hasSwapchainSupport(VkPhysicalDevice candidate) const;
    void                queryOptionalSupport(VkPhysicalDevice candidate);
    [[nodiscard]] int64_t scoreDevice(VkPhysicalDevice candidate, VulkanQueueFamilies& outFamilies) const;
};

// ===================== Logical device =====================

class VulkanDevice {
public:
    VulkanDevice() noexcept = default;

    VulkanDevice(VkPhysicalDevice physicalDevice,
        const VulkanQueueFamilies& queueFamilies,
        const VulkanDeviceCapabilities& capabilities,
        DeviceFeaturePolicy policy = DeviceFeaturePolicy::engineDefault());

    VulkanDevice(const VulkanDevice&) = delete;
    VulkanDevice& operator=(const VulkanDevice&) = delete;

    VulkanDevice(VulkanDevice&& other) noexcept;
    VulkanDevice& operator=(VulkanDevice&& other) noexcept;

    ~VulkanDevice() noexcept;

    [[nodiscard]] VkDevice get() const noexcept { return device; }
    [[nodiscard]] VkQueue  getGraphicsQueue() const noexcept { return graphicsQueue; }
    [[nodiscard]] VkQueue  getPresentQueue()  const noexcept { return presentQueue; }
    [[nodiscard]] VkQueue  getTransferQueue() const noexcept { return transferQueue; }
    [[nodiscard]] VkQueue  getComputeQueue() const noexcept { return computeQueue; }

    [[nodiscard]] uint32_t graphicsFamilyIndex() const noexcept { return graphicsFamily; }
    [[nodiscard]] uint32_t presentFamilyIndex()  const noexcept { return presentFamily; }
    [[nodiscard]] uint32_t transferFamilyIndex() const noexcept { return transferFamily; }
    [[nodiscard]] uint32_t computeFamilyIndex() const noexcept { return computeFamily; }

    [[nodiscard]] bool valid() const noexcept { return device != VK_NULL_HANDLE; }
    [[nodiscard]] const VulkanDeviceDispatch& dispatch() const noexcept { return dispatchTable; }
    [[nodiscard]] bool synchronization2Enabled() const noexcept { return synchronization2EnabledFlag; }

    void reset() noexcept;

private:
    VkDevice device{ VK_NULL_HANDLE };
    VkQueue  graphicsQueue{ VK_NULL_HANDLE };
    VkQueue  presentQueue{ VK_NULL_HANDLE };
    VkQueue  transferQueue{ VK_NULL_HANDLE };
    VkQueue  computeQueue{ VK_NULL_HANDLE };
    uint32_t graphicsFamily{ UINT32_MAX };
    uint32_t presentFamily{ UINT32_MAX };
    uint32_t transferFamily{ UINT32_MAX };
    uint32_t computeFamily{ UINT32_MAX };
    bool synchronization2EnabledFlag{ false };
    VulkanDeviceDispatch dispatchTable{};
};

// ===================== VulkanQueue =====================

class VulkanQueue {
public:
    VulkanQueue() noexcept = default;
    VulkanQueue(VkDevice device,
        uint32_t queueFamilyIndex,
        uint32_t queueIndex = 0,
        bool synchronization2Enabled = false,
        PFN_vkQueueSubmit2 queueSubmit2 = nullptr);

    VulkanQueue(const VulkanQueue&) = default;
    VulkanQueue& operator=(const VulkanQueue&) = default;

    ~VulkanQueue() = default;

    [[nodiscard]] VkQueue get() const noexcept { return queue; }
    [[nodiscard]] uint32_t familyIndex() const noexcept { return queueFamilyIndex; }
    [[nodiscard]] bool    valid() const noexcept { return queue != VK_NULL_HANDLE; }

    [[nodiscard]] vkutil::VkExpected<void> submit(const std::vector<VkSubmitInfo>& submitInfos,
        VkFence fence = VK_NULL_HANDLE,
        const char* subsystem = "queue") const;

    [[nodiscard]] vkutil::VkExpected<void> submit2(const std::vector<VkSubmitInfo2>& submitInfos,
        VkFence fence = VK_NULL_HANDLE,
        const char* subsystem = "queue") const;

    [[nodiscard]] VkResult present(VkSwapchainKHR swapchain,
        uint32_t imageIndex,
        VkSemaphore waitSemaphore = VK_NULL_HANDLE) const;

    [[nodiscard]] VkResult present(VkSwapchainKHR swapchain,
        uint32_t imageIndex,
        const std::vector<VkSemaphore>& waitSemaphores) const;

    [[nodiscard]] vkutil::VkExpected<void> waitIdle() const;

private:
    VkDevice device{ VK_NULL_HANDLE };
    VkQueue  queue{ VK_NULL_HANDLE };
    uint32_t queueFamilyIndex{ UINT32_MAX };
    std::shared_ptr<std::mutex> queueMutex{};
    bool synchronization2Enabled{ false };
    PFN_vkQueueSubmit2 pfnQueueSubmit2{ nullptr };
};
