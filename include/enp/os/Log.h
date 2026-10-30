// Logging routed to the platform's native sink (stderr, OutputDebugString, logcat, console).
#pragma once

namespace enp::os::log {

enum class Level { Debug, Info, Warning, Error };

void write(Level level, const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

} // namespace enp::os::log

#define ENP_LOG_DEBUG(...) ::enp::os::log::write(::enp::os::log::Level::Debug, __VA_ARGS__)
#define ENP_LOG_INFO(...)  ::enp::os::log::write(::enp::os::log::Level::Info, __VA_ARGS__)
#define ENP_LOG_WARN(...)  ::enp::os::log::write(::enp::os::log::Level::Warning, __VA_ARGS__)
#define ENP_LOG_ERROR(...) ::enp::os::log::write(::enp::os::log::Level::Error, __VA_ARGS__)
