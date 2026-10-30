#!/usr/bin/env bash
# Build EasyNotepad.
#
#   scripts/build.sh [options] [target]
#
# Targets:
#   native        Host platform (default): Linux, macOS, or Windows from Git Bash / MSYS2
#   linux-arm64   Cross-compile for Linux aarch64 (needs g++-aarch64-linux-gnu + arm64 dev libs)
#   web           WebAssembly via Emscripten (needs an activated emsdk: emcmake on PATH)
#   android       APK for arm64-v8a and x86_64 via Gradle (needs gradle + Android SDK/NDK)
#
# Options:
#   -c, --config <Debug|Release>   Build type (default: Release)
#   -j, --jobs <N>                 Parallel jobs (default: CPU count)
#   -t, --test                     Run the unit tests after building (native only)
#   -r, --run                      Launch the app after building (native only)
#       --clean                    Delete the build directory first
#       --universal                macOS: build a universal arm64 + x86_64 binary
#   -b, --build-dir <dir>          Parent directory for build trees (default: build/)
#   -h, --help                     Show this help
#
# Build output goes to <build-dir>/<host-or-target>-<config>/, e.g. build/linux-release,
# build/windows-debug, build/web-release (Android: android/app/build/outputs/apk/).
# Under WSL with the checkout on a Windows drive (/mnt/c/...), the default build dir is
# ~/.cache/easynotepad-build, because building on /mnt is slow and Windows file locks break it.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

TARGET="native"
CONFIG="Release"
JOBS=""
RUN_TESTS=0
RUN_APP=0
CLEAN=0
UNIVERSAL=0
BUILD_ROOT=""

usage() { awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"; }
die() { echo "error: $*" >&2; exit 1; }
need() { command -v "$1" > /dev/null 2>&1 || die "'$1' not found. $2"; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        -c|--config) CONFIG="${2:?missing value for $1}"; shift 2 ;;
        -j|--jobs) JOBS="${2:?missing value for $1}"; shift 2 ;;
        -t|--test) RUN_TESTS=1; shift ;;
        -r|--run) RUN_APP=1; shift ;;
        --clean) CLEAN=1; shift ;;
        --universal) UNIVERSAL=1; shift ;;
        -b|--build-dir) BUILD_ROOT="${2:?missing value for $1}"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        -*) die "unknown option '$1' (see --help)" ;;
        *) TARGET="$1"; shift ;;
    esac
done

case "$CONFIG" in
    Debug|Release|RelWithDebInfo|MinSizeRel) ;;
    debug) CONFIG=Debug ;;
    release) CONFIG=Release ;;
    *) die "unknown config '$CONFIG'" ;;
esac

if [[ -z "$JOBS" ]]; then
    JOBS="$(nproc 2> /dev/null || sysctl -n hw.ncpu 2> /dev/null || echo 4)"
fi

# Submodules must be present.
if [[ ! -f "$ROOT/3rdparty/imgui/imgui.h" || ! -f "$ROOT/3rdparty/IconFontCppHeaders/IconsFontAwesome6.h" ]]; then
    echo "Fetching git submodules..."
    git -C "$ROOT" submodule update --init --recursive
fi

case "$(uname -s)" in
    Linux*) HOST=linux ;;
    Darwin*) HOST=macos ;;
    MINGW*|MSYS*|CYGWIN*) HOST=windows ;;
    *) HOST=unknown ;;
esac

# Prefer Ninja when installed; otherwise CMake's default generator for the host.
GENERATOR_ARGS=()
if command -v ninja > /dev/null 2>&1; then
    GENERATOR_ARGS=(-G Ninja)
elif [[ "$HOST" == "windows" ]]; then
    GENERATOR_ARGS=(-G "MinGW Makefiles")
fi

# Native builds are named after the host so Windows and WSL can share one checkout.
BUILD_NAME="$TARGET"
[[ "$TARGET" == "native" ]] && BUILD_NAME="$HOST"
BUILD_NAME="${BUILD_NAME}-$(echo "$CONFIG" | tr '[:upper:]' '[:lower:]')"
if [[ -z "$BUILD_ROOT" ]]; then
    BUILD_ROOT="$ROOT/build"
    # WSL with the checkout on a Windows drive: building on /mnt/* is slow and fails when Windows
    # (e.g. Defender) locks freshly extracted files, so use the Linux file system instead.
    if [[ "$HOST" == "linux" && "$ROOT" == /mnt/* ]] && grep -qi microsoft /proc/version 2> /dev/null; then
        BUILD_ROOT="$HOME/.cache/easynotepad-build"
    fi
fi
BUILD_DIR="$BUILD_ROOT/$BUILD_NAME"

configure_and_build() {
    if [[ "$CLEAN" == 1 ]]; then
        echo "Removing $BUILD_DIR"
        rm -rf "$BUILD_DIR"
    fi
    echo "==> Configuring $TARGET ($CONFIG) in $BUILD_DIR"
    # ${a[@]+"${a[@]}"} keeps empty arrays safe under `set -u` on bash 3.2 (macOS).
    "$@" -S "$ROOT" -B "$BUILD_DIR" ${GENERATOR_ARGS[@]+"${GENERATOR_ARGS[@]}"} -DCMAKE_BUILD_TYPE="$CONFIG" \
        ${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}
    echo "==> Building with $JOBS jobs"
    cmake --build "$BUILD_DIR" --config "$CONFIG" --parallel "$JOBS"
}

EXTRA_ARGS=()
case "$TARGET" in
    native)
        need cmake "Install CMake 3.22 or newer."
        [[ "$HOST" == "unknown" ]] && die "unsupported host '$(uname -s)'"
        if [[ "$HOST" == "macos" && "$UNIVERSAL" == 1 ]]; then
            EXTRA_ARGS+=("-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64")
        fi
        configure_and_build cmake
        ;;

    linux-arm64)
        [[ "$HOST" == "linux" ]] || die "linux-arm64 cross-compilation requires a Linux (or WSL) host"
        need cmake "Install CMake 3.22 or newer."
        need aarch64-linux-gnu-g++ "Install the cross compiler: sudo apt install g++-aarch64-linux-gnu (see cmake/toolchains/aarch64-linux-gnu.cmake)"
        EXTRA_ARGS+=(-DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/toolchains/aarch64-linux-gnu.cmake"
                     -DENP_BUILD_TESTS=OFF -DGLFW_BUILD_WAYLAND=OFF)
        configure_and_build cmake
        ;;

    web)
        need emcmake "Install and activate emsdk: source <emsdk>/emsdk_env.sh"
        need cmake "Install CMake 3.22 or newer."
        configure_and_build emcmake cmake
        echo "==> Serve it with: (cd \"$BUILD_DIR\" && python3 -m http.server 8080) then open http://localhost:8080"
        ;;

    android)
        need gradle "Install Gradle 8.9+ or open the android/ folder in Android Studio."
        [[ -n "${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}" ]] || die "set ANDROID_HOME to your Android SDK location"
        task="assemble${CONFIG}"
        [[ "$CONFIG" == "Release" || "$CONFIG" == "Debug" ]] || task="assembleRelease"
        if [[ "$CLEAN" == 1 ]]; then
            (cd "$ROOT/android" && gradle clean)
        fi
        echo "==> gradle $task"
        (cd "$ROOT/android" && gradle "$task")
        echo "==> APKs:"
        ls -1 "$ROOT"/android/app/build/outputs/apk/*/*.apk
        exit 0
        ;;

    *)
        die "unknown target '$TARGET' (see --help)"
        ;;
esac

EXE="$BUILD_DIR/EasyNotepad"
[[ "$HOST" == "windows" && "$TARGET" == "native" ]] && EXE="$EXE.exe"
[[ "$TARGET" == "web" ]] && EXE="$BUILD_DIR/index.html"
echo "==> Built: $EXE"

if [[ "$RUN_TESTS" == 1 ]]; then
    [[ "$TARGET" == "native" ]] || die "--test is only available for the native target"
    ctest --test-dir "$BUILD_DIR" -C "$CONFIG" --output-on-failure
fi

if [[ "$RUN_APP" == 1 ]]; then
    [[ "$TARGET" == "native" ]] || die "--run is only available for the native target"
    "$EXE"
fi
