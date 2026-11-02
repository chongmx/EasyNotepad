---
name: memory-rules
description: Enforce EasyNotepad's memory ownership rules (docs/MEMORY.md) on any C++ change, and prove that closing a folder gives the memory back. Use whenever code in src/ or include/ is added or edited that owns state, adds a member container or cache, keeps anything per workspace, per document or per frame, starts background work or a child process, creates Dear ImGui windows or textures, or touches allocation; whenever a leak, growing memory, or "memory stays after close" is reported; and before committing or preparing a patch of such changes.
---

# Memory rules for EasyNotepad

The user judges leaks by the task manager: closing a folder must bring the process back to what it
held when it had just started. `docs/MEMORY.md` is the contract; this skill applies it to a change
and proves the result. Read `docs/MEMORY.md` once per session first, including section 6 (the
leaks already found: most new ones look like one of them).

Work in this order. Do not report a memory change as done until step 4 has passed, and say which
numbers you measured.

## 1. Review the diff against the rules

For every changed file, check each point and fix violations in the change itself.

1. **Ownership.** No `new`, `delete`, `malloc`, `calloc`, `realloc` or `free` outside `src/os/`.
   `std::unique_ptr` for one owner, `std::shared_ptr` only across threads.
2. **Workspace state.** Every member filled from the open folder (trees, maps, results, caches
   keyed by path) is released in its class's `workspaceChanged()`, which `openFolder()` and
   `closeFolder()` call. A new class with such state gets a `workspaceChanged()` and both calls.
3. **Release, do not clear.** Reset with `os::mem::release(x)`. `clear()` and `x = {}` keep the
   capacity (for `std::string` too).
4. **Views cache by id and prune.** A view cache keyed by `doc.id` is pruned when documents close
   (`EditorView::pruneClosed()`). No pointer to a document or node kept across frames.
5. **Background work.** `runAsync` lambdas capture `std::weak_ptr<bool> alive` and a generation,
   and drop late results. `workspaceChanged()` cancels running work about the old folder (agent
   runs, processes) and bumps the generation. Ask: if the folder closes while this runs, what does
   it hold, and where does its answer go?
6. **Per-frame buffers.** Anything filled every frame keeps its busiest frame's capacity: shrink it
   when the content is far smaller (`UiAutomation::endFrame()`). No `std::string`/`std::vector`
   temporaries in the per-frame path unless the content changed.
7. **ImGui.** One child window per role, never per document or item; no id, path or counter in a
   window name. A popup or a scrolling table is a window too: one popup id for a whole list (open it
   with `OpenPopup(id)` from the row), never `BeginPopupContextItem` under a per-row `PushID`; one
   table id per view, reset with `TableResetSettings` when its content changes kind. Textures: one owner, reference counted, flagged for the backend to destroy, freed
   when it reports destroyed (`MarkdownImages`).
8. **Scopes.** New state of more than a few KB is built inside `os::mem::Scope s("name");` with a
   string literal, and the scope is added to the table in `docs/MEMORY.md`.
9. **Large releases outside the close path** set the view-model's trim request
   (`memoryTrimRequested_`) so the memory goes back to the OS.
10. **Measurement code** takes its storage from `os::mem::pool`, never from the heap it measures.

## 2. Extend the tests

- Add the subsystem's exercise to the `cycle` lambda in
  `memory_accounting_counts_scopes_and_folder_cycles_release_everything` (`tests/TestMain.cpp`).
- A view-side cache or texture gets a counter on the view (like `imageTextureCount()`) and a check
  in `tests/UiIdTest.cpp` that it is zero after its documents close.
- Background work about a workspace gets a test like
  `closing_a_folder_stops_the_agent_dissection_of_it`, with a fake service whose answer arrives
  after the close.

## 3. Build and run the tests

```bash
cmake --build build/linux --target enp_tests enp_ui_tests EasyNotepad -j 8
bin/linux/x64/enp_tests | grep -E "FAIL|checks|scope "
(cd bin/linux/x64 && ./enp_ui_tests | tail -1)   # takes minutes; must run from its folder
```

A failing scope line names the subsystem. Never loosen a test or add a tolerance to make it pass:
find the holder (step 5) and release it.

## 4. Measure the real app

The unit test only sees growth per cycle in the view-models. First-cycle growth, view caches,
textures, late frees and anything outside the C++ heap only show in the running app:

```bash
DISPLAY=:1 python3 scripts/memory_cycle.py --app bin/linux/x64/EasyNotepad \
    --folder ~/code/sdk-clean --files README.md docs/userguide/camera.md mcp_image.png lib/libRealSensePlugin.so
```

Use a large folder and include every kind of file the change touches. The script runs eight
cycles, the first three a warm-up. It fails unless, after every close, the heap is at or below the
fresh start and identical across cycles, and after the warm-up nothing climbs: not the malloc
heap, the thread arenas, the GPU driver's mapping sizes, handles, GDI/USER objects or threads.
Oscillation is fine; growth in one direction is not, however small. Report its table to the user.
When a reading moves, run ten or more cycles before drawing a conclusion: early cycles settle.

Never trigger the agent CLI in a measurement or test (Dissect Project, Ask, Explain, Generate
Tests): it runs Claude Code on the user's account and keeps running after the close. Use Refresh
Map for the map.

## 5. When something stays: find the holder

1. **Scope:** the script prints scopes above the fresh start. `other` growing means a missing
   scope: add one and measure again.
2. **Line:** `cmake --preset linux-memtrace && cmake --build build/linux-memtrace --target EasyNotepad -j 8`,
   start the app with `ENP_MEM_TRACE=<scope>` and `EASYNOTEPAD_ENDPOINT=<file>`, send automation
   `memory_trace {"action":"mark"}`, reproduce, then `memory_trace {"action":"dump","file":...}`,
   and symbolize the `EasyNotepad(+0x...)` frames with `addr2line -e bin/linux/x64/EasyNotepad -f -C -i`.
   Rebuild the default `linux` preset afterwards: both write `bin/linux/x64`.
3. **Outside the heap** (heap back, private not): diff `/proc/<pid>/smaps` by mapping, and run the
   app as gdb's child (attaching is blocked when `ptrace_scope` is 1) to `call (void)malloc_stats()`
   and `call (int)malloc_trim(0)`. A large release from a second trim means the app's trim ran
   before the free.
4. **Memory that does not go through `operator new`** (C libraries, X11, the GL driver): use
   `tools/malloc-trace` (docs/MEMORY.md section 3, item 4). Take dumps several cycles apart and
   compare call sites; only a site that stays positive over several pairs is growing. Do not
   blame the driver without this evidence; do not call driver growth a leak while its mappings
   keep their sizes.
5. Typical causes, most likely first: kept capacity (rules 3 and 6), a member missing from
   `workspaceChanged()`, an unpruned view cache, work still running after the close, a trim that
   ran too early, a function-local `static` built inside a scope (harmless: first-use tables).

## 6. Before committing or making a patch

- `enp_tests` and `enp_ui_tests`: 0 failures. `scripts/memory_cycle.py`: OK on a large folder.
- For ownership changes, also once: `cmake --preset linux-asan && cmake --build --preset linux-asan -j 8`
  and run the tests from it.
- If a new leak was found and fixed, add a row to `docs/MEMORY.md` section 6.
- In the commit message or patch note: what the change owns, where it is released, which scope
  accounts for it, and the script's after-close numbers.
