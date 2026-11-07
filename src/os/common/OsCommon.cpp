#include "../OsInternal.h"
#include "enp/os/Memory.h"

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
#elif defined(__APPLE__)
#   include <dirent.h>
#   include <mach/mach.h>
#   include <malloc/malloc.h>
#endif
#include <cstddef>

// AddressSanitizer replaces the allocator: its statistics would describe ASan's, not the app's.
#if defined(__SANITIZE_ADDRESS__)
#   define ENP_UNDER_ASAN 1
#elif defined(__has_feature)
#   if __has_feature(address_sanitizer)
#       define ENP_UNDER_ASAN 1
#   endif
#endif
#ifndef ENP_UNDER_ASAN
#   define ENP_UNDER_ASAN 0
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
    mem::programStarted(); // from here, unscoped allocations are "other", not "static-init"
#if defined(__GLIBC__)
    // Fixed allocator thresholds instead of glibc's dynamic ones (docs/MEMORY.md rule 9). glibc
    // raises both after every large free (up to 32 MB for mmap, 64 MB for trimming), and then:
    // large blocks come from an arena instead of their own mapping, and the free top of a thread's
    // arena (where background work, such as the IntelliSense index, allocates) is never given back
    // - malloc_trim() does not release a thread arena's top, only free() past this threshold does.
    // Fixed: a block of 128 KB or more is its own mapping, unmapped when freed, whichever thread
    // made it; an arena gives back its free top beyond 128 KB (glibc's own starting value) as soon
    // as it is freed: a thread arena keeps at most that much after its work.
    mallopt(M_MMAP_THRESHOLD, 128 * 1024);
    mallopt(M_TRIM_THRESHOLD, 128 * 1024);
#endif
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
            else if (std::sscanf(line, "RssShmem: %llu", &value) == 1)
                s.sharedBytes = value * 1024;
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
#   if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33)) && !ENP_UNDER_ASAN
    // Every arena (main and threads') and every block mapped on its own. Locks each arena briefly:
    // fine once a second, not per allocation.
    const struct mallinfo2 info = mallinfo2();
    s.mallocInUseBytes = info.uordblks + info.hblkhd;
    s.mallocHeldBytes = info.arena + info.hblkhd;
#   endif
#elif defined(__APPLE__)
    // Mach's own accounting: phys_footprint is what Activity Monitor shows as the app's memory
    // (dirty private pages, compressed ones included, plus IOKit/GPU memory the process owns),
    // the macOS counterpart of Windows' private bytes and Linux's anonymous memory.
    task_vm_info_data_t vm{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vm), &count) == KERN_SUCCESS) {
        s.residentBytes = vm.resident_size;
        s.privateBytes = vm.phys_footprint;
        s.compressedBytes = vm.compressed;
        // The graphics ledger came with a later revision of the structure (macOS 12): read only
        // when the kernel filled that far.
        if (size_t(count) * sizeof(natural_t) >= offsetof(task_vm_info_data_t, ledger_tag_graphics_footprint) + sizeof(vm.ledger_tag_graphics_footprint))
            s.graphicsBytes = vm.ledger_tag_graphics_footprint;
    }
#   if !ENP_UNDER_ASAN
    // Every malloc zone (the default one, and any a library made): in use and held.
    malloc_statistics_t zones{};
    malloc_zone_statistics(nullptr, &zones);
    s.mallocInUseBytes = zones.size_in_use;
    s.mallocHeldBytes = zones.size_allocated;
#   endif
    // The thread list is a send right per thread in memory the kernel allocated for us: both are
    // given back before returning (one right or page kept per call would be a leak per frame).
    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t threadCount = 0;
    if (task_threads(mach_task_self(), &threads, &threadCount) == KERN_SUCCESS) {
        s.threads = int(threadCount);
        for (mach_msg_type_number_t i = 0; i < threadCount; ++i)
            mach_port_deallocate(mach_task_self(), threads[i]);
        vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads), vm_size_t(threadCount * sizeof(thread_act_t)));
    }
    if (DIR* dir = ::opendir("/dev/fd")) {
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

#if !defined(__ANDROID__)
namespace fs {

// Only Android keeps files outside the app's folders behind a permission (OsAndroid.cpp).
bool hasStorageAccess() { return true; }
void requestStorageAccess() {}

} // namespace fs
#endif

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
#elif defined(__APPLE__) && defined(ENP_APPLE_MALLOC_PRESSURE_RELIEF)
    // Best-effort only: some zones, including current xzone malloc, use a no-op callback here.
    malloc_zone_pressure_relief(nullptr, 0);
#endif
}

} // namespace enp::os
