// Debug service: call stacks of live blocks (see the header). Runs inside operator new through the
// allocator's hooks, so nothing here allocates on the heap: the table is carved from the
// measurement pool, the stack scratch is on the stack.
#include "enp/services/debug/MemoryTrace.h"

#include "enp/os/Memory.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>

#if defined(__linux__) && !defined(__ANDROID__)
#   include <execinfo.h>
#   define ENP_HAS_BACKTRACE 1
#else
#   define ENP_HAS_BACKTRACE 0
#endif

namespace enp::services::debug {
namespace {

#if ENP_HAS_BACKTRACE
constexpr std::size_t kCapacity = 1u << 16; // live blocks of one scope (~9 MB of the pool)
constexpr int kFrames = 14;
struct Entry {
    void* ptr = nullptr; // nullptr: empty; (void*)1: tombstone, keeps later probes reachable
    std::uint64_t size = 0;
    std::uint64_t serial = 0;
    int frames = 0;
    void* stack[kFrames];
};
Entry* g_table = nullptr;
const char* g_scope = nullptr;
std::atomic<std::uint64_t> g_serial{0};
std::uint64_t g_mark = 0;
std::mutex g_mutex;
thread_local bool t_inside = false;

std::size_t slot(void* p) { return ((reinterpret_cast<std::uintptr_t>(p) >> 4) * 0x9E3779B97F4A7C15ull >> 48) & (kCapacity - 1); }

void onAllocate(void* p, std::size_t size, const char* scope)
{
    if (t_inside || std::strcmp(scope, g_scope) != 0)
        return;
    t_inside = true; // backtrace() may allocate (with malloc) the first time
    void* stack[kFrames + 2];
    const int n = backtrace(stack, kFrames + 2);
    std::lock_guard<std::mutex> lock(g_mutex);
    std::size_t i = slot(p);
    for (std::size_t probe = 0; probe < kCapacity; ++probe, i = (i + 1) & (kCapacity - 1))
        if (!g_table[i].ptr || g_table[i].ptr == reinterpret_cast<void*>(1)) {
            Entry& e = g_table[i];
            e.ptr = p;
            e.size = size;
            e.serial = ++g_serial;
            e.frames = std::max(0, n - 2); // without this function and operator new
            std::memcpy(e.stack, stack + 2, std::size_t(e.frames) * sizeof(void*));
            break;
        }
    t_inside = false;
}

void onRelease(void* p)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    std::size_t i = slot(p);
    for (std::size_t probe = 0; probe < kCapacity; ++probe, i = (i + 1) & (kCapacity - 1)) {
        if (g_table[i].ptr == p) {
            g_table[i].ptr = reinterpret_cast<void*>(1);
            return;
        }
        if (!g_table[i].ptr)
            return;
    }
}
#endif

} // namespace

bool installMemoryTrace()
{
#if ENP_HAS_BACKTRACE
    const char* name = std::getenv("ENP_MEM_TRACE");
    if (!name || !*name || g_table)
        return g_table != nullptr;
    g_table = static_cast<Entry*>(os::mem::pool::allocate(kCapacity * sizeof(Entry)));
    if (!g_table)
        return false;
    std::memset(static_cast<void*>(g_table), 0, kCapacity * sizeof(Entry));
    g_scope = name;
    os::mem::setHooks(&onAllocate, &onRelease);
    return true;
#else
    return false;
#endif
}

bool memoryTraceActive()
{
#if ENP_HAS_BACKTRACE
    return g_table != nullptr;
#else
    return false;
#endif
}

void markMemoryTrace()
{
#if ENP_HAS_BACKTRACE
    std::lock_guard<std::mutex> lock(g_mutex);
    g_mark = g_serial.load();
#endif
}

void dumpLiveMemory(std::FILE* out)
{
#if ENP_HAS_BACKTRACE
    if (!g_table) {
        std::fprintf(out, "(no trace: start with ENP_MEM_TRACE=<scope>)\n");
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    std::uint64_t total = 0;
    int blocks = 0;
    for (std::size_t i = 0; i < kCapacity; ++i) {
        const Entry& e = g_table[i];
        if (reinterpret_cast<std::uintptr_t>(e.ptr) <= 1 || e.serial <= g_mark)
            continue;
        ++blocks;
        total += e.size;
        std::fprintf(out, "live block %p, %llu bytes, scope %s:\n", e.ptr, (unsigned long long)e.size, g_scope);
        std::fflush(out);
        backtrace_symbols_fd(e.stack, e.frames, fileno(out));
    }
    std::fprintf(out, "%d live block(s), %llu bytes, allocated since the mark\n", blocks, (unsigned long long)total);
#else
    std::fprintf(out, "(memory trace: not supported on this platform)\n");
#endif
}

} // namespace enp::services::debug
