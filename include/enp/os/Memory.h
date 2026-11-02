// Heap accounting: every C++ allocation in the program is counted, by subsystem, so that memory
// that stays behind names its owner (docs/MEMORY.md). ENP_MEMORY_ACCOUNTING (CMake option, on by default) replaces
// the global operator new/delete with versions that keep a 16-byte header per block; the cost is
// one relaxed atomic add per allocation and per free. Off, everything here is a no-op.
//
// Rules (docs/MEMORY.md): a subsystem that owns state wraps the code that builds it in a Scope;
// what it frees is attributed back to that scope whatever thread or scope frees it, so a scope's
// live bytes are exactly what it still holds. The unit tests open and close a workspace in a loop
// and require every scope to be back at zero and the total to be flat.
#pragma once

#include <cstdint>

namespace enp::os::mem {

struct Stats {
    std::uint64_t liveBytes = 0;        // bytes requested by blocks not yet freed (headers excluded)
    std::uint64_t liveBlocks = 0;
    std::uint64_t totalAllocations = 0; // since the program started
};

struct ScopeStats {
    const char* name = "";
    std::uint64_t liveBytes = 0;
    std::uint64_t liveBlocks = 0;
};

// Compiled in?
bool enabled();
Stats stats();
std::uint64_t liveBytes(const char* scope);

// The measurement pool: a fixed static block of ENP_MEM_POOL_BYTES (16 MB unless the build says
// otherwise; untouched pages cost nothing) with a simple first-fit manager. Everything that
// measures memory (snapshots, the allocation trace) lives here and nowhere else, so measuring
// never touches the heap it measures. Also available to benchmarks and probes.
namespace pool {
    // 16-byte aligned; nullptr when the pool is exhausted (never falls back to the heap).
    void* allocate(std::size_t size);
    void release(void* p);
    struct Usage {
        std::size_t capacity = 0;
        std::size_t used = 0;     // bytes in live blocks (headers excluded)
        std::size_t peakUsed = 0;
        int blocks = 0;
    };
    Usage usage();
} // namespace pool

// A snapshot of every scope that was ever entered (0 is "other" for unattributed allocations),
// in the order they were first entered, with the totals. Measuring must not move what it
// measures: snapshots are plain records filled into a static pool (a ring of kSnapshotPool
// entries) and nothing on this path allocates. Copy one to keep it beyond the next
// kSnapshotPool calls.
constexpr int kMaxScopes = 64;
constexpr int kSnapshotPool = 8;
struct Snapshot {
    ScopeStats scopes[kMaxScopes];
    int count = 0;
    Stats total;
    // Live bytes of a scope by name, 0 if it was never entered.
    std::uint64_t liveBytes(const char* scope) const;
};
const Snapshot& snapshot();

// Hooks for a debug service (services/debug/MemoryTrace, compiled only with ENP_MEMORY_TRACE):
// called for every allocation and free with the owning scope's name, from inside operator new,
// so a hook must not allocate through it. Null by default; set once at start-up.
using AllocateHook = void (*)(void* block, std::size_t size, const char* scope);
using ReleaseHook = void (*)(void* block);
void setHooks(AllocateHook onAllocate, ReleaseHook onRelease);

// Empties a container or string and gives its storage back. clear() keeps the capacity, and so
// does assigning {} to a std::string (libstdc++ reuses the buffer): a value that is reset on
// every workspace change keeps its largest size forever unless it is released like this.
template <class T>
void release(T& value)
{
    T().swap(value);
}

// RAII: allocations made on this thread while it lives belong to `name`. Pass a string literal
// (the pointer is kept). Nests; the previous scope is restored on destruction.
class Scope {
public:
    explicit Scope(const char* name);
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    int previous_ = 0;
};

} // namespace enp::os::mem
