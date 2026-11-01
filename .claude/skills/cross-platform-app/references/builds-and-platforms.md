# Builds and platforms

## Layout of the build

- One `CMakeLists.txt` for every target; Android's Gradle project points `externalNativeBuild` at it
  and shares `resources/` as assets.
- Presets (`CMakePresets.json`): `windows-x64`, `windows-arm64`, `windows-x86` (MSVC, `-A Win32`),
  `windows-mingw`, `windows-mingw-x86` (toolchain file), `linux`, `linux-arm64-cross`,
  `macos-universal` (`CMAKE_OSX_ARCHITECTURES=arm64;x86_64`), `web` (Emscripten).
- Outputs to `bin/<os>/<arch>/` with `resources/` next to the binaries. Name the arch from
  `CMAKE_SYSTEM_PROCESSOR` / the MSVC architecture id (`x64`, `x86`, `arm64`, `arm`, `wasm32`).
- Anything under `resources/` ships to users (and into the APK) — keep build-only files (patches,
  scripts) elsewhere or exclude them.

## Oldest Windows

- `add_compile_definitions(_WIN32_WINNT=0x0601)` for all Windows code: only Windows 7 APIs are
  declared, so nothing newer is linked by accident. (Do not also force `WINVER` if a dependency such
  as GLFW sets its own — that triggers redefinition warnings.)
- Newer functions by name at run time, with a fallback:

```cpp
template <class Fn> Fn user32Function(const char* name)
{
    return reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), name)));
}
UINT windowDpi(HWND hwnd)
{
    static const auto getDpiForWindow = user32Function<UINT(WINAPI*)(HWND)>("GetDpiForWindow"); // 10 1607+
    if (getDpiForWindow) return getDpiForWindow(hwnd);
    UINT dpi = 96;
    if (HDC dc = GetDC(hwnd)) { dpi = UINT(GetDeviceCaps(dc, LOGPIXELSX)); ReleaseDC(hwnd, dc); }
    return dpi ? dpi : 96;
}
```

- Verify: `objdump -p app.exe` lists every imported function; none may be newer than the target
  (watch for `*ForDpi`, `GetSystemTimePreciseAsFileTime`, `CreateFile2`, `WaitOnAddress`,
  `SetThreadDescription`, `CreatePseudoConsole`). A missing import means the program does not start
  at all ("entry point not found").
- Features that need newer Windows degrade instead (e.g. a built-in terminal needs ConPTY, Windows 10
  1809+). Tell users in the README. OpenGL 3 drivers are another floor on old/virtual machines.

## 32-bit with MinGW

- A 64-bit-only GCC (no multilib) fails at link time with `-m32` ("skipping incompatible
  libmingw32.a"). Use an i686 toolchain, e.g. w64devkit's x86 edition (same version as the 64-bit
  one, unpacked side by side).
- Its prefixed names (`i686-w64-mingw32-gcc`) may be launchers that need its `bin` first on PATH;
  use the plain `gcc`/`g++` from that folder. If another toolchain is first on PATH, GCC picks up the
  wrong assembler ("invalid instruction suffix for push"): add `-B<kit>/bin/` to C, C++ and linker
  flags.

```cmake
# cmake/toolchains/mingw-x86.cmake
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86)
set(_hints "$ENV{W64DEVKIT_X86}/bin" "C:/w64devkit-x86/bin")
find_program(X86_GCC NAMES gcc HINTS ${_hints} NO_DEFAULT_PATH)
find_program(X86_GCC NAMES i686-w64-mingw32-gcc REQUIRED)
cmake_path(GET X86_GCC PARENT_PATH _bin)
find_program(X86_GXX NAMES g++ i686-w64-mingw32-g++ HINTS "${_bin}" NO_DEFAULT_PATH REQUIRED)
set(CMAKE_C_COMPILER "${X86_GCC}")
set(CMAKE_CXX_COMPILER "${X86_GXX}")
set(CMAKE_C_FLAGS_INIT "-B${_bin}/")
set(CMAKE_CXX_FLAGS_INIT "-B${_bin}/")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-B${_bin}/")
```

- Run the full test suite on the 32-bit build too; it catches size/pointer assumptions.

## Dependencies

- Submodules: the recorded commit is the lock. Remove `branch =` from `.gitmodules` (it lets
  `git submodule update --remote` move silently) and beware `ignore = dirty` hiding edits.
- Local changes to a dependency: numbered patch files + a `BASE` file with the exact commit; CMake
  checks the checkout matches, copies the sources into the build directory and `git apply`s the
  patches there (works outside a repository). Fail configure with a clear message otherwise.

## CI

- A matrix job per preset that can run on hosted runners (Windows x64/x86/ARM64, Linux x64/ARM64,
  macOS), plus web and Android jobs; upload `bin/` as artifacts.
- Keep actions on current majors; GitHub deprecates Node 20 actions — e.g. `actions/checkout@v7`,
  `actions/upload-artifact@v7`, `actions/setup-java@v6`, `gradle/actions/setup-gradle@v6`,
  `mymindstorm/setup-emsdk@v15` all run on Node 24 (check each `action.yml` `runs.using`).
- Tests that need a real terminal or GUI-only tools may need skipping per platform (e.g. a test that
  runs `top` on macOS).

## Line endings and scripted edits

With `core.autocrlf=true`, git may rewrite working files to CRLF after a merge/checkout. Scripts that
patch source text must detect the file's ending and convert their patterns, or they silently stop
matching.

## Processes

See SKILL.md §7. Windows: `CreateProcess(... CREATE_SUSPENDED ...)`, `AssignProcessToJobObject`
with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, `ResumeThread`, then wait in 100 ms slices checking a
cancel flag and the deadline, `TerminateJobObject` to stop. POSIX: `setpgid(0, 0)` in the child,
poll the pipes with a timeout, `kill(-pid, SIGKILL)` to stop the group.
