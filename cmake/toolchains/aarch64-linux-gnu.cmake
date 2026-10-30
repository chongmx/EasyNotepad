# Cross-compile for Linux aarch64 from an x86_64 host (Debian/Ubuntu multiarch).
#   sudo dpkg --add-architecture arm64   (Ubuntu also needs a ports.ubuntu.com source for arm64)
#   sudo apt install g++-aarch64-linux-gnu libx11-dev:arm64 libxrandr-dev:arm64 libxinerama-dev:arm64 \
#                    libxcursor-dev:arm64 libxi-dev:arm64 libgl-dev:arm64
# The linux-arm64-cross preset builds GLFW with X11 only (GLFW_BUILD_WAYLAND=OFF).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

set(CMAKE_LIBRARY_ARCHITECTURE aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu /usr/lib/aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(ENV{PKG_CONFIG_LIBDIR} /usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig)
