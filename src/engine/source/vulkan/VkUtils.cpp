#include <fstream>
#include <string>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <iostream>
#include <exception>
#include <stdexcept>

#include "VkUtils.h"
namespace
{
    struct DebugUtilsState
    {
        PFN_vkSetDebugUtilsObjectNameEXT setName{ nullptr };
        PFN_vkCmdBeginDebugUtilsLabelEXT beginLabel{ nullptr };
        PFN_vkCmdEndDebugUtilsLabelEXT endLabel{ nullptr };
    };

    std::mutex& debugUtilsMutex()
    {
        static std::mutex m;
        return m;
    }

    std::unordered_map<VkDevice, DebugUtilsState>& debugUtilsRegistry()
    {
        static std::unordered_map<VkDevice, DebugUtilsState> registry;
        return registry;
    }

    std::mutex& diagnosticsMutex()
    {
        static std::mutex m;
        return m;
    }

    vkutil::VkDiagnosticSink& diagnosticSink()
    {
        static vkutil::VkDiagnosticSink sink;
        return sink;
    }
}

namespace vkutil {

    void setDiagnosticSink(VkDiagnosticSink sink)
    {
        const std::lock_guard<std::mutex> lock(diagnosticsMutex());
        diagnosticSink() = std::move(sink);
    }

    void clearDiagnosticSink() noexcept
    {
        const std::lock_guard<std::mutex> lock(diagnosticsMutex());
        diagnosticSink() = nullptr;
    }

    void reportDiagnostic(VkDiagnosticMessage message)
    {
        VkDiagnosticSink sink;
        {
            const std::lock_guard<std::mutex> lock(diagnosticsMutex());
            sink = diagnosticSink();
        }

        if (sink) {
            sink(message);
            return;
        }

        std::cerr << "[" << (message.subsystem ? message.subsystem : "vk") << "] "
                  << (message.operation ? message.operation : "operation")
                  << " failed: " << message.text << " ("
                  << vkResultToString(message.result)
                  << ", frame=" << message.frameIndex
                  << ", file=" << (message.callsiteFile ? message.callsiteFile : "<unknown>")
                  << ":" << message.callsiteLine << ")\n";
    }

    VkExpected<void> makeError(
        const char* functionName,
        VkResult result,
        const char* subsystem,
        const char* objectName,
        uint64_t objectHandle,
        bool retryable,
        uint64_t frameIndex,
        const std::source_location& location)
    {
        VkDiagnosticMessage msg{};
        msg.subsystem = subsystem;
        msg.objectName = objectName;
        msg.operation = functionName;
        msg.callsiteFile = location.file_name();
        msg.callsiteLine = location.line();
        msg.frameIndex = frameIndex;
        msg.result = result;
        msg.text = vkErrorMessage(functionName, result);
        reportDiagnostic(std::move(msg));

        return VkExpected<void>(VkErrorContext{
            .result = result,
            .operation = functionName,
            .subsystem = subsystem,
            .objectName = objectName,
            .objectHandle = objectHandle,
            .frameIndex = frameIndex,
            .retryable = retryable || isRetryable(result),
            .callsiteFile = location.file_name(),
            .callsiteLine = location.line()
        });
    }

    void initDebugUtils(VkInstance instance, VkDevice device)
    {
        static_cast<void>(instance);

        const std::lock_guard<std::mutex> lock(debugUtilsMutex());

        if (device == VK_NULL_HANDLE) {
            return;
        }

        DebugUtilsState state{};
        state.setName = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
            vkGetDeviceProcAddr(device, "vkSetDebugUtilsObjectNameEXT"));
        state.beginLabel = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
            vkGetDeviceProcAddr(device, "vkCmdBeginDebugUtilsLabelEXT"));
        state.endLabel = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
            vkGetDeviceProcAddr(device, "vkCmdEndDebugUtilsLabelEXT"));

        debugUtilsRegistry()[device] = state;
    }

    void shutdownDebugUtils() noexcept
    {
        const std::lock_guard<std::mutex> lock(debugUtilsMutex());
        debugUtilsRegistry().clear();
    }

    void shutdownDebugUtils(VkDevice device) noexcept
    {
        if (device == VK_NULL_HANDLE) return;
        const std::lock_guard<std::mutex> lock(debugUtilsMutex());
        debugUtilsRegistry().erase(device);
    }

    void setObjectName(VkDevice device, VkObjectType type, uint64_t handle, const char* name)
    {
        if (device == VK_NULL_HANDLE || handle == 0 || name == nullptr || name[0] == '\0') {
            return;
        }

        PFN_vkSetDebugUtilsObjectNameEXT setName = nullptr;
        {
            const std::lock_guard<std::mutex> lock(debugUtilsMutex());
            const auto it = debugUtilsRegistry().find(device);
            if (it == debugUtilsRegistry().end()) return;
            setName = it->second.setName;
        }

        if (!setName) return;

        VkDebugUtilsObjectNameInfoEXT info{ VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT };
        info.objectType = type;
        info.objectHandle = handle;
        info.pObjectName = name;
        static_cast<void>(setName(device, &info));
    }

    void cmdBeginLabel(VkDevice device, VkCommandBuffer cb, const char* name)
    {
        if (device == VK_NULL_HANDLE || cb == VK_NULL_HANDLE || name == nullptr || name[0] == '\0') {
            return;
        }

        PFN_vkCmdBeginDebugUtilsLabelEXT beginLabel = nullptr;
        {
            const std::lock_guard<std::mutex> lock(debugUtilsMutex());
            const auto it = debugUtilsRegistry().find(device);
            if (it == debugUtilsRegistry().end()) return;
            beginLabel = it->second.beginLabel;
        }

        if (!beginLabel) return;

        VkDebugUtilsLabelEXT label{ VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT };
        label.pLabelName = name;
        label.color[0] = 0.2f;
        label.color[1] = 0.6f;
        label.color[2] = 1.0f;
        label.color[3] = 1.0f;
        beginLabel(cb, &label);
    }

    void cmdEndLabel(VkDevice device, VkCommandBuffer cb)
    {
        if (device == VK_NULL_HANDLE || cb == VK_NULL_HANDLE) return;

        PFN_vkCmdEndDebugUtilsLabelEXT endLabel = nullptr;
        {
            const std::lock_guard<std::mutex> lock(debugUtilsMutex());
            const auto it = debugUtilsRegistry().find(device);
            if (it == debugUtilsRegistry().end()) return;
            endLabel = it->second.endLabel;
        }

        if (endLabel) {
            endLabel(cb);
        }
    }

    void cmdBeginLabel(VkCommandBuffer cb, const char* name)
    {
        if (cb == VK_NULL_HANDLE) return;

        VkDevice knownDevice = VK_NULL_HANDLE;
        {
            const std::lock_guard<std::mutex> lock(debugUtilsMutex());
            if (debugUtilsRegistry().size() == 1u) {
                knownDevice = debugUtilsRegistry().begin()->first;
            }
        }

        if (knownDevice != VK_NULL_HANDLE) {
            cmdBeginLabel(knownDevice, cb, name);
        }
    }

    void cmdEndLabel(VkCommandBuffer cb)
    {
        if (cb == VK_NULL_HANDLE) return;

        VkDevice knownDevice = VK_NULL_HANDLE;
        {
            const std::lock_guard<std::mutex> lock(debugUtilsMutex());
            if (debugUtilsRegistry().size() == 1u) {
                knownDevice = debugUtilsRegistry().begin()->first;
            }
        }

        if (knownDevice != VK_NULL_HANDLE) {
            cmdEndLabel(knownDevice, cb);
        }
    }

    void readFile(const std::string& path, std::vector<char>& out)
    {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) {
            static_cast<void>(makeError("vkutil::readFile", VK_ERROR_INITIALIZATION_FAILED, "io", path.c_str()));
            throw std::runtime_error("vkutil::readFile: failed to open file: " + path);
        }

        const std::streampos endPos = f.tellg();
        if (endPos < 0) {
            static_cast<void>(makeError("vkutil::readFile::tellg", VK_ERROR_INITIALIZATION_FAILED, "io", path.c_str()));
            throw std::runtime_error("vkutil::readFile: tellg failed for file: " + path);
        }

        const auto size = static_cast<std::size_t>(endPos);
        out.assign(size, '\0');

        f.seekg(0, std::ios::beg);
        if (size > 0) {
            f.read(out.data(), static_cast<std::streamsize>(size));
            if (!f) {
                static_cast<void>(makeError("vkutil::readFile::read", VK_ERROR_INITIALIZATION_FAILED, "io", path.c_str()));
                throw std::runtime_error("vkutil::readFile: failed reading file: " + path);
            }
        }
    }


    const char* vkResultToString(VkResult result) noexcept
    {
        switch (result) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_EVENT_SET: return "VK_EVENT_SET";
        case VK_EVENT_RESET: return "VK_EVENT_RESET";
        case VK_INCOMPLETE: return "VK_INCOMPLETE";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
        case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
        case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
        case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_ERROR_INCOMPATIBLE_DISPLAY_KHR: return "VK_ERROR_INCOMPATIBLE_DISPLAY_KHR";
        case VK_ERROR_VALIDATION_FAILED_EXT: return "VK_ERROR_VALIDATION_FAILED_EXT";
        case VK_ERROR_INVALID_SHADER_NV: return "VK_ERROR_INVALID_SHADER_NV";
        case VK_ERROR_FRAGMENTATION_EXT: return "VK_ERROR_FRAGMENTATION_EXT";
        case VK_ERROR_NOT_PERMITTED_KHR: return "VK_ERROR_NOT_PERMITTED_KHR";
        default: return "UNKNOWN";
        }
    }

    [[noreturn]] void throwVkError(const char* functionName, VkResult result)
    {
        throw VkException(result, vkErrorMessage(functionName, result));
    }

    VkResult exceptionToVkResult() noexcept
    {
        try {
            throw;
        } catch (const VkException& ex) {
            return ex.result();
        } catch (const std::bad_alloc&) {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        } catch (...) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }

    std::string vkErrorMessage(const char* functionName, VkResult result)
    {
        std::string msg = "Vulkan call failed: ";
        msg += (functionName != nullptr ? functionName : "<unknown>");
        msg += " (";
        msg += vkResultToString(result);
        msg += ", code=";
        msg += std::to_string(static_cast<int>(result));
        msg += ")";
        return msg;
    }

    const char* presentModeToString(VkPresentModeKHR m) noexcept
    {
        switch (m) {
        case VK_PRESENT_MODE_IMMEDIATE_KHR:     return "IMMEDIATE";
        case VK_PRESENT_MODE_MAILBOX_KHR:       return "MAILBOX";
        case VK_PRESENT_MODE_FIFO_KHR:          return "FIFO";
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR:  return "FIFO_RELAXED";
#ifdef VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR
        case VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR:  return "SHARED_DEMAND_REFRESH";
#endif
#ifdef VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR
        case VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR: return "SHARED_CONTINUOUS_REFRESH";
#endif
        default:                                return "UNKNOWN";
        }
    }

} // namespace vkutil
