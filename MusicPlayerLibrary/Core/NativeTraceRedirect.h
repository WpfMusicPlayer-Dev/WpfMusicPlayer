// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <cstdarg>
#include <mutex>
#include <string>
#include "Core/NativeLogWriter.h"

class NativeTraceRedirect
{
public:
    explicit NativeTraceRedirect(std::unique_ptr<MusicPlayerLibrary::INativeLogWriter> log_writer);

    ~NativeTraceRedirect();

    NativeTraceRedirect(const NativeTraceRedirect&) = delete;
    NativeTraceRedirect& operator=(const NativeTraceRedirect&) = delete;

    void Enable();
    void Disable();
    [[nodiscard]] bool IsEnabled() const { return enable_redirect; }

    void SetIncludeTimestamp(bool include) { timestamp_enable = include; }
    void SetIncludeFileInfo(bool include) { info_enable = include; }

    void TraceEx(const char* file_name, int line_num, const wchar_t* format, ...) noexcept;
    void TraceEx(const char* file_name, int line_num, const char* format, ...) noexcept;

    static NativeTraceRedirect* GetTraceRedirector();
    static void SetTraceRedirector(NativeTraceRedirect*);
    static void InitNativeTraceRedirect();
    static void ShutdownNativeTraceRedirect() noexcept;

private:
    [[nodiscard]] std::string query_time_stamp() const;
    void write_log(const char* file_name_full, int line_num, const char* message);

    std::string format_message_va(const wchar_t* format, va_list args);
    std::string format_message_va(const char* format, va_list args);

    template <typename Character>
    void trace_va(
        const char* file_name,
        int line_num,
        const Character* format,
        va_list args) noexcept
    {
        if (!enable_redirect || format == nullptr)
            return;

        try
        {
            const std::string message = format_message_va(format, args);
            write_log(file_name, line_num, message.c_str());
        }
        catch (...)
        {
        }
    }

    bool enable_redirect;
    bool timestamp_enable;
    bool info_enable;
    std::mutex file_mut;

    std::unique_ptr<MusicPlayerLibrary::INativeLogWriter> native_log_writer_;
    static std::unique_ptr<NativeTraceRedirect> global_trace_redirector;
};

#define NATIVE_TRACE_REDIRECT_EX(redirector, fmt, ...) \
    do { \
        if ((redirector) != nullptr) { \
            (redirector)->TraceEx(__FILE__, __LINE__, fmt, __VA_ARGS__); \
        } \
    } while(0)

#if defined(NATIVE_TRACE_REDIRECT_ENABLED)
#if defined(NATIVE_TRACE)
#undef NATIVE_TRACE
#endif
#define NATIVE_TRACE(fmt, ...) NATIVE_TRACE_REDIRECT_EX(NativeTraceRedirect::GetTraceRedirector(), fmt, __VA_ARGS__)
#endif
