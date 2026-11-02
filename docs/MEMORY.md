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
   `GitViewModel`, find-in-files). A new subsystem that keeps per-workspace state adds a
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
   set trim) twice: four frames later, once the textures released by the close are destroyed, and
   1.5 s later, once late results of background work are freed too. The app keeps drawing until
   both have run. A change that frees something large elsewhere sets the request too.

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
| `markdown` | rendered Markdown and parsed formulas |
| `images` | decoding (the pixels themselves are in `imgui`) |
| `emoji` | the emoji pack's index and reading pictures from it (`EmojiCache`, `services::EmojiPack`) |
| `data-views` | a CSV table or JSON/YAML tree while a view shows it (`DataViews`): freed on close or back to text |
| `library` | what a library or program contains while its view shows it (`LibraryView`, `model::bin::Image`): freed on close or when opened as text |
| `views` | everything else the views allocate (`Application::frame`) |
| `imgui` | Dear ImGui: windows, draw buffers, font atlas, textures |
| `other` | not attributed: startup, services, threads |

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
5. **Telling a climb from settling.** Run at least ten cycles and look at the after-close readings
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

Things that make measurements wrong:

- **Commands that start the agent CLI** (Dissect Project, Ask, Explain, Generate Tests) run Claude
  Code: they cost real usage, and they keep running and holding memory after the close. The
  script and tests use the local Refresh Map. Never put them in an automated run.
- **Reading too soon.** The trim runs four frames and 1.5 s after a close; wait 3 s.
- **Comparing RSS alone.** RSS includes shared libraries and the GL driver (about 55 MB here).
  Compare the accounted heap first, then private memory.
- **A memtrace or asan binary** left in `bin/linux/x64` from a previous run.
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
