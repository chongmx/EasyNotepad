# Memory: how it is owned, measured and kept honest

EasyNotepad is C++17 with no garbage collector, so memory safety is a matter of ownership rules,
and this document is the contract every change is held to. It is enforced four ways: by the
compiler (RAII and `unique_ptr`), by the heap accounting built into every build, by unit and UI
tests that fail when an open/close cycle keeps a byte, and by a script that measures the real app
the way a user does, in a task manager.

**Definition of done for any change that holds memory:** closing the folder (or the document)
gives everything back. Concretely, all of these hold:

1. `enp_tests` and `enp_ui_tests` pass (section 4).
2. `scripts/memory_cycle.py` passes on a large folder with the files the change touches (section 5):
   after every close the C++ heap is at or below the fresh start and identical from cycle to
   cycle; after the warm-up cycles, private memory may oscillate but never climbs, cycle after
   cycle; and handles, GDI/USER objects and threads do not grow. **Oscillation is acceptable;
   one-way growth is not, not even a few hundred bytes a cycle.**

## 1. Ownership rules

1. **No raw `new`, `delete`, `malloc` or `free` outside `src/os/`.** Everything is owned by a
   value, a `std::unique_ptr`, a container, or (rarely, for shared lifetimes across threads) a
   `std::shared_ptr`. The unit test `source_rules_no_raw_allocation_outside_the_os_layer` scans the
   sources and fails on a violation. The OS layer is the one place that talks to C APIs and wraps
   every handle in an RAII type before it leaves the layer. A C library that wants allocator hooks
   (stb_image in `ImageDecoder.cpp`) is pointed at `::operator new`/`delete`, so its memory is
   accounted like the rest.
2. **A view-model owns its state; a view only caches.** View-models (`include/enp/viewmodel`)
   own documents, the explorer tree, git state, the layer map, search results. Views may keep
   render caches keyed by document id, and must drop them when the document goes:
   `EditorView::pruneClosed()` is the model for this. Never keep a pointer to a document or node
   across frames; keep its id.
3. **Everything that belongs to a workspace is released by `workspaceChanged()`.** Opening or
   closing a folder calls it on every subsystem (`LayersViewModel`, `LogicalMapViewModel`,
   `GitViewModel`, `IntellisenseViewModel`, find-in-files). A new subsystem that keeps per-workspace state adds a
   `workspaceChanged()` and is called from `NotepadViewModel::openFolder()` and `closeFolder()`.
4. **Background work holds a `weak_ptr` to its owner, and work about a workspace stops with it.**
   Work posted with `os::dispatch::runAsync` captures a `std::weak_ptr<bool> alive` and a
   generation, and drops its result if the owner is gone or the generation moved on.
   `workspaceChanged()` cancels work still running about the old folder and bumps the
   generation: a running task holds its inputs until it ends, and a late answer must never land
   in the next folder.
5. **Reset with `os::mem::release(x)`, not `clear()` or `x = {}`.** `clear()` keeps a
   container's capacity, and libstdc++ keeps a `std::string`'s buffer even when `{}` is assigned,
   so a member that is reset on every workspace change keeps its largest size forever.
   `release()` swaps in an empty value and frees the storage.
6. **Buffers rebuilt every frame shrink when the peak has passed.** A vector filled per frame
   (the automation registry of clickable elements, layout rows) keeps the capacity of the busiest
   frame it ever saw. Give the room back once the content is far smaller (`UiAutomation::
   endFrame()`: more than 4x the size plus 64 elements).
7. **GPU textures are owned like heap memory.** A texture created through ImGui's texture
   protocol (`MarkdownImages`) has one owner, is shared by reference count, is flagged for the
   backend to destroy when the last user lets go, and is freed once the backend reports it
   destroyed. Its pixels count in the `imgui` scope. An image that cannot be shown (missing,
   remote, not decodable) is cached as such and drawn as a placeholder; nothing is retried per frame.
   Emoji pictures (`EmojiCache`) are shared by every view and held only while drawn: one not drawn
   for 30 s, or not on screen when the app trims after a close, is let go, at most 512 are held,
   and with none held the pack's index (about 200 KB) goes too. A large resource is read in parts
   (`os::fs::readAssetRange`), never whole.
8. **Dear ImGui keeps windows forever and never shrinks on its own.** Tables (an `ImGuiTable` per
   id) are freed by the trim after a close when not drawn in the last frame (`TableRemove`). Use one child window per
   role with `SetNextWindowScroll` to swap content, never one window per document
   (`"##editorText"`, `"##markdownText"`, `"##fileView"` in `EditorView`); never put a document id,
   path or counter in a window name. Popups and scrolling tables are windows as well: a context
   menu opened with `BeginPopupContextItem` inside a per-row `PushID` makes one window per row ever
   right-clicked, so a list has one popup id (`OpenPopup(id)` from the row, `BeginPopupEx(id)` once
   after the table), and a view has one table id whose settings are reset when it shows another
   kind of list (`LibraryView::beginTable`).
9. **Freed is not returned.** The C allocator keeps freed pages for reuse, so a release does not
   show in a task manager by itself. When a folder or the last document closes, the view-model
   sets `takeMemoryTrimRequest()`; `Application::endFrame()` then compacts ImGui's buffers and the
   font atlas and calls `os::trimMemory()` (glibc `malloc_trim`; Windows `_heapmin` and a working
   set trim; macOS `malloc_zone_pressure_relief`, which madvises every zone's free pages away as
   memory pressure would; Android `M_PURGE`) twice: four frames later, once the textures released by the close are destroyed, and
   1.5 s later, once late results of background work are freed too. The app keeps drawing until
   both have run. A change that frees something large elsewhere sets the request too.
   On glibc, `os::init()` also fixes the allocator's thresholds (`mallopt`: 128 KB mmap, 1 MB
   trim) instead of glibc's dynamic ones, which rise after large frees (to 32 and 64 MB): a block
   of 128 KB or more is then always its own mapping, unmapped when freed whichever thread made it,
   and a thread's arena gives its free top back on `free()`. `malloc_trim()` never releases the
   top of a thread's arena, so without this, memory built on a background thread (the
   IntelliSense index) stayed private after its folder closed. Prefer a few large blocks to many
   small ones for big state built off the main thread (`model::SymbolIndex` pools its text).
10. **Preallocate what is allocated over and over.** A scan or a burst that allocates a buffer per
   item, or grows a container by doubling, frees everything at the end and still leaves the heap's
   pages moved around (`tools/malloc-trace/churn.c` shows it, section 3). Take the room once: one
   buffer reserved at the largest size and reused for every item (`os::fs::readFileInto`,
   `IFileService::readInto`: the IntelliSense index build reads 4,000 files into one 2 MB buffer),
   containers reserved from an estimate before they are filled (`SymbolIndex::reserve`,
   `TextBuffer::text()`), and buffers reused while a feature is active (the suggestion list's
   ranking), released when it ends. Read documents in place rather than copying their text.
   Background work that recurs runs on one `os::dispatch::Worker` thread kept by its owner, not a
   new thread each time (`runAsync`): a new thread gets whichever malloc arena is free, and each
   arena keeps up to the trim threshold of its free top, so the same work moved memory between
   arenas from cycle to cycle.

## 2. Heap accounting (`enp/os/Memory.h`)

With `ENP_MEMORY_ACCOUNTING` (CMake option, on by default) the global `operator new`/`delete` are
replaced: each block carries a 16-byte header with its size and the scope that allocated it.
Cost: one relaxed atomic add per allocation and per free, and 16 bytes per block. Dear ImGui's
allocations go through it too (`ImGui::SetAllocatorFunctions` in the platform mains).

```cpp
#include "enp/os/Memory.h"

void LayersViewModel::rebuild()
{
    os::mem::Scope memScope("layers"); // this thread's allocations belong to "layers" until it ends
    ...
}
```

A free is always attributed to the scope that allocated the block, whichever thread or scope
frees it, so a scope's live bytes are exactly what that subsystem still holds. Scopes in use:

| Scope | Holds |
|---|---|
| `documents` | open documents, their text and undo |
| `explorer` | the folder tree |
| `git` | repository state, logs, diffs |
| `layers`, `logical-map` | the Read ribbon's layers and map |
| `find` | find-in-files results |
| `intellisense` | the workspace's symbol index (one text pool and one entry array, `model::SymbolIndex`), open completion suggestions, context chosen for an AI provider, the Back/Forward places (at most 50 each way, by document id and path) (`IntellisenseViewModel`), the question a model is answering and its streamed answer (`CodeAssistant`; the local model's own memory is the engine's): released by `workspaceChanged()`; a background build is cancelled and its late result dropped |
| `markdown` | rendered Markdown and parsed formulas |
| `images` | decoding (the pixels themselves are in `imgui`) |
| `emoji` | the emoji pack's index and reading pictures from it (`EmojiCache`, `services::EmojiPack`) |
| `data-views` | a CSV table or JSON/YAML tree while a view shows it (`DataViews`): freed on close or back to text |
| `library` | what a library or program contains while its view shows it (`LibraryView`, `model::bin::Image`): freed on close or when opened as text |
| `views` | everything else the views allocate (`Application::frame`) |
| `imgui` | Dear ImGui: windows, draw buffers, font atlas, textures |
| `automation` | the automation connector, the app's side of a test bench or the MCP bridge: its connection threads, requests, replies and calls queued for the main thread (`services::AutomationServer`). Ended connections are reaped every 200 ms; idle, it holds 0 bytes, and while a reading is taken it holds exactly that request |
| `app` | the view-model, its services, commands and settings, made at start-up (`MainGlfw.cpp`) |
| `gl-driver` | the GL driver's own C++ allocations (Mesa's LLVM registers itself when the driver loads and compiles shaders on first draws): creating the window and context, each frame's clear, draw, read-back and swap, popped-out windows |
| `static-init` | constructors that run before `main()`, ours and the libraries' (`os::mem::programStarted()`, called by `os::init()`, ends it) |
| `local-engine` | llama.cpp's log ring and backend registry, set up once (`LocalEngineService::initBackend`) |
| `language-tables` | the languages' keyword tables, built once by the view-model's constructor |
| `other` | not attributed. With a folder closed: about 12 KB, the automation request being answered, the dispatch queue's storage and small strings of the main view-model (status text, recent folders) |

**Any new subsystem that builds state of more than a few kilobytes gets its own scope** and a
line in the cycle test (section 4).

Reading the numbers never touches the heap being measured. Everything that measures lives in the
**measurement pool** (`os::mem::pool`): a fixed static block of `ENP_MEM_POOL_BYTES` (16 MB by
default; pages never written cost nothing) with a first-fit manager that never falls back to the
heap. `os::mem::snapshot()` fills one record of a ring in that pool with every scope's live bytes
and blocks; copy a snapshot to keep it. Probes and benchmarks use `pool::allocate()` and
`pool::release()` for their own bookkeeping.

Where to read it:

- Developer mode (seven taps on the version in About): the status bar shows resident and private
  memory, heap bytes and blocks, handles, GDI/USER objects, threads and ImGui's counts.
- The automation endpoint and MCP server: `get_state` returns `memory` (`liveKB`, `blocks`,
  `allocations`, `poolKB`, and `scopes`) alongside `process` and `imgui`.

## 3. Finding who holds a block

1. **Which scope.** Compare `get_state` scopes before and after (the script prints the scopes
   that stayed above the fresh start). If `other` grew, the allocation site has no scope: add one
   and look again.
2. **Which line.** Build the trace: `cmake --preset linux-memtrace && cmake --build
   build/linux-memtrace --target EasyNotepad enp_tests -j 8`. It compiles
   `src/services/debug/MemoryTrace.cpp`; a default build contains none of it. Start the app with
   `ENP_MEM_TRACE=<scope>`, call automation `memory_trace {"action": "mark"}` at the baseline,
   reproduce, then `memory_trace {"action": "dump", "file": "/tmp/live.txt"}`. Every block of that
   scope allocated since the mark and still live is listed with its stack as `module(+offset)`;
   symbolize with `addr2line -e <module> -f -C -i <offset>` and group by the first app function.
   The same calls exist as `markMemoryTrace()` and `dumpLiveMemory()` for probes. Note that the
   preset writes to the same `bin/linux/x64` as the default build: rebuild with the `linux` preset
   before measuring anything else.
3. **Outside the C++ heap.** If the heap is back but private memory is not, diff
   `/proc/<pid>/smaps` by mapping (`[heap]` is the main malloc arena; unnamed mappings are thread
   arenas and stacks; `/dev/nvidia*` and `lib*GL*` are the driver). To see what the allocator holds,
   run the app as a child of gdb (attaching to a running process is blocked where
   `kernel.yama.ptrace_scope` is 1), stop it with SIGINT, and
   `call (void)malloc_stats()` (in-use against system bytes per arena) and
   `call (int)malloc_trim(0)`: if a second trim releases a lot, something is freed after the app's
   trim, so it ran too early. On Windows, watch the developer readout: a climbing handle, GDI or
   USER count names the kind of resource; VMMap and UMDH play the role of smaps and the trace.
4. **Allocations that bypass `operator new`** (C libraries, X11, the GL driver: the accounting cannot
   see them). `tools/malloc-trace/preload.c` is an `LD_PRELOAD` library that records the stack of
   every live `malloc` block of the app (not of its child processes). Build it with
   `gcc -shared -fPIC -O1 -o preload.so tools/malloc-trace/preload.c -ldl -lpthread`, start the app
   with `MTRACE_DIR=<dir> LD_PRELOAD=<dir>/preload.so`, and create `<dir>/dump` after a close to get
   every live block in `<dir>/live.txt`. Take two dumps several cycles apart and run
   `python3 tools/malloc-trace/sites.py first.txt second.txt`: it prints the net bytes per call
   site. Growth that is real stays positive over several pairs of dumps; churn changes sign. The
   app runs several times slower while traced, and its RSS includes the tracer's table.
5. **What is allocated at all** (and freed again): `tools/malloc-trace/churn.c`, built and preloaded
   the same way with `CHURN_DIR=<dir>`, totals every allocation of 1 KB or more per call stack between
   `<dir>/start` and `<dir>/dump`; `python3 tools/malloc-trace/churn.py <dir>/churn.txt 2` groups it by
   the app function that asked. Live bytes can be identical cycle after cycle while hundreds of MB
   are allocated and freed: such buffers move which pages are dirty. Preallocate them (rule 10).
6. **Telling a climb from settling.** Run at least ten cycles and look at the after-close readings
   from cycle 4 on. A per-mapping split (the script prints it) says whose memory it is. A driver
   mapping whose size stays the same while its dirty pages grow is a fixed buffer being written
   for the first time (bounded by its size); a new mapping, or a growing size, is a real leak.
   Say so in the report: that growth is real, one-way and slow (about 0.5 KB per cycle in the
   160-cycle run), and it is the driver's, not ours; it cannot be removed from our side.

## 4. Tests that keep it honest

- `memory_accounting_counts_scopes_and_folder_cycles_release_everything` (`enp_tests`): opens a
  workspace, opens files, builds the layer and logical maps, runs find-in-files, closes the folder;
  after cycle 2 and after cycle 8 the total live bytes, the block count and every scope must be
  **byte-for-byte equal**. Add your subsystem's exercise here. It catches growth per cycle, not
  first-cycle growth or anything the views hold: that is what the script is for.
- `closing_a_folder_stops_the_agent_dissection_of_it` (`enp_tests`): a late agent answer about a
  closed folder is cancelled and ignored (rule 4).
- `closing_a_folder_stops_the_symbol_index_build` (`enp_tests`): the IntelliSense index built on a
  background thread is cancelled by a close, its late result is dropped, and the `intellisense`
  scope is back to the byte afterwards, also when the view-model is destroyed mid-build. The cycle
  test also requires that scope to be exactly 0 after every close.
- `automation_connector_holds_nothing_per_request` (`enp_tests`): the app's side of a test bench
  holds the same after 10 and after 200 requests (0 bytes once they have ended), and nothing once
  stopped. A planted leak of 40 bytes per request fails it (858 -> 16,328 bytes).
- `source_rules_no_raw_allocation_outside_the_os_layer` and `measurement_pool_is_a_fixed_first_fit_manager`.
- `enp_ui_tests`: renders every layout state headless for hundreds of thousands of frames, fails
  on growth of ImGui's live allocations and windows in the steady state, and opens and closes
  rendered Markdown, images and binary documents, requiring that no rendered document, image
  document or texture survives (`renderedMarkdownCount`, `imageDocumentCount`, `imageTextureCount`).
- Sanitizers: `cmake --preset linux-asan && cmake --build --preset linux-asan` builds with
  AddressSanitizer and UndefinedBehaviorSanitizer; LeakSanitizer reports unreachable blocks at exit
  with their stacks. Run it before merging anything that changes ownership.

## 5. Measuring the real app

```bash
python3 scripts/memory_cycle.py --app bin/linux/x64/EasyNotepad --folder ~/code/large-repo \
    --files README.md docs/guide.md assets/logo.png lib/libfoo.so
```

It starts the app with an automation endpoint, reads the fresh start, then opens the folder, the
files and the map, closes the folder, and reads again: eight cycles by default, the first three
a warm-up. It fails when a close does not give the memory back, when the heap after close is not
identical from cycle to cycle, when the malloc heap or the thread arenas climb in every cycle
after the warm-up, when the GPU driver's mappings grow in size, or when handles, GDI/USER objects
or threads grow. On Linux it prints the private memory per mapping class (malloc heap, anonymous,
GPU driver) between the first close after the warm-up and the last. Use a folder the size of a
real project, and include every kind of file the change touches.

What it counts is the whole process; nothing of the app is left out:

| Reading | Linux | macOS | Windows |
|---|---|---|---|
| `private` (checked) | anonymous memory + shared memory the process wrote (`RssAnon` + `RssShmem`: the window's MIT-SHM frame buffer with a software GL driver, memfd buffers) | physical footprint (`TASK_VM_INFO.phys_footprint`, Activity Monitor's "Memory": dirty and compressed pages, IOKit/GPU memory the app owns) | committed private bytes |
| heap, blocks, scopes (checked) | the accounted C++ heap (`os::mem`) | same | same |
| by mapping (checked) | malloc heap, thread arenas, GPU driver mappings (`/proc/<pid>/smaps`) | - | - |
| by kind (trend checked) | C allocator in use / held, every arena (`mallinfo2`) | C allocator in use / held, every zone (`malloc_zone_statistics`); GPU memory charged to the app (Mach graphics ledger); compressed pages | - |
| threads, handles | `/proc/self/status`, `/proc/self/fd` | `task_threads`, `/dev/fd` | toolhelp, handle count, GDI/USER |

Only the bench itself is told apart: the script's own process is not the app's, and the app's
side of the connection is the `automation` scope, which must read the same at every reading (the
script fails otherwise). Before 2026-11, `os::processStats()` had no macOS branch: private,
resident and threads read 0 there, in the status bar and in `get_state`.

Things that make measurements wrong:

- **Commands that start the agent CLI** (Dissect Project, Ask, Explain, Generate Tests) run Claude
  Code: they cost real usage, and they keep running and holding memory after the close. The
  script and tests use the local Refresh Map. Never put them in an automated run.
- **Reading too soon.** The trim runs four frames and 1.5 s after a close; wait 3 s.
- **Comparing RSS alone.** RSS includes shared libraries and the GL driver (about 55 MB here).
  Compare the accounted heap first, then private memory.
- **A memtrace or asan binary** left in `bin/linux/x64` from a previous run.
- **The malloc tracer's own table.** `tools/malloc-trace/preload.so` maps a 288 MB table and fills
  it as it records: it shows as one anonymous mapping growing to hundreds of MB. Leave it out of
  any reading taken under the tracer (it is the bench's memory, not the app's).
- **A binary just built.** Its code pages are dirty in the page cache until written back (about
  7 MB of `Private_Dirty` in the executable's mapping, not anonymous): file-backed and reclaimable,
  not the app's memory. `RssAnon` does not count them; `smaps_rollup`'s `Private_Dirty` does.
- **Screenshots** (`screenshot` over automation). The first one costs a one-time 6.3 MB in Mesa
  llvmpipe (a window-sized RGBA block, 1332x800x4 bytes, and the shader variants LLVM compiles for
  an RGBA8 target; LLVM's C++ allocations show as 4 blocks in `other`); 20 more add 64 KB, then
  nothing. `glFinish()` after deleting the capture texture changes nothing: it is the driver's
  cache. `memory_cycle.py` takes none.
- **Building with `-j 24`** on a busy machine: the linker has been OOM-killed. Use `-j 8`.

## 6. Leaks found so far

Kept so the same mistakes are recognised quickly. Each row is fixed and has a test or a rule.

| Symptom | Cause | Rule |
|---|---|---|
| +620 KB per folder open/close | one ImGui child window per document; caches keyed by document never pruned; find results, layers and git state kept after close | 2, 3, 8 |
| Large repository showed half a folder | depth-first explorer scan with a global budget | (explorer is breadth-first, folders load on expand) |
| Map kept 24 MB after close | `LogicalMapViewModel` had no `workspaceChanged()` | 3 |
| Layers grew a few bytes per cycle | `clear()` and `= {}` kept a string's capacity | 5 |
| 150 KB kept after a large folder | the automation registry kept its peak capacity | 6 |
| 393 KB kept, late answer could land in the next folder | agent dissection not cancelled on close | 4 |
| Private memory 16 MB above start with heap back | the trim ran before textures and late results were freed | 9 |
| Task manager showed no drop at all | freed pages never returned to the OS | 9 |
| 160 cycles on sdk-clean (11 files, map), 2026-11-02, passes: accounted heap 5,227 KB in 1,864 blocks, identical for 138 cycles, then one step of +1 KB and +3 blocks when ImGui created a 17th window (flat for the last 21 cycles); malloc heap pages oscillate (12,180 to 12,350 KB, no trend); the driver's dirty pages crept +76 KB in steps (16,380 to 16,456 KB) inside mappings whose sizes never changed, bounded by roughly 700 KB of room | the trend test in `scripts/memory_cycle.py` (`--cycles 160 --warmup 10 --csv`) |
| About +100 KB private per cycle over the first cycles (investigated, not a leak) | the C++ heap was identical to the byte, and every `malloc` caller oscillated (net -416 B between cycles 3 and 9, traced); the one-way part was the NVIDIA driver writing new pages of its own command buffers: 21 mappings, 16.5 MB, sizes unchanged, two rings already full | section 3, items 4 and 5 |
| 33 MB private kept per closed folder, anon arenas climbing about 1 MB a cycle, heap back to the byte | the IntelliSense index was built on a background thread: its blocks lived in that thread's malloc arena; freed, they merged into the arena's top, which `malloc_trim()` never returns, and glibc's dynamic trim threshold (raised by earlier large frees) kept `free()` from returning it too. Found with `malloc_stats()` under gdb: arenas of 33 MB and 12 MB with 3 KB in use | 9: fixed `mallopt` thresholds in `os::init()`; the index pools its text into a handful of large blocks (mapped on their own) instead of about 190,000 small ones |
| (Prevented, IntelliSense) a scope would have kept bytes after close although its owner freed everything | code running inside `Scope("intellisense")` called into the main view-model (status text, opening a file, recent files): what those allocated outlives the folder and would be counted as IntelliSense's | 8: a scope covers only the subsystem's own state; calls into other owners happen outside it (`IntellisenseViewModel::status()` uses `Scope("other")`) |
| Private memory swinging 1.6 MB from close to close (47.0-48.6 MB) and the malloc heap's dirty pages creeping (+19 KB a cycle) with IntelliSense, although every live byte was the same after each close | churn, not growth: each cycle allocated and freed about 175 MB for IntelliSense in about 4,200 blocks (a new string per file read for the index, the index's pool doubled 54 times then copied, text copies grown line by line), and every index build ran on a new thread, so whichever malloc arena that thread got kept up to the trim threshold (then 1 MB) of its free top. Found with `tools/malloc-trace/churn.c` and a per-mapping diff of the anonymous mappings between closes: all of the swing was in two thread arenas | 10: one 2 MB read buffer per build, the index reserved from the sources' size, exact-size text copies, documents read in place, the ranking buffer reused; builds on one `os::dispatch::Worker` thread; trim threshold 128 KB. IntelliSense churn 35 MB in about 100 blocks a cycle; over 30 cycles private 46.1-46.6 MB and thread arenas 13.23-13.41 MB, every trend flat |
| macOS CI: `enp_ui_tests` failed "Ctrl+click did not go to the definition", Linux and Windows passed (investigated as a possible OS-layer memory difference: it is not memory) | the test pressed `ImGuiMod_Ctrl`; on macOS (`io.ConfigMacOSXBehaviors`) ImGui swaps the keys a backend reports (Cmd arrives as Ctrl, the Ctrl key as Super) and turns Super+left click into a right click, so the editor saw a right click. The app was right: a user's Cmd+click arrives as Ctrl. Reproduced on Linux by switching the mac mapping on | the UI test now runs the Ctrl+click block with both mappings on every platform, pressing Cmd under the mac one |
| macOS: the status bar and `get_state` read 0 MB private, 0 resident, 0 threads; closing a folder trimmed nothing | `os::processStats()` and `os::trimMemory()` had no macOS branch (glibc-only `malloc_trim`/`mallopt`, `/proc` reads) | Mach `task_info(TASK_VM_INFO)` (`phys_footprint`, `resident_size`), `task_threads` (its rights and array given back), `/dev/fd`; `malloc_zone_pressure_relief` |
| The test bench's readings could include its own connections (thread per request, last one kept until the next) and left 4.3 MB of the app uncounted (shared memory) | `AutomationServer` reaped an ended connection only when the next arrived; `privateBytes` on Linux was `RssAnon` only | `automation` scope, ended connections reaped every 200 ms, the scope checked at every reading; `sharedBytes` (`RssShmem`) counted in the status bar and in the script's private reading. 5,100 bench requests (get_state, list_elements, list_commands, screenshots) on an idle app: C++ heap identical to the byte from request 510 to 5,100, `automation` the same at every reading, threads and descriptors constant |
| "The GL driver's memory is not released when the folder closes" (investigated, 2026-11-02: it is) | traced by library with `tools/malloc-trace`, fresh start then three open/close cycles of a 4,563-file tree with Mesa llvmpipe (no GPU in the container): the driver's live blocks were 1,913 KB at the fresh start, 1.7-2.0 MB with the folder open and 1,426 KB after every close. What stays is the driver's fixed cost of drawing the window at all, the same from the first frame: libLLVM's writable data 6,760 KB and libgallium's 1,180 KB (the driver's libraries relocated when they load; given back only if the driver is unloaded), the window's frame buffer 5,060 KB (MIT-SHM, sized by the window), 1.4 MB of context state and compiled shaders. It is given back when the window's GL context is destroyed, at exit; destroying it on close would only rebuild it at the next frame (the window is still drawn), recompiling shaders and re-uploading the font atlas. With a real GPU the frame buffers live in video memory and the driver keeps fixed command rings instead (NVIDIA: 16.5 MB, row above) | not a leak; `memory_cycle.py` fails if the driver's mappings grow |
| `documents` held 148 KB with no document open after a folder closed (the same after every close) | `NotepadViewModel::outline()` cached the active document's outline and region summaries and kept them after the document closed (84 KB for EditorView.cpp); the languages' keyword tables (64 KB, built on first use) were counted in the first document's scope | the outline goes with its document (`releaseOutline()` on close and when none is active; test `closing_a_document_frees_its_outline`); the tables are built under `language-tables` at start-up. `documents` with nothing open: 133 bytes |
| The map view kept the closed folder's map layout (boxes, arrows, labels; 1.9 KB here, more for a large map) until the map view was drawn again | `MapView::layout()` cleared with `clear()` (capacity kept) and only when drawn | `releaseIfMapGone()` every frame, by `built()` (asking `map()` would build a map); UI test "map view layout after the map is gone" |
| `views` read 221 KB after a close, of which 156 KB was not the views' | the local engine's one-time set-up (llama.cpp's log ring and registry) ran inside a draw | `local-engine` scope in `initBackend`; `views` 65 KB |
| 438 KB of the C++ heap had no owner (`other`) | the GL driver's C++ (LLVM) allocations, constructors before `main()`, and the app's start-up objects were unscoped | `gl-driver`, `static-init`, `app` (above); 12 KB left in `other`, named in the table. Every scope after each of three closes, to the byte: 6,191,835 bytes in all |
| The closed folder's path kept as heap storage by the terminal and the code assistant | `setWorkingDirectory(home)` assigned a short path into the string that held the long one (rule 5) | swapped in |
| **Open:** about 1.5 MB/s while idle with a large repo, Windows only | not reproduced on Linux (flat for minutes in every mode); Windows-only code audited without a finding | watch the developer readout on Windows: which counter climbs names it |

## 7. What "Rust-like" means here, and what it does not

RAII plus `unique_ptr` gives Rust's ownership and drop semantics: one owner, freed exactly when
the owner dies, moves instead of copies, no manual frees. What C++ cannot give is the borrow
checker: a reference or pointer can outlive what it points to, and the compiler will not say so.
The defences are rule 4 (`weak_ptr` and generations across threads), AddressSanitizer (it finds a
use-after-free when it happens), and ids rather than pointers for anything that lives across a
frame.

Performance: the accounting adds roughly a nanosecond per allocation. Keep allocations out of
per-frame paths: reuse buffers across frames (`std::string` members, `reserve()`), build labels in
fixed `char` arrays, and let the UI test's steady-state check catch regressions.

The Claude Code skill `.claude/skills/memory-rules/SKILL.md` walks a change through these rules,
the tests and the script before it is committed.
