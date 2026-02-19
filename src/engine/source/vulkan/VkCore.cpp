#include "VkCore.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>
#include <stdexcept>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <limits>

 #include "VkUtils.h"

// parasoft-begin-suppress ALL "suppress all violations"
#include <GLFW/glfw3.h>
// parasoft-end-suppress ALL "suppress all violations"

namespace
{
    constexpr const char kNullString[] = "(null)";
    constexpr uint32_t makeVersion(uint32_t major, uint32_t minor, uint32_t patch) noexcept
    {
        return (major << 22) | (minor << 12) | patch;
    }

    struct QueueKey
    {
        VkDevice device{ VK_NULL_HANDLE };
        VkQueue queue{ VK_NULL_HANDLE };

        bool operator==(const QueueKey& other) const noexcept
        {
            return device == other.device && queue == other.queue;
        }
    };

    struct QueueKeyHash
    {
        std::size_t operator()(const QueueKey& key) const noexcept
        {
            return (static_cast<std::size_t>(reinterpret_cast<uintptr_t>(key.device)) << 1u) ^
                static_cast<std::size_t>(reinterpret_cast<uintptr_t>(key.queue));
        }
    };

    std::mutex& queueRegistryMutex()
    {
        static std::mutex m;
        return m;
    }

    std::unordered_map<QueueKey, std::weak_ptr<std::mutex>, QueueKeyHash>& queueRegistry()
    {
        static std::unordered_map<QueueKey, std::weak_ptr<std::mutex>, QueueKeyHash> reg;
        return reg;
    }

    std::shared_ptr<std::mutex> getSharedQueueMutex(VkDevice device, VkQueue queue)
    {
        const std::lock_guard<std::mutex> lock(queueRegistryMutex());
        auto& reg = queueRegistry();

        for (auto it = reg.begin(); it != reg.end();) {
            if (it->second.expired()) {
                it = reg.erase(it);
            }
            else {
                ++it;
            }
        }

        const QueueKey key{ device, queue };
        const auto it = reg.find(key);
        if (it != reg.end()) {
            if (auto existing = it->second.lock()) {
                return existing;
            }
            reg.erase(it);
        }

        auto created = std::make_shared<std::mutex>();
        reg[key] = created;
        return created;
    }

    uint32_t queryLoaderApiVersion() noexcept
    {
        uint32_t loaderVersion = VK_API_VERSION_1_0;
        const auto pfnEnumerateInstanceVersion =
            reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
                vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
        if (pfnEnumerateInstanceVersion != nullptr) {
            static_cast<void>(pfnEnumerateInstanceVersion(&loaderVersion));
        }
        return loaderVersion;
    }

    uint32_t negotiateInstanceApiVersion(uint32_t loaderVersion) noexcept
    {
        constexpr uint32_t kTargetVersion = VK_API_VERSION_1_3;
        return std::min(loaderVersion, kTargetVersion);
    }

    VulkanDeviceCapabilities::FeatureProvisionSource resolveFeatureSource(
        bool enabled,
        uint32_t negotiatedApiVersion,
        uint32_t coreVersionForFeature) noexcept
    {
        if (!enabled) {
            return VulkanDeviceCapabilities::FeatureProvisionSource::Disabled;
        }
        return negotiatedApiVersion >= coreVersionForFeature
            ? VulkanDeviceCapabilities::FeatureProvisionSource::Core
            : VulkanDeviceCapabilities::FeatureProvisionSource::Extension;
    }
}

// ================= Instance helpers =================

static bool cstrEq(const char* a, const char* b) {
    return a && b && std::strcmp(a, b) == 0;
}

static bool enumerateInstanceLayers(std::vector<VkLayerProperties>& out) {
    out.clear();
    uint32_t n = 0;
    const VkResult res = vkEnumerateInstanceLayerProperties(&n, nullptr);
    if (res != VK_SUCCESS) {
        return false;
    }
    out.resize(n);
    if (n != 0) {
        const VkResult res2 = vkEnumerateInstanceLayerProperties(&n, out.data());
        if (res2 != VK_SUCCESS) {
            out.clear();
            return false;
        }
    }
    return true;
}

static bool enumerateInstanceExtensions(std::vector<VkExtensionProperties>& out) {
    out.clear();
    uint32_t n = 0;
    const VkResult res = vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    if (res != VK_SUCCESS) {
        return false;
    }
    out.resize(n);
    if (n != 0) {
        const VkResult res2 = vkEnumerateInstanceExtensionProperties(nullptr, &n, out.data());
        if (res2 != VK_SUCCESS) {
            out.clear();
            return false;
        }
    }
    return true;
}



DeviceFeaturePolicy DeviceFeaturePolicy::engineDefault()
{
    DeviceFeaturePolicy policy{};
    policy.timelineSemaphore = DeviceFeaturePolicy::Requirement::Required;
    policy.dynamicRendering = DeviceFeaturePolicy::Requirement::Optional;
    policy.synchronization2 = DeviceFeaturePolicy::Requirement::Optional;
    policy.descriptorIndexing = DeviceFeaturePolicy::Requirement::Optional;
    policy.bufferDeviceAddress = DeviceFeaturePolicy::Requirement::Optional;
    policy.requiredExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    return policy;
}

// ===================== VulkanInstance =====================

bool VulkanInstance::layerAvailable(const char* name) {
    std::vector<VkLayerProperties> layers;
    if (!enumerateInstanceLayers(layers)) {
        return false;
    }
    for (const auto& lp : layers) {
        if (cstrEq(lp.layerName, name)) return true;
    }
    return false;
}

bool VulkanInstance::instanceExtensionAvailable(const char* name) {
    std::vector<VkExtensionProperties> extensions;
    if (!enumerateInstanceExtensions(extensions)) {
        return false;
    }
    for (const auto& ep : extensions) {
        if (cstrEq(ep.extensionName, name)) return true;
    }
    return false;
}

bool VulkanInstance::listContains(const std::vector<const char*>& lst, const char* needle) {
    return std::any_of(lst.begin(), lst.end(),
        [&](const char* s) { return cstrEq(s, needle); });
}

void VulkanInstance::ensureExtensionsAvailable(const std::vector<const char*>& exts) {
    for (const char* const e : exts) {
        if (!instanceExtensionAvailable(e)) {
            std::string msg = "VulkanInstance: required instance extension missing: ";
            msg += (e != nullptr ? e : kNullString);
            throw std::runtime_error(msg);
        }
    }
}

VulkanInstance::VulkanInstance(const std::vector<const char*>& requiredExtensions,
    bool enableValidationLayers)
    : instance(VK_NULL_HANDLE)
    , capabilityProfile{}
{
    createInstance(requiredExtensions, enableValidationLayers);
}

VulkanInstance::VulkanInstance(VulkanInstance&& other) noexcept
    : instance(std::exchange(other.instance, VK_NULL_HANDLE))
    , capabilityProfile(std::exchange(other.capabilityProfile, VulkanInstanceCapabilityProfile{}))
{
}

VulkanInstance& VulkanInstance::operator=(VulkanInstance&& other) noexcept {
    if (this != &other) {
        reset();
        instance = std::exchange(other.instance, VK_NULL_HANDLE);
        capabilityProfile = std::exchange(other.capabilityProfile, VulkanInstanceCapabilityProfile{});
    }
    return *this;
}

VulkanInstance::~VulkanInstance() noexcept {
    reset();
}

void VulkanInstance::reset() noexcept {
    if (instance != VK_NULL_HANDLE) {
        vkDestroyInstance(instance, nullptr);
        instance = VK_NULL_HANDLE;
    }
    capabilityProfile = VulkanInstanceCapabilityProfile{};
}

void VulkanInstance::createInstance(const std::vector<const char*>& requiredExtensions,
    bool enableValidationLayers)
{
    std::vector<const char*> exts = requiredExtensions;
    std::vector<const char*> layers{};

    if (enableValidationLayers) {
        if (!layerAvailable("VK_LAYER_KHRONOS_validation")) {
            throw std::runtime_error("VulkanInstance: validation enabled but VK_LAYER_KHRONOS_validation not available");
        }
        layers.push_back("VK_LAYER_KHRONOS_validation");

        if (!listContains(exts, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
    }

    // Optional but important on MoltenVK/macOS.
    const bool havePortabilityEnum = instanceExtensionAvailable(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    if (havePortabilityEnum && !listContains(exts, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    }

    // Validate extensions (including GLFW-provided)
    ensureExtensionsAvailable(exts);

    const uint32_t loaderApiVersion = queryLoaderApiVersion();
    const uint32_t negotiatedApiVersion = negotiateInstanceApiVersion(loaderApiVersion);

    VkApplicationInfo appInfo{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    appInfo.pApplicationName = "VulkanApp";
    appInfo.applicationVersion = makeVersion(1u, 0u, 0u);
    appInfo.pEngineName = "NoEngine";
    appInfo.engineVersion = makeVersion(1u, 0u, 0u);
    appInfo.apiVersion = negotiatedApiVersion;

    VkInstanceCreateInfo ci{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ci.pApplicationInfo = &appInfo;
    ci.enabledLayerCount = static_cast<uint32_t>(layers.size());
    ci.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();
    ci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    ci.ppEnabledExtensionNames = exts.empty() ? nullptr : exts.data();

    ci.flags = 0;
    if (havePortabilityEnum) {
        ci.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }

    const VkResult res = vkCreateInstance(&ci, nullptr, &instance);
    if (res != VK_SUCCESS) {
        vkutil::throwVkError("vkCreateInstance", res);
    }

    capabilityProfile.loaderApiVersion = loaderApiVersion;
    capabilityProfile.requestedApiVersion = makeVersion(1u, 3u, 0u);
    capabilityProfile.negotiatedApiVersion = negotiatedApiVersion;
    capabilityProfile.portabilityEnumerationEnabled = havePortabilityEnum;
}

// ===================== VulkanDebugUtilsMessenger =====================

VulkanDebugUtilsMessenger::VulkanDebugUtilsMessenger(VkInstance inst)
    : instance(inst)
    , debugMessenger(VK_NULL_HANDLE)
    , pfnCreate(nullptr)
    , pfnDestroy(nullptr)
{
    if (instance == VK_NULL_HANDLE) {
        throw std::runtime_error("VulkanDebugUtilsMessenger: instance is null");
    }

    pfnCreate =
        reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
    pfnDestroy =
        reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));

    if (!pfnCreate || !pfnDestroy) {
        throw std::runtime_error("VulkanDebugUtilsMessenger: failed to load debug utils messenger functions");
    }

    VkDebugUtilsMessengerCreateInfoEXT ci{ VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
    ci.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    ci.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    ci.pfnUserCallback = debugCallback;
    ci.pUserData = nullptr;

    const VkResult res = pfnCreate(instance, &ci, nullptr, &debugMessenger);
    if (res != VK_SUCCESS) {
        vkutil::throwVkError("vkCreateDebugUtilsMessengerEXT", res);
    }
}

VulkanDebugUtilsMessenger::VulkanDebugUtilsMessenger(VulkanDebugUtilsMessenger&& other) noexcept
    : instance(std::exchange(other.instance, VK_NULL_HANDLE))
    , debugMessenger(std::exchange(other.debugMessenger, VK_NULL_HANDLE))
    , pfnCreate(std::exchange(other.pfnCreate, nullptr))
    , pfnDestroy(std::exchange(other.pfnDestroy, nullptr))
{
}

VulkanDebugUtilsMessenger&
VulkanDebugUtilsMessenger::operator=(VulkanDebugUtilsMessenger&& other) noexcept
{
    if (this != &other) {
        reset();
        instance = std::exchange(other.instance, VK_NULL_HANDLE);
        debugMessenger = std::exchange(other.debugMessenger, VK_NULL_HANDLE);
        pfnCreate = std::exchange(other.pfnCreate, nullptr);
        pfnDestroy = std::exchange(other.pfnDestroy, nullptr);
    }
    return *this;
}

VulkanDebugUtilsMessenger::~VulkanDebugUtilsMessenger() noexcept {
    reset();
}

void VulkanDebugUtilsMessenger::reset() noexcept {
    if (debugMessenger != VK_NULL_HANDLE && pfnDestroy) {
        pfnDestroy(instance, debugMessenger, nullptr);
        debugMessenger = VK_NULL_HANDLE;
    }
    instance = VK_NULL_HANDLE;
    pfnCreate = nullptr;
    pfnDestroy = nullptr;
}

VKAPI_ATTR VkBool32 VKAPI_CALL VulkanDebugUtilsMessenger::debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*type*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* /*userData*/)
{
    // Keep it simple: warnings+errors are what you asked for.
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        const char* const msg = (data && data->pMessage) ? data->pMessage : kNullString;
        std::cerr << "[VK] " << msg << '\n';
    }
    return VK_FALSE;
}

// ===================== VulkanSurface =====================

VulkanSurface::VulkanSurface(VkInstance inst, GLFWwindow* window)
    : instance(inst)
    , surface(VK_NULL_HANDLE)
{
    if (instance == VK_NULL_HANDLE) {
        throw std::runtime_error("VulkanSurface: instance is null");
    }
    if (!window) {
        throw std::runtime_error("VulkanSurface: GLFWwindow is null");
    }

    const VkResult res = glfwCreateWindowSurface(instance, window, nullptr, &surface);
    if (res != VK_SUCCESS) {
        vkutil::throwVkError("glfwCreateWindowSurface", res);
    }
}

VulkanSurface::VulkanSurface(VulkanSurface&& other) noexcept
    : instance(std::exchange(other.instance, VK_NULL_HANDLE))
    , surface(std::exchange(other.surface, VK_NULL_HANDLE))
{
}

VulkanSurface& VulkanSurface::operator=(VulkanSurface&& other) noexcept {
    if (this != &other) {
        reset();
        instance = std::exchange(other.instance, VK_NULL_HANDLE);
        surface = std::exchange(other.surface, VK_NULL_HANDLE);
    }
    return *this;
}

VulkanSurface::~VulkanSurface() noexcept {
    reset();
}

void VulkanSurface::reset() noexcept {
    if (surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance, surface, nullptr);
        surface = VK_NULL_HANDLE;
    }
    instance = VK_NULL_HANDLE;
}

// ===================== VulkanPhysicalDevice =====================

VulkanPhysicalDevice::VulkanPhysicalDevice(VkInstance inst,
    VkSurfaceKHR surf,
    const std::vector<const char*>& requiredDeviceExtensions,
    DeviceFeaturePolicy policy,
    uint32_t instanceApiVersionIn)
    : instance(inst)
    , surface(surf)
    , requiredExtensions(requiredDeviceExtensions)
    , featurePolicy(std::move(policy))
    , instanceApiVersion(instanceApiVersionIn)
{
    if (instance == VK_NULL_HANDLE) {
        throw std::runtime_error("VulkanPhysicalDevice: instance is null");
    }
    if (surface == VK_NULL_HANDLE) {
        throw std::runtime_error("VulkanPhysicalDevice: surface is null");
    }

    pickPhysicalDevice();
}

void VulkanPhysicalDevice::pickPhysicalDevice() {
    uint32_t count = 0;
    const VkResult res = vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (res != VK_SUCCESS || count == 0) {
        throw std::runtime_error("VulkanPhysicalDevice: no GPUs with Vulkan support");
    }

    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());

    int64_t bestScore = std::numeric_limits<int64_t>::min();
    VkPhysicalDevice bestDevice = VK_NULL_HANDLE;
    VulkanQueueFamilies bestFamilies{};

    for (const auto dev : devices) {
        VulkanQueueFamilies candidateFamilies{};
        const int64_t score = scoreDevice(dev, candidateFamilies);
        if (score > bestScore) {
            bestScore = score;
            bestDevice = dev;
            bestFamilies = candidateFamilies;
        }
    }

    if (bestDevice == VK_NULL_HANDLE || bestScore < 0) {
        throw std::runtime_error("VulkanPhysicalDevice: failed to find a suitable GPU");
    }

    physicalDevice = bestDevice;
    families = bestFamilies;
    queryOptionalSupport(bestDevice);
}

bool VulkanPhysicalDevice::evaluatePolicyRequirement(DeviceFeaturePolicy::Requirement requirement, bool supported) const
{
    if (requirement == DeviceFeaturePolicy::Requirement::Disabled) {
        return false;
    }
    if (requirement == DeviceFeaturePolicy::Requirement::Required) {
        return supported;
    }
    return supported;
}

void VulkanPhysicalDevice::queryOptionalSupport(VkPhysicalDevice candidate) {
    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> exts(extCount);
    if (extCount) vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extCount, exts.data());

    std::unordered_set<std::string> availableExts;
    availableExts.reserve(exts.size());
    for (const auto& e : exts) {
        availableExts.insert(e.extensionName);
    }

    const auto hasExtension = [&](const char* name) {
        return name != nullptr && availableExts.contains(name);
    };

    portabilitySubsetAvailable = hasExtension(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(candidate, &props);

    VulkanDeviceCapabilities caps{};
    caps.enabledFeatures2 = VkPhysicalDeviceFeatures2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    caps.timelineFeatures = VkPhysicalDeviceTimelineSemaphoreFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES };
    caps.dynamicRenderingFeatures = VkPhysicalDeviceDynamicRenderingFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES };
    caps.synchronization2Features = VkPhysicalDeviceSynchronization2Features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES };
    caps.descriptorIndexingFeatures = VkPhysicalDeviceDescriptorIndexingFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES };
    caps.bufferDeviceAddressFeatures = VkPhysicalDeviceBufferDeviceAddressFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES };

    caps.enabledFeatures2.pNext = &caps.timelineFeatures;
    caps.timelineFeatures.pNext = &caps.dynamicRenderingFeatures;
    caps.dynamicRenderingFeatures.pNext = &caps.synchronization2Features;
    caps.synchronization2Features.pNext = &caps.descriptorIndexingFeatures;
    caps.descriptorIndexingFeatures.pNext = &caps.bufferDeviceAddressFeatures;

    vkGetPhysicalDeviceFeatures2(candidate, &caps.enabledFeatures2);

    caps.coreFeatures = caps.enabledFeatures2.features;
    caps.timelineSemaphoreSupported = (caps.timelineFeatures.timelineSemaphore == VK_TRUE);
    caps.dynamicRenderingSupported = (caps.dynamicRenderingFeatures.dynamicRendering == VK_TRUE);
    caps.synchronization2Supported = (caps.synchronization2Features.synchronization2 == VK_TRUE);
    caps.descriptorIndexingSupported =
        (caps.descriptorIndexingFeatures.runtimeDescriptorArray == VK_TRUE) &&
        (caps.descriptorIndexingFeatures.shaderSampledImageArrayNonUniformIndexing == VK_TRUE);
    caps.bufferDeviceAddressSupported = (caps.bufferDeviceAddressFeatures.bufferDeviceAddress == VK_TRUE);

    caps.timelineSemaphoreEnabled = evaluatePolicyRequirement(featurePolicy.timelineSemaphore, caps.timelineSemaphoreSupported);
    caps.dynamicRenderingEnabled = evaluatePolicyRequirement(featurePolicy.dynamicRendering, caps.dynamicRenderingSupported);
    caps.synchronization2Enabled = evaluatePolicyRequirement(featurePolicy.synchronization2, caps.synchronization2Supported);
    caps.descriptorIndexingEnabled = evaluatePolicyRequirement(featurePolicy.descriptorIndexing, caps.descriptorIndexingSupported);
    caps.bufferDeviceAddressEnabled = evaluatePolicyRequirement(featurePolicy.bufferDeviceAddress, caps.bufferDeviceAddressSupported);

    caps.timelineFeatures.timelineSemaphore = caps.timelineSemaphoreEnabled ? VK_TRUE : VK_FALSE;
    caps.dynamicRenderingFeatures.dynamicRendering = caps.dynamicRenderingEnabled ? VK_TRUE : VK_FALSE;
    caps.synchronization2Features.synchronization2 = caps.synchronization2Enabled ? VK_TRUE : VK_FALSE;
    caps.descriptorIndexingFeatures.runtimeDescriptorArray = caps.descriptorIndexingEnabled ? VK_TRUE : VK_FALSE;
    caps.descriptorIndexingFeatures.shaderSampledImageArrayNonUniformIndexing = caps.descriptorIndexingEnabled ? VK_TRUE : VK_FALSE;
    caps.bufferDeviceAddressFeatures.bufferDeviceAddress = caps.bufferDeviceAddressEnabled ? VK_TRUE : VK_FALSE;

    std::unordered_set<std::string> chosen;
    const auto pushExtensionUnique = [&](const char* extensionName, bool required) {
        if (extensionName == nullptr || chosen.contains(extensionName)) {
            return;
        }
        if (!hasExtension(extensionName)) {
            if (required) {
                throw std::runtime_error(std::string("VulkanPhysicalDevice: missing required extension from policy: ") + extensionName);
            }
            return;
        }
        chosen.insert(extensionName);
        caps.enabledExtensions.push_back(extensionName);
    };

    for (const char* extensionName : requiredExtensions) {
        pushExtensionUnique(extensionName, true);
    }
    for (const char* extensionName : featurePolicy.requiredExtensions) {
        pushExtensionUnique(extensionName, true);
    }

    for (const char* extensionName : featurePolicy.optionalExtensions) {
        pushExtensionUnique(extensionName, false);
    }
    for (const char* extensionName : featurePolicy.experimentalExtensions) {
        pushExtensionUnique(extensionName, false);
    }

    if (portabilitySubsetAvailable) {
        pushExtensionUnique(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME, false);
    }

    if (caps.dynamicRenderingEnabled && hasExtension(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME)) {
        pushExtensionUnique(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME, false);
    }
    if (caps.synchronization2Enabled && hasExtension(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME)) {
        pushExtensionUnique(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME, false);
    }
    if (caps.descriptorIndexingEnabled && hasExtension(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME)) {
        pushExtensionUnique(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME, false);
    }
    if (caps.bufferDeviceAddressEnabled && hasExtension(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME)) {
        pushExtensionUnique(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME, false);
    }

    for (const char* extensionName : featurePolicy.disabledExtensions) {
        if (extensionName == nullptr) {
            continue;
        }
        caps.enabledExtensions.erase(
            std::remove_if(caps.enabledExtensions.begin(), caps.enabledExtensions.end(),
                [&](const char* enabled) { return cstrEq(enabled, extensionName); }),
            caps.enabledExtensions.end());
    }

    const uint32_t major = VK_API_VERSION_MAJOR(props.apiVersion);
    const uint32_t minor = VK_API_VERSION_MINOR(props.apiVersion);
    const bool apiAtLeast13 = (major > 1) || (major == 1 && minor >= 3);

    const auto hasEnabledExtension = [&](const char* extensionName) {
        return std::any_of(caps.enabledExtensions.begin(), caps.enabledExtensions.end(),
            [&](const char* enabled) { return cstrEq(enabled, extensionName); });
    };

    if (!apiAtLeast13) {
        if (caps.dynamicRenderingEnabled && !hasEnabledExtension(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME)) {
            throw std::runtime_error("VulkanPhysicalDevice: dynamic rendering enabled by policy but required extension missing on Vulkan < 1.3");
        }
        if (caps.synchronization2Enabled && !hasEnabledExtension(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME)) {
            throw std::runtime_error("VulkanPhysicalDevice: synchronization2 enabled by policy but required extension missing on Vulkan < 1.3");
        }
    }

    caps.runtimeContract.loaderApiVersion = queryLoaderApiVersion();
    caps.runtimeContract.instanceApiVersion = instanceApiVersion;
    caps.runtimeContract.physicalDeviceApiVersion = props.apiVersion;
    caps.runtimeContract.negotiatedApiVersion = std::min(caps.runtimeContract.instanceApiVersion, props.apiVersion);

    caps.runtimeContract.timelineSemaphoreSource = resolveFeatureSource(
        caps.timelineSemaphoreEnabled,
        caps.runtimeContract.negotiatedApiVersion,
        VK_API_VERSION_1_2);
    caps.runtimeContract.dynamicRenderingSource = resolveFeatureSource(
        caps.dynamicRenderingEnabled,
        caps.runtimeContract.negotiatedApiVersion,
        VK_API_VERSION_1_3);
    caps.runtimeContract.synchronization2Source = resolveFeatureSource(
        caps.synchronization2Enabled,
        caps.runtimeContract.negotiatedApiVersion,
        VK_API_VERSION_1_3);
    caps.runtimeContract.descriptorIndexingSource = resolveFeatureSource(
        caps.descriptorIndexingEnabled,
        caps.runtimeContract.negotiatedApiVersion,
        VK_API_VERSION_1_2);
    caps.runtimeContract.bufferDeviceAddressSource = resolveFeatureSource(
        caps.bufferDeviceAddressEnabled,
        caps.runtimeContract.negotiatedApiVersion,
        VK_API_VERSION_1_2);

    deviceCapabilities = std::move(caps);
}

bool VulkanPhysicalDevice::isSuitable(VkPhysicalDevice candidate) const {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(candidate, &props);

    if (props.apiVersion < makeVersion(1u, 2u, 0u)) return false;

    VulkanQueueFamilies q{};
    findQueueFamilies(candidate, q);
    if (!q.complete()) return false;

    if (!checkExtensions(candidate)) return false;
    if (!hasSwapchainSupport(candidate)) return false;

    VkPhysicalDeviceTimelineSemaphoreFeatures timeline{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES };
    VkPhysicalDeviceDynamicRenderingFeatures dynamicRendering{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES };
    VkPhysicalDeviceSynchronization2Features synchronization2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES };
    VkPhysicalDeviceDescriptorIndexingFeatures descriptorIndexing{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES };
    VkPhysicalDeviceBufferDeviceAddressFeatures bda{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES };
    VkPhysicalDeviceFeatures2 f2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    f2.pNext = &timeline;
    timeline.pNext = &dynamicRendering;
    dynamicRendering.pNext = &synchronization2;
    synchronization2.pNext = &descriptorIndexing;
    descriptorIndexing.pNext = &bda;
    vkGetPhysicalDeviceFeatures2(candidate, &f2);

    if (featurePolicy.timelineSemaphore == DeviceFeaturePolicy::Requirement::Required && timeline.timelineSemaphore != VK_TRUE) return false;
    if (featurePolicy.dynamicRendering == DeviceFeaturePolicy::Requirement::Required && dynamicRendering.dynamicRendering != VK_TRUE) return false;
    if (featurePolicy.synchronization2 == DeviceFeaturePolicy::Requirement::Required && synchronization2.synchronization2 != VK_TRUE) return false;
    const bool descriptorIndexingSupported = descriptorIndexing.runtimeDescriptorArray == VK_TRUE
        && descriptorIndexing.shaderSampledImageArrayNonUniformIndexing == VK_TRUE;
    if (featurePolicy.descriptorIndexing == DeviceFeaturePolicy::Requirement::Required && !descriptorIndexingSupported) return false;
    if (featurePolicy.bufferDeviceAddress == DeviceFeaturePolicy::Requirement::Required && bda.bufferDeviceAddress != VK_TRUE) return false;

    return true;
}

bool VulkanPhysicalDevice::checkExtensions(VkPhysicalDevice candidate) const {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(candidate, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    if (count) vkEnumerateDeviceExtensionProperties(candidate, nullptr, &count, available.data());

    auto hasExt = [&](const char* req) {
        for (const auto& e : available) {
            if (cstrEq(e.extensionName, req)) {
                return true;
            }
        }
        return false;
    };

    for (const char* const req : requiredExtensions) {
        if (!hasExt(req)) return false;
    }
    for (const char* const req : featurePolicy.requiredExtensions) {
        if (!hasExt(req)) return false;
    }
    return true;
}

void VulkanPhysicalDevice::findQueueFamilies(VkPhysicalDevice candidate,
    VulkanQueueFamilies& out) const
{
    out = VulkanQueueFamilies{};

    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr);
    std::vector<VkQueueFamilyProperties> props(count);
    if (count) vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, props.data());

    uint32_t firstGraphics = UINT32_MAX;
    uint32_t firstPresent = UINT32_MAX;
    uint32_t dedicatedTransfer = UINT32_MAX;
    uint32_t fallbackTransfer = UINT32_MAX;
    uint32_t dedicatedCompute = UINT32_MAX;
    uint32_t fallbackCompute = UINT32_MAX;

    for (uint32_t i = 0; i < count; ++i) {
        const bool hasGraphics = (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
        const bool hasCompute = (props[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
        const bool hasTransfer = (props[i].queueFlags & VK_QUEUE_TRANSFER_BIT) != 0;

        if (hasGraphics && firstGraphics == UINT32_MAX) {
            firstGraphics = i;
        }

        VkBool32 presentSupport = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface, &presentSupport);
        if (presentSupport && firstPresent == UINT32_MAX) {
            firstPresent = i;
        }

        if (hasTransfer) {
            if (!hasGraphics && !hasCompute && dedicatedTransfer == UINT32_MAX) {
                dedicatedTransfer = i;
            }
            if (!hasGraphics && fallbackTransfer == UINT32_MAX) {
                fallbackTransfer = i;
            }
        }

        if (hasCompute) {
            if (!hasGraphics && dedicatedCompute == UINT32_MAX) {
                dedicatedCompute = i;
            }
            if (fallbackCompute == UINT32_MAX) {
                fallbackCompute = i;
            }
        }
    }

    out.graphicsFamily = firstGraphics;
    out.presentFamily = firstPresent;
    out.computeFamily = dedicatedCompute != UINT32_MAX ? dedicatedCompute : fallbackCompute;

    if (dedicatedTransfer != UINT32_MAX) {
        out.transferFamily = dedicatedTransfer;
    } else if (fallbackTransfer != UINT32_MAX) {
        out.transferFamily = fallbackTransfer;
    } else {
        out.transferFamily = out.graphicsFamily;
    }

    if (out.computeFamily == UINT32_MAX) {
        out.computeFamily = out.graphicsFamily;
    }
}

bool VulkanPhysicalDevice::hasSwapchainSupport(VkPhysicalDevice candidate) const {
    uint32_t fmtCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(candidate, surface, &fmtCount, nullptr);

    uint32_t pmCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(candidate, surface, &pmCount, nullptr);

    return fmtCount > 0 && pmCount > 0;
}

int64_t VulkanPhysicalDevice::scoreDevice(VkPhysicalDevice candidate, VulkanQueueFamilies& outFamilies) const
{
    if (!isSuitable(candidate)) {
        return -1;
    }

    findQueueFamilies(candidate, outFamilies);

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(candidate, &props);

    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(candidate, &memProps);

    uint64_t localHeapBytes = 0;
    for (uint32_t i = 0; i < memProps.memoryHeapCount; ++i) {
        if ((memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
            localHeapBytes += memProps.memoryHeaps[i].size;
        }
    }

    int64_t score = 0;
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
        score += 200000;
    } else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
        score += 50000;
    }

    score += static_cast<int64_t>(localHeapBytes / (1024ull * 1024ull));
    score += static_cast<int64_t>(props.limits.maxImageDimension2D) * 4;
    score += static_cast<int64_t>(props.limits.maxBoundDescriptorSets) * 16;

    if (outFamilies.transferFamily != outFamilies.graphicsFamily) {
        score += 20000;
    }
    if (outFamilies.computeFamily != outFamilies.graphicsFamily) {
        score += 12000;
    }

    return score;
}

void VulkanPhysicalDevice::getProperties(VkPhysicalDeviceProperties& out) const {
    if (physicalDevice == VK_NULL_HANDLE) {
        throw std::runtime_error("VulkanPhysicalDevice::getProperties: physical device not selected");
    }
    vkGetPhysicalDeviceProperties(physicalDevice, &out);
}

void VulkanPhysicalDevice::getFeatures(VkPhysicalDeviceFeatures& out) const {
    if (physicalDevice == VK_NULL_HANDLE) {
        throw std::runtime_error("VulkanPhysicalDevice::getFeatures: physical device not selected");
    }
    vkGetPhysicalDeviceFeatures(physicalDevice, &out);
}

// ===================== VulkanDevice =====================

VulkanDevice::VulkanDevice(VkPhysicalDevice pd,
    const VulkanQueueFamilies& queueFamilies,
    const VulkanDeviceCapabilities& capabilities,
    DeviceFeaturePolicy policy)
    : device(VK_NULL_HANDLE)
    , graphicsQueue(VK_NULL_HANDLE)
    , presentQueue(VK_NULL_HANDLE)
    , transferQueue(VK_NULL_HANDLE)
    , computeQueue(VK_NULL_HANDLE)
    , graphicsFamily(queueFamilies.graphicsFamily)
    , presentFamily(queueFamilies.presentFamily)
    , transferFamily(queueFamilies.transferFamily)
    , computeFamily(queueFamilies.computeFamily)
    , synchronization2EnabledFlag(capabilities.synchronization2Enabled)
{
    if (pd == VK_NULL_HANDLE) {
        throw std::runtime_error("VulkanDevice: physical device is null");
    }
    if (graphicsFamily == UINT32_MAX || presentFamily == UINT32_MAX || transferFamily == UINT32_MAX || computeFamily == UINT32_MAX) {
        throw std::runtime_error("VulkanDevice: invalid queue family indices");
    }
    if (policy.timelineSemaphore == DeviceFeaturePolicy::Requirement::Required && !capabilities.timelineSemaphoreEnabled) {
        throw std::runtime_error("VulkanDevice: timeline semaphores are required by policy");
    }

    float priority = 1.0f;

    std::vector<VkDeviceQueueCreateInfo> queueCIs{};
    queueCIs.reserve(4u);

    const auto addQueue = [&](uint32_t family) {
        for (const auto& existing : queueCIs) {
            if (existing.queueFamilyIndex == family) {
                return;
            }
        }
        VkDeviceQueueCreateInfo q{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        q.queueFamilyIndex = family;
        q.queueCount = 1;
        q.pQueuePriorities = &priority;
        queueCIs.push_back(q);
    };

    addQueue(graphicsFamily);
    addQueue(presentFamily);
    addQueue(transferFamily);
    addQueue(computeFamily);

    VulkanDeviceCapabilities enabledCaps = capabilities;
    enabledCaps.enabledFeatures2.pNext = &enabledCaps.timelineFeatures;
    enabledCaps.timelineFeatures.pNext = &enabledCaps.dynamicRenderingFeatures;
    enabledCaps.dynamicRenderingFeatures.pNext = &enabledCaps.synchronization2Features;
    enabledCaps.synchronization2Features.pNext = &enabledCaps.descriptorIndexingFeatures;
    enabledCaps.descriptorIndexingFeatures.pNext = &enabledCaps.bufferDeviceAddressFeatures;
    enabledCaps.bufferDeviceAddressFeatures.pNext = nullptr;

    VkDeviceCreateInfo ci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    ci.queueCreateInfoCount = static_cast<uint32_t>(queueCIs.size());
    ci.pQueueCreateInfos = queueCIs.data();
    ci.enabledExtensionCount = static_cast<uint32_t>(enabledCaps.enabledExtensions.size());
    ci.ppEnabledExtensionNames = enabledCaps.enabledExtensions.empty() ? nullptr : enabledCaps.enabledExtensions.data();
    ci.pEnabledFeatures = nullptr;
    ci.pNext = &enabledCaps.enabledFeatures2;

    const VkResult res = vkCreateDevice(pd, &ci, nullptr, &device);
    if (res != VK_SUCCESS) {
        vkutil::throwVkError("vkCreateDevice", res);
    }

    vkGetDeviceQueue(device, graphicsFamily, 0, &graphicsQueue);
    vkGetDeviceQueue(device, presentFamily, 0, &presentQueue);
    vkGetDeviceQueue(device, transferFamily, 0, &transferQueue);
    vkGetDeviceQueue(device, computeFamily, 0, &computeQueue);

    if (graphicsQueue == VK_NULL_HANDLE || presentQueue == VK_NULL_HANDLE || transferQueue == VK_NULL_HANDLE || computeQueue == VK_NULL_HANDLE) {
        reset();
        throw std::runtime_error("VulkanDevice: vkGetDeviceQueue returned null queue(s)");
    }

    dispatchTable.queueSubmit2 = reinterpret_cast<PFN_vkQueueSubmit2>(vkGetDeviceProcAddr(device, "vkQueueSubmit2"));
    dispatchTable.cmdPipelineBarrier2 = reinterpret_cast<PFN_vkCmdPipelineBarrier2>(vkGetDeviceProcAddr(device, "vkCmdPipelineBarrier2"));
    dispatchTable.cmdWaitEvents2 = reinterpret_cast<PFN_vkCmdWaitEvents2>(vkGetDeviceProcAddr(device, "vkCmdWaitEvents2"));
    dispatchTable.cmdWriteTimestamp2 = reinterpret_cast<PFN_vkCmdWriteTimestamp2>(vkGetDeviceProcAddr(device, "vkCmdWriteTimestamp2"));

    if (synchronization2EnabledFlag && !dispatchTable.hasSynchronization2()) {
        reset();
        throw std::runtime_error("VulkanDevice: synchronization2 enabled but vkQueueSubmit2 dispatch is unavailable");
    }
}

VulkanDevice::VulkanDevice(VulkanDevice&& other) noexcept
    : device(std::exchange(other.device, VK_NULL_HANDLE))
    , graphicsQueue(std::exchange(other.graphicsQueue, VK_NULL_HANDLE))
    , presentQueue(std::exchange(other.presentQueue, VK_NULL_HANDLE))
    , transferQueue(std::exchange(other.transferQueue, VK_NULL_HANDLE))
    , computeQueue(std::exchange(other.computeQueue, VK_NULL_HANDLE))
    , graphicsFamily(std::exchange(other.graphicsFamily, UINT32_MAX))
    , presentFamily(std::exchange(other.presentFamily, UINT32_MAX))
    , transferFamily(std::exchange(other.transferFamily, UINT32_MAX))
    , computeFamily(std::exchange(other.computeFamily, UINT32_MAX))
    , synchronization2EnabledFlag(std::exchange(other.synchronization2EnabledFlag, false))
    , dispatchTable(std::exchange(other.dispatchTable, VulkanDeviceDispatch{}))
{
}

VulkanDevice& VulkanDevice::operator=(VulkanDevice&& other) noexcept {
    if (this != &other) {
        reset();
        device = std::exchange(other.device, VK_NULL_HANDLE);
        graphicsQueue = std::exchange(other.graphicsQueue, VK_NULL_HANDLE);
        presentQueue = std::exchange(other.presentQueue, VK_NULL_HANDLE);
        transferQueue = std::exchange(other.transferQueue, VK_NULL_HANDLE);
        computeQueue = std::exchange(other.computeQueue, VK_NULL_HANDLE);
        graphicsFamily = std::exchange(other.graphicsFamily, UINT32_MAX);
        presentFamily = std::exchange(other.presentFamily, UINT32_MAX);
        transferFamily = std::exchange(other.transferFamily, UINT32_MAX);
        computeFamily = std::exchange(other.computeFamily, UINT32_MAX);
        synchronization2EnabledFlag = std::exchange(other.synchronization2EnabledFlag, false);
        dispatchTable = std::exchange(other.dispatchTable, VulkanDeviceDispatch{});
    }
    return *this;
}

VulkanDevice::~VulkanDevice() noexcept {
    reset();
}

void VulkanDevice::reset() noexcept {
    if (device != VK_NULL_HANDLE) {
        vkDestroyDevice(device, nullptr);
        device = VK_NULL_HANDLE;
    }
    graphicsQueue = VK_NULL_HANDLE;
    presentQueue = VK_NULL_HANDLE;
    transferQueue = VK_NULL_HANDLE;
    computeQueue = VK_NULL_HANDLE;
    graphicsFamily = UINT32_MAX;
    presentFamily = UINT32_MAX;
    transferFamily = UINT32_MAX;
    computeFamily = UINT32_MAX;
    synchronization2EnabledFlag = false;
    dispatchTable = VulkanDeviceDispatch{};
}

// ===================== VulkanQueue =====================

VulkanQueue::VulkanQueue(VkDevice dev,
    uint32_t queueFamilyIndex,
    uint32_t queueIndex,
    bool synchronization2EnabledIn,
    PFN_vkQueueSubmit2 queueSubmit2Fn)
    : device(dev)
    , queue(VK_NULL_HANDLE)
    , queueFamilyIndex(queueFamilyIndex)
    , synchronization2Enabled(synchronization2EnabledIn)
    , pfnQueueSubmit2(queueSubmit2Fn)
{
    if (device == VK_NULL_HANDLE) {
        throw std::runtime_error("VulkanQueue: device is null");
    }

    vkGetDeviceQueue(device, queueFamilyIndex, queueIndex, &queue);
    if (queue == VK_NULL_HANDLE) {
        throw std::runtime_error("VulkanQueue: vkGetDeviceQueue returned null");
    }

    queueMutex = getSharedQueueMutex(device, queue);
}

vkutil::VkExpected<void> VulkanQueue::submit(const std::vector<VkSubmitInfo>& submitInfos, VkFence fence, const char* subsystem) const {
    if (queue == VK_NULL_HANDLE) {
        return vkutil::makeError("VulkanQueue::submit", VK_ERROR_DEVICE_LOST, subsystem);
    }
    if (!queueMutex) {
        return vkutil::makeError("VulkanQueue::submit", VK_ERROR_DEVICE_LOST, subsystem);
    }
    for (const auto& submitInfo : submitInfos) {
        if (submitInfo.waitSemaphoreCount > 0 && submitInfo.pWaitSemaphores == nullptr) {
            return vkutil::makeError("VulkanQueue::submit", VK_ERROR_INITIALIZATION_FAILED, subsystem);
        }
        if (submitInfo.waitSemaphoreCount > 0 && submitInfo.pWaitDstStageMask == nullptr) {
            return vkutil::makeError("VulkanQueue::submit", VK_ERROR_INITIALIZATION_FAILED, subsystem);
        }
        if (submitInfo.commandBufferCount > 0 && submitInfo.pCommandBuffers == nullptr) {
            return vkutil::makeError("VulkanQueue::submit", VK_ERROR_INITIALIZATION_FAILED, subsystem);
        }
        if (submitInfo.signalSemaphoreCount > 0 && submitInfo.pSignalSemaphores == nullptr) {
            return vkutil::makeError("VulkanQueue::submit", VK_ERROR_INITIALIZATION_FAILED, subsystem);
        }
    }
    const std::lock_guard<std::mutex> lock(*queueMutex);
    VKUTIL_RETURN_IF_FAILED(vkQueueSubmit(queue,
        static_cast<uint32_t>(submitInfos.size()),
        submitInfos.data(),
        fence), "vkQueueSubmit", subsystem);
    return {};
}

vkutil::VkExpected<void> VulkanQueue::submit2(const std::vector<VkSubmitInfo2>& submitInfos, VkFence fence, const char* subsystem) const {
    if (queue == VK_NULL_HANDLE) {
        return vkutil::makeError("VulkanQueue::submit2", VK_ERROR_DEVICE_LOST, subsystem);
    }

    if (!synchronization2Enabled || pfnQueueSubmit2 == nullptr) {
        return vkutil::makeError("VulkanQueue::submit2", VK_ERROR_FEATURE_NOT_PRESENT, subsystem, "synchronization2_unavailable");
    }
    if (!queueMutex) {
        return vkutil::makeError("VulkanQueue::submit2", VK_ERROR_DEVICE_LOST, subsystem);
    }
    for (const auto& submitInfo : submitInfos) {
        if (submitInfo.waitSemaphoreInfoCount > 0 && submitInfo.pWaitSemaphoreInfos == nullptr) {
            return vkutil::makeError("VulkanQueue::submit2", VK_ERROR_INITIALIZATION_FAILED, subsystem);
        }
        if (submitInfo.commandBufferInfoCount > 0 && submitInfo.pCommandBufferInfos == nullptr) {
            return vkutil::makeError("VulkanQueue::submit2", VK_ERROR_INITIALIZATION_FAILED, subsystem);
        }
        if (submitInfo.signalSemaphoreInfoCount > 0 && submitInfo.pSignalSemaphoreInfos == nullptr) {
            return vkutil::makeError("VulkanQueue::submit2", VK_ERROR_INITIALIZATION_FAILED, subsystem);
        }
    }

    const std::lock_guard<std::mutex> lock(*queueMutex);
    VKUTIL_RETURN_IF_FAILED(pfnQueueSubmit2(queue,
        static_cast<uint32_t>(submitInfos.size()),
        submitInfos.data(),
        fence), "vkQueueSubmit2", subsystem);
    return {};
}

VkResult VulkanQueue::present(VkSwapchainKHR swapchain, uint32_t imageIndex, VkSemaphore waitSemaphore) const {
    if (!queueMutex) {
        return VK_ERROR_DEVICE_LOST;
    }
    if (waitSemaphore == VK_NULL_HANDLE) {
        const std::vector<VkSemaphore> none{};
        return present(swapchain, imageIndex, none);
    }
    const std::vector<VkSemaphore> one{ waitSemaphore };
    return present(swapchain, imageIndex, one);
}

VkResult VulkanQueue::present(VkSwapchainKHR swapchain,
    uint32_t imageIndex,
    const std::vector<VkSemaphore>& waitSemaphores) const
{
    if (queue == VK_NULL_HANDLE) {
        return VK_ERROR_DEVICE_LOST; // best-effort: queue is dead
    }
    if (!queueMutex) {
        return VK_ERROR_DEVICE_LOST;
    }

    VkPresentInfoKHR pi{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.size());
    pi.pWaitSemaphores = waitSemaphores.empty() ? nullptr : waitSemaphores.data();
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain;
    pi.pImageIndices = &imageIndex;

    const std::lock_guard<std::mutex> lock(*queueMutex);
    return vkQueuePresentKHR(queue, &pi);
}

vkutil::VkExpected<void> VulkanQueue::waitIdle() const {
    if (queue == VK_NULL_HANDLE) {
        return vkutil::makeError("VulkanQueue::waitIdle", VK_ERROR_DEVICE_LOST, "queue");
    }
    if (!queueMutex) {
        return vkutil::makeError("VulkanQueue::waitIdle", VK_ERROR_DEVICE_LOST, "queue");
    }
    const std::lock_guard<std::mutex> lock(*queueMutex);
    VKUTIL_RETURN_IF_FAILED(vkQueueWaitIdle(queue), "vkQueueWaitIdle", "queue");
    return {};
}
