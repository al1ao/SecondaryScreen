#pragma once
#include <windows.h>
#include <atomic>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <utility>

namespace pds {
enum class LogLevel { Trace, Debug, Info, Warn, Error };
inline std::atomic<LogLevel> g_log_level{LogLevel::Info};
inline std::mutex g_log_mutex;
template <typename... Args>
void Log(LogLevel level, Args&&... args) {
    if (level < g_log_level.load()) return;
    constexpr const char* names[]{"TRACE", "DEBUG", "INFO", "WARN", "ERROR"};
    std::ostringstream line;
    (line << ... << std::forward<Args>(args));
    std::lock_guard lock(g_log_mutex);
    std::cerr << '[' << names[static_cast<int>(level)] << "] " << line.str() << '\n';
}
inline bool CheckHr(HRESULT hr, const char* context) {
    if (SUCCEEDED(hr)) return true;
    Log(LogLevel::Error, context, ": HRESULT=0x", std::hex, static_cast<unsigned long>(hr));
    return false;
}
inline HRESULT LastErrorHr() {
    const auto error = GetLastError();
    return error == ERROR_SUCCESS ? E_FAIL : HRESULT_FROM_WIN32(error);
}
}
#define LOG_TRACE(...) ::pds::Log(::pds::LogLevel::Trace, __VA_ARGS__)
#define LOG_DEBUG(...) ::pds::Log(::pds::LogLevel::Debug, __VA_ARGS__)
#define LOG_INFO(...) ::pds::Log(::pds::LogLevel::Info, __VA_ARGS__)
#define LOG_WARN(...) ::pds::Log(::pds::LogLevel::Warn, __VA_ARGS__)
#define LOG_ERROR(...) ::pds::Log(::pds::LogLevel::Error, __VA_ARGS__)
#define LOG_HR(hr, context) ::pds::CheckHr((hr), (context))
