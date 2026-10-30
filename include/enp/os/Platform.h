// Compile-time platform / architecture detection and runtime queries.
#pragma once

// ---- Operating system --------------------------------------------------------
#if defined(__EMSCRIPTEN__)
#   define ENP_OS_WEB 1
#elif defined(__ANDROID__)
#   define ENP_OS_ANDROID 1
#elif defined(_WIN32)
#   define ENP_OS_WINDOWS 1
#elif defined(__APPLE__)
#   define ENP_OS_MACOS 1
#elif defined(__linux__)
#   define ENP_OS_LINUX 1
#else
#   error "EasyNotepad: unsupported operating system"
#endif

#if defined(ENP_OS_LINUX) || defined(ENP_OS_MACOS) || defined(ENP_OS_ANDROID) || defined(ENP_OS_WEB)
#   define ENP_OS_POSIX 1
#endif

#if defined(ENP_OS_WINDOWS) || defined(ENP_OS_LINUX) || defined(ENP_OS_MACOS)
#   define ENP_OS_DESKTOP 1
#endif

// ---- CPU architecture --------------------------------------------------------
#if defined(__wasm64__)
#   define ENP_ARCH_WASM64 1
#elif defined(__wasm32__) || defined(__wasm__)
#   define ENP_ARCH_WASM32 1
#elif defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64)
#   define ENP_ARCH_X64 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#   define ENP_ARCH_ARM64 1
#elif defined(__i386__) || defined(_M_IX86)
#   define ENP_ARCH_X86 1
#elif defined(__arm__) || defined(_M_ARM)
#   define ENP_ARCH_ARM 1
#else
#   define ENP_ARCH_UNKNOWN 1
#endif

// Background threads are available everywhere except single-threaded wasm builds.
#if !defined(ENP_OS_WEB) || defined(__EMSCRIPTEN_PTHREADS__)
#   define ENP_HAS_THREADS 1
#endif

namespace enp::os {

constexpr const char* osName()
{
#if defined(ENP_OS_WEB)
    return "Web";
#elif defined(ENP_OS_ANDROID)
    return "Android";
#elif defined(ENP_OS_WINDOWS)
    return "Windows";
#elif defined(ENP_OS_MACOS)
    return "macOS";
#else
    return "Linux";
#endif
}

constexpr const char* archName()
{
#if defined(ENP_ARCH_WASM64)
    return "wasm64";
#elif defined(ENP_ARCH_WASM32)
    return "wasm32";
#elif defined(ENP_ARCH_X64)
    return "x86_64";
#elif defined(ENP_ARCH_ARM64)
    return "aarch64";
#elif defined(ENP_ARCH_X86)
    return "x86";
#elif defined(ENP_ARCH_ARM)
    return "arm";
#else
    return "unknown";
#endif
}

constexpr bool isDesktop()
{
#if defined(ENP_OS_DESKTOP)
    return true;
#else
    return false;
#endif
}

constexpr bool isMobile()
{
#if defined(ENP_OS_ANDROID)
    return true;
#else
    return false;
#endif
}

constexpr bool isWeb()
{
#if defined(ENP_OS_WEB)
    return true;
#else
    return false;
#endif
}

} // namespace enp::os
