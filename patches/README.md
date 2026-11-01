# Patches to third-party code

The submodules under `3rdparty/` are never edited. When the app needs a change in one, the change is
a patch file here.

At configure time, CMake does the following (`cmake/PatchedSource.cmake`):
1. Checks that the submodule is at the commit named in `<name>/BASE`. Otherwise configuring fails
   and says which commit to check out.
2. Exports that commit with `git archive` into the build folder, as `build/<preset>/<name>-src`.
   Local edits and line endings in the checkout do not matter.
3. Applies `<name>/*.patch` there with `git apply`, in name order.

This is redone only when `BASE` or a patch changes. These files are build-only: nothing here ships
(unlike `resources/`).

## llama.cpp (`BASE`: b12030, `a91eab6`)

| Patch | Why |
|---|---|
| `0001-common-optional-downloader.patch` | Adds the `LLAMA_COMMON_DOWNLOAD` option. When it is off, the argument parser, the model downloader and cpp-httplib are left out. cpp-httplib needs Windows 10, the app downloads models itself, and the Windows build must still start on Windows 7. |
| `0002-build-info-from-embedder.patch` | ggml and common take the build number and commit from the embedding project. The exported copy has no `.git`, so asking git would report this repository's commit instead. |

## Upgrading

1. Check out the new commit in the submodule.
2. Put its hash (and tag) in `BASE`.
3. Re-make each patch against it: edit a clean checkout, then `git diff > NNNN-name.patch`.
4. Build and run the tests. Also check the Windows import table for anything newer than Windows 7
   (`objdump -p bin/windows/x64/EasyNotepad.exe`).
