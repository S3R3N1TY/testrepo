#pragma once

#include <string>
#include <vector>
#include <stdexcept>
#include <cstdint>
#include <source_location>
#include <utility>
#include <functional>

// parasoft-begin-suppress ALL "suppress all violations"
#include <vulkan/vulkan.h>
// parasoft-end-suppress ALL "suppress all violations"

namespace vkutil {

    class VkException : public std::runtime_error {
    public:
        VkException(VkResult result, const std::string& message)
            : std::runtime_error(message)
            , result_(result) {
        }

        [[nodiscard]] VkResult result() const noexcept { return result_; }

    private:
        VkResult result_{ VK_ERROR_UNKNOWN };
    };

    struct VkErrorContext {
        VkResult result{ VK_SUCCESS };
        const char* operation{ nullptr };
        const char* subsystem{ "vk" };
        const char* objectName{ nullptr };
        uint64_t objectHandle{ 0 };
        uint64_t frameIndex{ 0 };
        bool retryable{ false };
        const char* callsiteFile{ nullptr };
        uint32_t callsiteLine{ 0 };
    };

    [[nodiscard]] inline bool isRetryable(VkResult result) noexcept
    {
        return result == VK_NOT_READY || result == VK_TIMEOUT || result == VK_ERROR_OUT_OF_DATE_KHR;
    }

    template<typename T>
    class EngineResult {
    public:
        EngineResult(const T& value)
            : hasValue_(true), value_(value), error_() {
        }

        EngineResult(T&& value)
            : hasValue_(true), value_(std::move(value)), error_() {
        }

        EngineResult(const VkErrorContext& error)
            : hasValue_(false), value_(), error_(error) {
        }

        [[nodiscard]] bool hasValue() const noexcept { return hasValue_; }
        [[nodiscard]] explicit operator bool() const noexcept { return hasValue_; }
        [[nodiscard]] const T& value() const { return value_; }
        [[nodiscard]] T& value() { return value_; }
        [[nodiscard]] VkResult error() const noexcept { return error_.result; }
        [[nodiscard]] const VkErrorContext& context() const noexcept { return error_; }

    private:
        bool hasValue_ = false;
        T value_{};
        VkErrorContext error_{};
    };

    template<>
    class EngineResult<void> {
    public:
        EngineResult() = default;

        EngineResult(const VkErrorContext& error)
            : error_(error) {
        }

        [[nodiscard]] bool hasValue() const noexcept { return error_.result == VK_SUCCESS; }
        [[nodiscard]] explicit operator bool() const noexcept { return hasValue(); }
        [[nodiscard]] VkResult error() const noexcept { return error_.result; }
        [[nodiscard]] const VkErrorContext& context() const noexcept { return error_; }
    private:
        VkErrorContext error_{};
    };

    template<typename T>
    class VkExpected {
    public:
        VkExpected(const T& value)
            : impl_(value) {
        }

        VkExpected(T&& value)
            : impl_(std::move(value)) {
        }

        VkExpected(VkResult error)
            : impl_(VkErrorContext{ .result = error, .retryable = isRetryable(error) }) {
        }

        VkExpected(const VkErrorContext& context)
            : impl_(context) {
        }

        [[nodiscard]] bool hasValue() const noexcept { return impl_.hasValue(); }
        [[nodiscard]] explicit operator bool() const noexcept { return hasValue(); }
        [[nodiscard]] const T& value() const { return impl_.value(); }
        [[nodiscard]] T& value() { return impl_.value(); }
        [[nodiscard]] VkResult error() const noexcept { return impl_.error(); }
        [[nodiscard]] const VkErrorContext& context() const noexcept { return impl_.context(); }
    private:
        EngineResult<T> impl_;
    };

    template<>
    class VkExpected<void> {
    public:
        VkExpected()
            : impl_() {
        }

        VkExpected(VkResult error)
            : impl_(VkErrorContext{ .result = error, .retryable = isRetryable(error) }) {
        }

        VkExpected(const VkErrorContext& context)
            : impl_(context) {
        }

        [[nodiscard]] bool hasValue() const noexcept { return impl_.hasValue(); }
        [[nodiscard]] explicit operator bool() const noexcept { return hasValue(); }
        [[nodiscard]] VkResult error() const noexcept { return impl_.error(); }
        [[nodiscard]] const VkErrorContext& context() const noexcept { return impl_.context(); }

    private:
        EngineResult<void> impl_;
    };

    struct VkDiagnosticMessage {
        const char* subsystem = "vk";
        const char* objectName = nullptr;
        const char* operation = nullptr;
        const char* callsiteFile = nullptr;
        uint32_t callsiteLine = 0;
        uint64_t frameIndex = 0;
        VkResult result = VK_SUCCESS;
        std::string text;
    };

    using VkDiagnosticSink = std::function<void(const VkDiagnosticMessage&)>;

    void setDiagnosticSink(VkDiagnosticSink sink);
    void clearDiagnosticSink() noexcept;
    void reportDiagnostic(VkDiagnosticMessage message);

    [[nodiscard]] VkExpected<void> makeError(
        const char* functionName,
        VkResult result,
        const char* subsystem = "vk",
        const char* objectName = nullptr,
        uint64_t objectHandle = 0,
        bool retryable = false,
        uint64_t frameIndex = 0,
        const std::source_location& location = std::source_location::current());

    [[nodiscard]] inline VkExpected<void> makeError(
        const char* functionName,
        VkResult result,
        const char* subsystem,
        const char* objectName,
        uint64_t frameIndex,
        const std::source_location& location)
    {
        return makeError(functionName, result, subsystem, objectName, 0, false, frameIndex, location);
    }


    [[nodiscard]] inline VkExpected<void> checkResult(
        VkResult result,
        const char* functionName,
        const char* subsystem = "vk",
        const char* objectName = nullptr,
        uint64_t frameIndex = 0,
        const std::source_location& location = std::source_location::current())
    {
        if (result == VK_SUCCESS) {
            return VkExpected<void>{};
        }
        return makeError(functionName, result, subsystem, objectName, frameIndex, location);
    }

#define VKUTIL_RETURN_IF_FAILED(vk_expr, function_name, subsystem_name) \
    do { \
        const VkResult _vkutil_res = (vk_expr); \
        if (_vkutil_res != VK_SUCCESS) { \
            return vkutil::checkResult(_vkutil_res, (function_name), (subsystem_name)); \
        } \
    } while (false)

    // Initialise debug utils function pointers (safe to call even if extension missing).
    void initDebugUtils(VkInstance instance, VkDevice device);
    void shutdownDebugUtils() noexcept;
    void shutdownDebugUtils(VkDevice device) noexcept;

    void setObjectName(VkDevice device,
        VkObjectType type,
        uint64_t handle,
        const char* name);

    template<typename HandleT>
    inline void setObjectName(VkDevice device,
        VkObjectType type,
        HandleT handle,
        const char* name)
    {
        setObjectName(device, type,
            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle)),
            name);
    }

    void cmdBeginLabel(VkDevice device, VkCommandBuffer cb, const char* name);
    void cmdEndLabel(VkDevice device, VkCommandBuffer cb);

    void cmdBeginLabel(VkCommandBuffer cb, const char* name);
    void cmdEndLabel(VkCommandBuffer cb);

    void readFile(const std::string& path, std::vector<char>& out);

    [[nodiscard]] const char* presentModeToString(VkPresentModeKHR mode) noexcept;

    [[nodiscard]] const char* vkResultToString(VkResult result) noexcept;

    [[nodiscard]] std::string vkErrorMessage(const char* functionName, VkResult result);

    [[noreturn]] void throwVkError(const char* functionName, VkResult result);

    [[nodiscard]] VkResult exceptionToVkResult() noexcept;

    inline void check(VkResult res,
        const char* expr,
        const std::source_location& location = std::source_location::current())
    {
        if (res != VK_SUCCESS) {
            static_cast<void>(makeError(expr, res, "vk", nullptr, 0, location));
        }
    }

} // namespace vkutil
