// Debug service: who holds a block. Compiled only with ENP_MEMORY_TRACE (CMake option, off by
// default; preset linux-memtrace). Started with ENP_MEM_TRACE=<scope> in the environment, every
// live allocation of that scope keeps its call stack, in the measurement pool (os::mem::pool), never
// on the heap it watches. markMemoryTrace() forgets the current blocks; dumpLiveMemory() prints the
// stacks of the blocks allocated since the mark that are still live, as module(+offset) lines for
// addr2line -e <module> -f -C -i <offset>. Without the option every function here is an inline
// no-op, so callers need no #if.
#pragma once

#include <cstdio>

namespace enp::services::debug {

#if ENP_MEMORY_TRACE
// Reads ENP_MEM_TRACE and hooks the allocator; false when unset or on a platform without
// backtrace(). Call first thing in main().
bool installMemoryTrace();
// Compiled in and started (ENP_MEM_TRACE was set): marks and dumps mean something.
bool memoryTraceActive();
void markMemoryTrace();
void dumpLiveMemory(std::FILE* out);
#else
inline bool installMemoryTrace() { return false; }
inline bool memoryTraceActive() { return false; }
inline void markMemoryTrace() {}
inline void dumpLiveMemory(std::FILE*) {}
#endif

} // namespace enp::services::debug
