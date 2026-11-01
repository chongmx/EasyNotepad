# 32-bit Windows (x86) with MinGW-w64's i686 GCC, e.g. w64devkit's x86 edition:
#   https://github.com/skeeto/w64devkit/releases  (w64devkit-x86-<version>.7z.exe)
# The compiler is looked for in $W64DEVKIT_X86/bin, then C:/w64devkit-x86/bin, then on PATH.
# The windows-mingw-x86 preset uses this file; the output goes to bin/windows/x86/.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86)

set(_enp_x86_hints "$ENV{W64DEVKIT_X86}/bin" "C:/w64devkit-x86/bin")
# w64devkit's i686-w64-mingw32-* names are launchers that need the kit first on PATH: use the
# plain drivers from the kit's own folder.
find_program(ENP_X86_GCC NAMES gcc HINTS ${_enp_x86_hints} NO_DEFAULT_PATH)
find_program(ENP_X86_GCC NAMES i686-w64-mingw32-gcc REQUIRED)
cmake_path(GET ENP_X86_GCC PARENT_PATH _enp_x86_bin)
find_program(ENP_X86_GXX NAMES g++ i686-w64-mingw32-g++ HINTS "${_enp_x86_bin}" NO_DEFAULT_PATH REQUIRED)
find_program(ENP_X86_WINDRES NAMES windres i686-w64-mingw32-windres HINTS "${_enp_x86_bin}" NO_DEFAULT_PATH)

set(CMAKE_C_COMPILER "${ENP_X86_GCC}")
set(CMAKE_CXX_COMPILER "${ENP_X86_GXX}")
if(ENP_X86_WINDRES)
    set(CMAKE_RC_COMPILER "${ENP_X86_WINDRES}")
endif()
# GCC looks for its assembler and linker on PATH, where a 64-bit kit may come first: -B points it
# at this kit's own.
set(CMAKE_C_FLAGS_INIT "-B${_enp_x86_bin}/")
set(CMAKE_CXX_FLAGS_INIT "-B${_enp_x86_bin}/")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-B${_enp_x86_bin}/")
