# Building EasyNotepad

## Build and run

```sh
git clone --recursive https://github.com/chongmx/EasyNotepad.git
scripts/build.sh --test --run     # build, run the tests, launch
```

Binaries go to `bin/<os>/<arch>/`, and Android APKs to `bin/android/`. CMake presets cover
Windows (MSVC x64/ARM64/x86, MinGW x64/x86), Linux (native and aarch64 cross), macOS (universal)
and the web; `android/` is a Gradle project. Releases are made as [RELEASING.md](RELEASING.md)
describes.

## Windows 7 and 32-bit Windows

The Windows build runs on Windows 7 and later, 64- or 32-bit. The built-in terminal needs Windows 10
1809 or later, and the app needs OpenGL 3.0 graphics drivers. For a 32-bit build with MinGW, install
the x86 edition of [w64devkit](https://github.com/skeeto/w64devkit/releases) at `C:\w64devkit-x86`
(or set `W64DEVKIT_X86` to its folder), then:

```sh
cmake --preset windows-mingw-x86 && cmake --build --preset windows-mingw-x86   # bin/windows/x86/
```

## The on-device engine

The on-device engine is llama.cpp, a submodule pinned to one commit (`3rdparty/llama.cpp`). The
build never uses it as checked out. CMake exports that commit into the build folder and applies
`patches/llama.cpp/*.patch` there (see [patches/README.md](../patches/README.md)). The engine is in
64-bit builds only; `-DENP_LOCAL_ENGINE=OFF` leaves it out.

## Running

The agent CLI used for dissection is set with `EASYNOTEPAD_AGENT_CLI` (default `claude -p`); it
gets the prompt on stdin and answers with JSON.

Launch options: `--workspace=agent|code`, `--detail=Full|Balanced|Overview|Outline`,
`--panel=terminal`, `--tour`, `--native-titlebar`, `--no-viewports` (popups and the popped-out
terminal stay inside the main window), `--no-automation`.

## Tracking down a leak

Turn on developer mode (seven taps on the version in About) and the status bar shows what the
process holds (memory, handles, GDI and USER objects, threads, Dear ImGui allocations and windows),
refreshed every second; the automation `get_state` reply carries the same numbers. Whichever count
climbs while the app idles names the culprit. The rules and tools are in [MEMORY.md](MEMORY.md).

## Architecture

MVVM in layers: **os** (files, sockets, pseudo-terminals) → **model** (text, outline, terminal
emulator, diff) → **interfaces / services** → **view-models** → **view** (ImGui). Services are
**command-line based**: they wrap CLI tools (shells, linters, the agent) behind `I*Service`
interfaces. Unit tests and a headless UI test (ImGui ID conflicts and usage errors) run in CI.
