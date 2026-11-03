// Heap accounting: see include/enp/os/Memory.h. Nothing in here may allocate through operator new.
#include "enp/os/Memory.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>

#if defined(_WIN32)
#   include <malloc.h>
#endif

#ifndef ENP_MEM_POOL_BYTES
#   define ENP_MEM_POOL_BYTES (16u << 20)
#endif

namespace enp::os::mem {

// ---- The measurement pool -----------------------------------------------------------------------
// A fixed static array managed first-fit: blocks carry a 16-byte header (size, previous block's
// size, free flag) in address order; a freed block merges with free neighbours. Simple on purpose:
// it serves a handful of long-lived records (snapshots, the trace table) and probes' bookkeeping,
// and it must work from the first allocation the program makes, before any constructor ran.
namespace pool {
namespace {

constexpr std::size_t kAlign = 16;
struct Block {
    std::uint64_t size;     // payload bytes
    std::uint32_t prevSize; // payload bytes of the block before (0 for the first)
    std::uint32_t free;
};
static_assert(sizeof(Block) == 16, "block headers keep 16-byte alignment");

alignas(kAlign) unsigned char g_pool[ENP_MEM_POOL_BYTES];
bool g_init = false;
std::size_t g_used = 0, g_peak = 0;
int g_blocks = 0;
std::mutex& poolMutex()
{
    static std::mutex m;
    return m;
}
std::size_t roundUp(std::size_t n) { return (n + kAlign - 1) & ~(kAlign - 1); }
Block* first() { return reinterpret_cast<Block*>(g_pool); }
Block* next(Block* b)
{
    unsigned char* p = reinterpret_cast<unsigned char*>(b) + sizeof(Block) + b->size;
    return p + sizeof(Block) <= g_pool + sizeof(g_pool) ? reinterpret_cast<Block*>(p) : nullptr;
}
Block* prev(Block* b)
{
    return reinterpret_cast<unsigned char*>(b) == g_pool ? nullptr
                                                          : reinterpret_cast<Block*>(reinterpret_cast<unsigned char*>(b) - b->prevSize - sizeof(Block));
}
void init()
{
    if (g_init)
        return;
    Block* b = first();
    b->size = sizeof(g_pool) - sizeof(Block);
    b->prevSize = 0;
    b->free = 1;
    g_init = true;
}

} // namespace

void* allocate(std::size_t size)
{
    std::lock_guard<std::mutex> lock(poolMutex());
    init();
    const std::size_t want = roundUp(size ? size : 1);
    for (Block* b = first(); b; b = next(b)) {
        if (!b->free || b->size < want)
            continue;
        if (b->size >= want + sizeof(Block) + kAlign) { // split: the rest stays free
            Block* rest = reinterpret_cast<Block*>(reinterpret_cast<unsigned char*>(b) + sizeof(Block) + want);
            rest->size = b->size - want - sizeof(Block);
            rest->prevSize = std::uint32_t(want);
            rest->free = 1;
            if (Block* after = next(rest))
                after->prevSize = std::uint32_t(rest->size);
            b->size = want;
        }
        b->free = 0;
        g_used += b->size;
        g_peak = std::max(g_peak, g_used);
        ++g_blocks;
        return reinterpret_cast<unsigned char*>(b) + sizeof(Block);
    }
    return nullptr;
}

void release(void* p)
{
    if (!p)
        return;
    std::lock_guard<std::mutex> lock(poolMutex());
    Block* b = reinterpret_cast<Block*>(static_cast<unsigned char*>(p) - sizeof(Block));
    if (reinterpret_cast<unsigned char*>(b) < g_pool || reinterpret_cast<unsigned char*>(b) >= g_pool + sizeof(g_pool) || b->free)
        return;
    g_used -= b->size;
    --g_blocks;
    b->free = 1;
    if (Block* after = next(b); after && after->free) { // merge with the next
        b->size += sizeof(Block) + after->size;
    }
    if (Block* before = prev(b); before && before->free) { // merge into the previous
        before->size += sizeof(Block) + b->size;
        b = before;
    }
    if (Block* after = next(b))
        after->prevSize = std::uint32_t(b->size);
}

Usage usage()
{
    std::lock_guard<std::mutex> lock(poolMutex());
    return {sizeof(g_pool), g_used, g_peak, g_blocks};
}

} // namespace pool

namespace {

constexpr std::size_t kHeader = 16;

struct Header {
    std::uint64_t size;
    std::uint32_t scope;
    std::uint32_t magic;
};
static_assert(sizeof(Header) == kHeader, "the header keeps the default new alignment");
constexpr std::uint32_t kMagic = 0x454E504Du; // "ENPM"

struct Registry {
    std::atomic<std::uint64_t> bytes[kMaxScopes];
    std::atomic<std::uint64_t> blocks[kMaxScopes];
    std::atomic<std::uint64_t> allocations{0};
    const char* names[kMaxScopes] = {"other", "static-init"};
    std::atomic<int> count{2};
    std::mutex registration; // only when a scope is first seen
};
// Constant-initialised: usable from the very first allocation, before any constructor ran.
Registry& registry()
{
    static Registry r;
    return r;
}
thread_local int t_scope = 0;
[[maybe_unused]] constexpr int kStaticInit = 1; // registry().names[1]
std::atomic<bool> g_programStarted{false}; // constant-initialised: false before any constructor

int intern(const char* name)
{
    Registry& r = registry();
    const int n = r.count.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        if (r.names[i] == name || std::strcmp(r.names[i], name) == 0)
            return i;
    std::lock_guard<std::mutex> lock(r.registration);
    const int m = r.count.load(std::memory_order_relaxed);
    for (int i = n; i < m; ++i)
        if (std::strcmp(r.names[i], name) == 0)
            return i;
    if (m >= kMaxScopes)
        return 0;
    r.names[m] = name;
    r.count.store(m + 1, std::memory_order_release);
    return m;
}

std::atomic<AllocateHook> g_onAllocate{nullptr};
std::atomic<ReleaseHook> g_onRelease{nullptr};

} // namespace

void programStarted()
{
    g_programStarted.store(true, std::memory_order_relaxed);
}

void setHooks(AllocateHook onAllocate, ReleaseHook onRelease)
{
    g_onAllocate.store(onAllocate, std::memory_order_release);
    g_onRelease.store(onRelease, std::memory_order_release);
}

#if ENP_MEMORY_ACCOUNTING

bool enabled() { return true; }

namespace {

void* allocate(std::size_t size, std::size_t alignment)
{
    // The header sits right before the returned pointer; `alignment` bytes (at least the header)
    // are reserved in front so that the result keeps the requested alignment.
    const std::size_t front = std::max(alignment, kHeader);
    const std::size_t total = size + front;
    void* raw = nullptr;
    if (alignment <= kHeader) {
        raw = std::malloc(total);
    } else {
#if defined(_WIN32)
        raw = _aligned_malloc(total, alignment);
#else
        if (posix_memalign(&raw, alignment, total) != 0)
            raw = nullptr;
#endif
    }
    if (!raw)
        return nullptr;
    char* p = static_cast<char*>(raw) + front;
    Header* h = reinterpret_cast<Header*>(p - kHeader);
    int scope = t_scope;
    if (scope == 0 && !g_programStarted.load(std::memory_order_relaxed))
        scope = kStaticInit; // a static constructor, before main()
    h->size = size;
    h->scope = std::uint32_t(scope);
    h->magic = kMagic;
    Registry& r = registry();
    r.bytes[scope].fetch_add(size, std::memory_order_relaxed);
    r.blocks[scope].fetch_add(1, std::memory_order_relaxed);
    r.allocations.fetch_add(1, std::memory_order_relaxed);
    if (AllocateHook hook = g_onAllocate.load(std::memory_order_acquire))
        hook(p, size, r.names[scope]);
    return p;
}

void releaseBlock(void* p, std::size_t alignment)
{
    if (!p)
        return;
    const std::size_t front = std::max(alignment, kHeader);
    Header* h = reinterpret_cast<Header*>(static_cast<char*>(p) - kHeader);
    if (h->magic == kMagic) {
        Registry& r = registry();
        const int scope = int(h->scope) < kMaxScopes ? int(h->scope) : 0;
        r.bytes[scope].fetch_sub(h->size, std::memory_order_relaxed);
        r.blocks[scope].fetch_sub(1, std::memory_order_relaxed);
        h->magic = 0;
        if (ReleaseHook hook = g_onRelease.load(std::memory_order_acquire))
            hook(p);
    }
    void* raw = static_cast<char*>(p) - front;
    if (alignment <= kHeader)
        std::free(raw);
    else {
#if defined(_WIN32)
        _aligned_free(raw);
#else
        std::free(raw);
#endif
    }
}

void* allocateOrThrow(std::size_t size, std::size_t alignment)
{
    if (void* p = allocate(size ? size : 1, alignment))
        return p;
    throw std::bad_alloc();
}

} // namespace
#else
bool enabled() { return false; }
#endif

Stats stats()
{
    Stats s;
    Registry& r = registry();
    const int n = r.count.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        s.liveBytes += r.bytes[i].load(std::memory_order_relaxed);
        s.liveBlocks += r.blocks[i].load(std::memory_order_relaxed);
    }
    s.totalAllocations = r.allocations.load(std::memory_order_relaxed);
    return s;
}

const Snapshot& snapshot()
{
    // The ring lives in the measurement pool, never on the heap being measured.
    static Snapshot* ring = new (pool::allocate(sizeof(Snapshot) * kSnapshotPool)) Snapshot[kSnapshotPool];
    static std::atomic<unsigned> next{0};
    Snapshot& out = ring[next.fetch_add(1, std::memory_order_relaxed) % kSnapshotPool];
    Registry& r = registry();
    out.count = std::min(kMaxScopes, r.count.load(std::memory_order_acquire));
    out.total = Stats{};
    for (int i = 0; i < out.count; ++i) {
        out.scopes[i] = {r.names[i], r.bytes[i].load(std::memory_order_relaxed), r.blocks[i].load(std::memory_order_relaxed)};
        out.total.liveBytes += out.scopes[i].liveBytes;
        out.total.liveBlocks += out.scopes[i].liveBlocks;
    }
    out.total.totalAllocations = r.allocations.load(std::memory_order_relaxed);
    return out;
}

std::uint64_t Snapshot::liveBytes(const char* scope) const
{
    for (int i = 0; i < count; ++i)
        if (std::strcmp(scopes[i].name, scope) == 0)
            return scopes[i].liveBytes;
    return 0;
}

std::uint64_t liveBytes(const char* scope)
{
    Registry& r = registry();
    const int n = r.count.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        if (std::strcmp(r.names[i], scope) == 0)
            return r.bytes[i].load(std::memory_order_relaxed);
    return 0;
}

Scope::Scope(const char* name)
{
    previous_ = t_scope;
    t_scope = intern(name);
}

Scope::~Scope()
{
    t_scope = previous_;
}

} // namespace enp::os::mem

#if ENP_MEMORY_ACCOUNTING
// ---- Replacement global allocation functions ---------------------------------------------------
using enp::os::mem::allocateOrThrow;
using enp::os::mem::allocate;
using enp::os::mem::releaseBlock;
constexpr std::size_t kDefaultAlign = __STDCPP_DEFAULT_NEW_ALIGNMENT__;

void* operator new(std::size_t size) { return allocateOrThrow(size, kDefaultAlign); }
void* operator new[](std::size_t size) { return allocateOrThrow(size, kDefaultAlign); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return allocate(size ? size : 1, kDefaultAlign); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return allocate(size ? size : 1, kDefaultAlign); }
void operator delete(void* p) noexcept { releaseBlock(p, kDefaultAlign); }
void operator delete[](void* p) noexcept { releaseBlock(p, kDefaultAlign); }
void operator delete(void* p, std::size_t) noexcept { releaseBlock(p, kDefaultAlign); }
void operator delete[](void* p, std::size_t) noexcept { releaseBlock(p, kDefaultAlign); }
void operator delete(void* p, const std::nothrow_t&) noexcept { releaseBlock(p, kDefaultAlign); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { releaseBlock(p, kDefaultAlign); }

void* operator new(std::size_t size, std::align_val_t al) { return allocateOrThrow(size, std::size_t(al)); }
void* operator new[](std::size_t size, std::align_val_t al) { return allocateOrThrow(size, std::size_t(al)); }
void* operator new(std::size_t size, std::align_val_t al, const std::nothrow_t&) noexcept { return allocate(size ? size : 1, std::size_t(al)); }
void* operator new[](std::size_t size, std::align_val_t al, const std::nothrow_t&) noexcept { return allocate(size ? size : 1, std::size_t(al)); }
void operator delete(void* p, std::align_val_t al) noexcept { releaseBlock(p, std::size_t(al)); }
void operator delete[](void* p, std::align_val_t al) noexcept { releaseBlock(p, std::size_t(al)); }
void operator delete(void* p, std::size_t, std::align_val_t al) noexcept { releaseBlock(p, std::size_t(al)); }
void operator delete[](void* p, std::size_t, std::align_val_t al) noexcept { releaseBlock(p, std::size_t(al)); }
void operator delete(void* p, std::align_val_t al, const std::nothrow_t&) noexcept { releaseBlock(p, std::size_t(al)); }
void operator delete[](void* p, std::align_val_t al, const std::nothrow_t&) noexcept { releaseBlock(p, std::size_t(al)); }
#endif
