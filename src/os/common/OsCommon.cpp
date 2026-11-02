#include "../OsInternal.h"

#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#   ifndef NOMINMAX
#       define NOMINMAX
#   endif
#   ifndef WIN32_LEAN_AND_MEAN
#       define WIN32_LEAN_AND_MEAN
#   endif
#   include <windows.h>
#   define PSAPI_VERSION 2 // K32GetProcessMemoryInfo from kernel32: no psapi.dll on Windows 7+
#   include <psapi.h>
#   include <tlhelp32.h>
#   include <malloc.h>
#elif defined(__linux__) || defined(__ANDROID__)
#   include <dirent.h>
#   include <malloc.h>
#endif

namespace enp::os {

namespace {
std::vector<std::string> g_arguments;
bool g_initialized = false;
} // namespace

bool init(const InitParams& params)
{
    if (g_initialized)
        return true;
    g_arguments.clear();
    g_initialized = detail::platformInit(params, g_arguments);
    return g_initialized;
}

void shutdown()
{
    if (!g_initialized)
        return;
    detail::platformShutdown();
    g_initialized = false;
}

ProcessStats processStats()
{
    ProcessStats s;
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) {
        s.residentBytes = memory.WorkingSetSize;
        s.privateBytes = memory.PrivateUsage;
    }
    DWORD handles = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &handles))
        s.handles = int(handles);
    s.gdiObjects = int(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS));
    s.userObjects = int(GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS));
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        const DWORD pid = GetCurrentProcessId();
        for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry))
            if (entry.th32OwnerProcessID == pid)
                ++s.threads;
        CloseHandle(snapshot);
    }
#elif defined(__linux__) || defined(__ANDROID__)
    if (FILE* f = std::fopen("/proc/self/status", "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), f)) {
            unsigned long long value = 0;
            if (std::sscanf(line, "VmRSS: %llu", &value) == 1)
                s.residentBytes = value * 1024;
            else if (std::sscanf(line, "RssAnon: %llu", &value) == 1)
                s.privateBytes = value * 1024;
            else if (std::sscanf(line, "Threads: %llu", &value) == 1)
                s.threads = int(value);
        }
        std::fclose(f);
    }
    if (DIR* dir = ::opendir("/proc/self/fd")) {
        while (dirent* d = ::readdir(dir))
            if (d->d_name[0] != '.')
                ++s.handles;
        ::closedir(dir);
        --s.handles; // the directory handle itself
    }
#endif
    return s;
}

const std::vector<std::string>& arguments()
{
    return g_arguments;
}

double now()
{
    using Clock = std::chrono::steady_clock;
    static const Clock::time_point start = Clock::now();
    return std::chrono::duration<double>(Clock::now() - start).count();
}

namespace log {

void write(Level level, const char* fmt, ...)
{
    char buffer[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    detail::writeLogSink(level, buffer);
}

} // namespace log

namespace detail {

std::string toLowerAscii(std::string s)
{
    for (char& c : s)
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    return s;
}

std::string shellQuote(const std::string& s)
{
    std::string out = "'";
    for (char c : s) {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    out += "'";
    return out;
}

bool isUrl(std::string_view s)
{
    if (s.rfind("mailto:", 0) == 0)
        return true;
    size_t pos = s.find("://");
    if (pos == std::string_view::npos || pos == 0)
        return false;
    for (size_t i = 0; i < pos; ++i) {
        char c = s[i];
        if (!(std::isalnum((unsigned char)c) || c == '+' || c == '-' || c == '.'))
            return false;
    }
    return true;
}

std::string percentDecode(std::string_view s)
{
    auto hexValue = [](char c) {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = hexValue(s[i + 1]);
            int lo = hexValue(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += char(hi * 16 + lo);
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

} // namespace detail

void trimMemory()
{
#if defined(_WIN32)
    _heapmin(); // CRT heap: coalesces free blocks and decommits what it can
    SetProcessWorkingSetSize(GetCurrentProcess(), SIZE_T(-1), SIZE_T(-1));
#elif defined(__GLIBC__)
    malloc_trim(0); // returns free pages at the top of the heap and inside it (MADV_DONTNEED)
#elif defined(__ANDROID__) && defined(M_PURGE) && __ANDROID_API__ >= 26
    mallopt(M_PURGE, 0); // bionic: release cached pages (mallopt is API 26+; minSdk is 24)
#endif
}

} // namespace enp::os
