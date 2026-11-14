cmake_minimum_required(VERSION 3.22)

# The version lives in one place, the VERSION file (docs/RELEASING.md); the Android build reads it too.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" ENP_VERSION_FILE LIMIT_COUNT 1)
string(STRIP "${ENP_VERSION_FILE}" ENP_VERSION_FILE)
if(NOT ENP_VERSION_FILE MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "VERSION must be MAJOR.MINOR.PATCH, not '${ENP_VERSION_FILE}'")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")

project(EasyNotepad VERSION ${ENP_VERSION_FILE} LANGUAGES C CXX)

# A release build is one built from its tag (cmake/Version.cmake); CI passes the tag it builds.
set(ENP_RELEASE_TAG "" CACHE STRING "The tag being built for a release (vMAJOR.MINOR.PATCH); empty otherwise")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---- Target platform ----------------------------------------------------------------------
if(EMSCRIPTEN)
    set(ENP_PLATFORM web)
elseif(ANDROID)
    set(ENP_PLATFORM android)
elseif(WIN32)
    set(ENP_PLATFORM windows)
    # Windows 7 and later: the headers declare nothing newer, so nothing newer is linked by accident
    # (newer functions, such as per-monitor DPI and the pseudo console, are looked up at run time).
    add_compile_definitions(_WIN32_WINNT=0x0601)
elseif(APPLE)
    set(ENP_PLATFORM macos)
else()
    set(ENP_PLATFORM linux)
endif()

# Target architecture name used for the output folder: x64, arm64, x86, arm, wasm32 or universal.
if(EMSCRIPTEN)
    set(ENP_ARCH wasm32)
elseif(APPLE AND CMAKE_OSX_ARCHITECTURES)
    list(LENGTH CMAKE_OSX_ARCHITECTURES _enp_arch_count)
    if(_enp_arch_count GREATER 1)
        set(ENP_ARCH universal)
    else()
        set(ENP_ARCH ${CMAKE_OSX_ARCHITECTURES})
    endif()
elseif(ANDROID)
    set(ENP_ARCH ${ANDROID_ABI})
elseif(MSVC AND CMAKE_CXX_COMPILER_ARCHITECTURE_ID) # the target, also when cross-compiling with -A ARM64
    set(ENP_ARCH ${CMAKE_CXX_COMPILER_ARCHITECTURE_ID})
else()
    set(ENP_ARCH ${CMAKE_SYSTEM_PROCESSOR})
endif()
string(TOLOWER "${ENP_ARCH}" ENP_ARCH)
if(ENP_ARCH MATCHES "^(x86_64|amd64|x64)$")
    set(ENP_ARCH x64)
elseif(ENP_ARCH MATCHES "^(aarch64|arm64|arm64-v8a)$")
    set(ENP_ARCH arm64)
elseif(ENP_ARCH MATCHES "^(i.86|x86)$")
    set(ENP_ARCH x86)
elseif(ENP_ARCH MATCHES "^(armv7.*|arm|armeabi.*)$")
    set(ENP_ARCH arm)
endif()
message(STATUS "EasyNotepad: platform=${ENP_PLATFORM} arch=${ENP_ARCH}")

# All executables (app, tests, web bundle) go to bin/<os>/<arch>/, with resources/ next to them.
# Android is excluded: Gradle decides where the native library goes and packages it into the APK.
set(ENP_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/bin/${ENP_PLATFORM}/${ENP_ARCH}" CACHE PATH "Output directory for binaries")
if(NOT ANDROID)
    # The no-op generator expression stops multi-config generators (Visual Studio) adding a Debug/Release subfolder.
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${ENP_OUTPUT_DIR}$<0:>")
endif()

set(ENP_IS_DESKTOP OFF)
if(ENP_PLATFORM MATCHES "windows|macos|linux")
    set(ENP_IS_DESKTOP ON)
endif()

option(ENP_BUILD_TESTS "Build the unit tests (desktop only)" ${ENP_IS_DESKTOP})
option(ENP_FETCH_GLFW "Download GLFW if it is not installed (desktop)" ON)
option(ENP_MEMORY_ACCOUNTING "Count every heap allocation by subsystem (developer readout, leak tests)" ON)
option(ENP_MEMORY_TRACE "Compile the allocation call-stack trace (services/debug; Linux developer builds)" OFF)
option(ENP_APPLE_MALLOC_PRESSURE_RELIEF "Call Apple's best-effort malloc pressure-relief API when trimming" ON)
option(ENP_APPLE_LARGE_ALLOCATION_MMAP "Use mmap/munmap for large C++ allocations in Apple heap-accounting builds" ON)
option(ENP_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer (GCC/Clang)" OFF)
if(ENP_SANITIZE AND NOT MSVC)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()
set(ENP_WEB_GLFW "contrib" CACHE STRING "Emscripten GLFW implementation: 'contrib' (--use-port=contrib.glfw3) or 'builtin' (-sUSE_GLFW=3)")

set(ENP_THIRD_PARTY ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty)
if(NOT EXISTS ${ENP_THIRD_PARTY}/imgui/imgui.h OR NOT EXISTS ${ENP_THIRD_PARTY}/IconFontCppHeaders/IconsFontAwesome6.h)
    message(FATAL_ERROR "Submodules missing. Run: git submodule update --init --recursive")
endif()

if(MSVC)
    add_compile_options(/utf-8)
endif()

# Warnings for EasyNotepad's own targets (third-party code is built with its defaults).
function(enp_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /wd4100 /wd4996)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endif()
endfunction()

if(ENP_PLATFORM STREQUAL "web")
    if(ENP_WEB_GLFW STREQUAL "contrib")
        set(ENP_WEB_GLFW_FLAG "--use-port=contrib.glfw3")
    else()
        set(ENP_WEB_GLFW_FLAG "-sUSE_GLFW=3")
    endif()
endif()

# ---- Dear ImGui ---------------------------------------------------------------------------
set(IMGUI_DIR ${ENP_THIRD_PARTY}/imgui)
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
# 32-bit draw indices: OpenGL ES and WebGL (Android, the web) cannot offset vertices, so with
# 16-bit indices a window's draw list may hold at most 65535 of them: a PDF zoomed far out on a
# phone (six pages of dense text) holds 86000, and ImGui asserts (a crash) or draws garbage.
# ES 3 and WebGL 2 draw 32-bit indices natively; the same everywhere, so every build and test uses it.
target_compile_definitions(imgui PUBLIC "ImDrawIdx=unsigned int")
if(NOT MSVC)
    target_compile_options(imgui PRIVATE -w)
endif()

# Font Awesome codepoint headers (header only).
add_library(icon_font_headers INTERFACE)
target_include_directories(icon_font_headers INTERFACE ${ENP_THIRD_PARTY}/IconFontCppHeaders)

# LLVM's demangler (C++ names of GCC/Clang and MSVC, Rust, D), vendored from llvm-project
# llvmorg-19.1.7 (llvm/lib/Demangle); it needs nothing else from LLVM.
add_library(llvm_demangle STATIC
    ${ENP_THIRD_PARTY}/llvm-demangle/lib/Demangle.cpp
    ${ENP_THIRD_PARTY}/llvm-demangle/lib/ItaniumDemangle.cpp
    ${ENP_THIRD_PARTY}/llvm-demangle/lib/MicrosoftDemangle.cpp
    ${ENP_THIRD_PARTY}/llvm-demangle/lib/MicrosoftDemangleNodes.cpp
    ${ENP_THIRD_PARTY}/llvm-demangle/lib/RustDemangle.cpp
    ${ENP_THIRD_PARTY}/llvm-demangle/lib/DLangDemangle.cpp
)
target_include_directories(llvm_demangle PUBLIC ${ENP_THIRD_PARTY}/llvm-demangle/include)
if(NOT MSVC)
    target_compile_options(llvm_demangle PRIVATE -w)
endif()

# ---- On-device inference engine (llama.cpp, plan 23) ----------------------------------------
# 64-bit desktop and Android only: the model's 3 GB mapping does not fit a 32-bit address space,
# and the web build has no engine yet (WebGPU is a later rung). Also off where ggml cannot build
# for a fixed CPU level: MSVC for ARM64 (it wants clang), and macOS universal binaries (two
# architectures in one pass).
set(_enp_engine_default OFF)
if(NOT EMSCRIPTEN AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT (MSVC AND ENP_ARCH STREQUAL "arm64")
   AND NOT ENP_ARCH STREQUAL "universal")
    set(_enp_engine_default ON)
endif()
option(ENP_LOCAL_ENGINE "Build the on-device inference engine (llama.cpp)" ${_enp_engine_default})
if(ENP_LOCAL_ENGINE)
    if(NOT EXISTS ${ENP_THIRD_PARTY}/llama.cpp/CMakeLists.txt)
        message(FATAL_ERROR "Submodule 3rdparty/llama.cpp missing. Run: git submodule update --init --recursive")
    endif()
    # The submodule stays as checked out; the build uses a patched copy (patches/llama.cpp).
    include(cmake/PatchedSource.cmake)
    enp_patched_source(llama.cpp SOURCE ${ENP_THIRD_PARTY}/llama.cpp PATCHES ${CMAKE_CURRENT_SOURCE_DIR}/patches/llama.cpp
                       OUT ENP_LLAMA_DIR PATHS CMakeLists.txt LICENSE cmake common ggml include licenses src vendor)
    string(REGEX REPLACE "^b" "" LLAMA_BUILD_NUMBER "${ENP_LLAMA_DIR_TAG}")
    set(LLAMA_BUILD_COMMIT ${ENP_LLAMA_DIR_COMMIT})
    # Static libraries linked into the app; only the library and its common helpers are built.
    set(BUILD_SHARED_LIBS OFF)
    foreach(_opt LLAMA_BUILD_TESTS LLAMA_BUILD_TOOLS LLAMA_BUILD_EXAMPLES LLAMA_BUILD_SERVER LLAMA_BUILD_APP
                 LLAMA_BUILD_UI LLAMA_OPENSSL LLAMA_SUBPROCESS LLAMA_ALL_WARNINGS GGML_OPENMP GGML_NATIVE)
        set(${_opt} OFF CACHE BOOL "" FORCE)
    endforeach()
    set(LLAMA_BUILD_COMMON ON CACHE BOOL "" FORCE) # speculative decoding lives there
    # Without the downloader (cpp-httplib needs Windows 10): the app fetches models itself.
    set(LLAMA_COMMON_DOWNLOAD OFF CACHE BOOL "" FORCE)
    # One fixed CPU level, not the run-time variants (those need dynamically loaded backends, which
    # Android packaging does not handle yet). The engine checks the CPU has it before running.
    if(ENP_ARCH STREQUAL "x64")
        foreach(_opt GGML_AVX GGML_AVX2 GGML_FMA GGML_F16C GGML_BMI2)
            set(${_opt} ON CACHE BOOL "" FORCE)
        endforeach()
    elseif(ANDROID AND ENP_ARCH STREQUAL "arm64")
        set(GGML_CPU_ARM_ARCH "armv8.2-a+dotprod+fp16" CACHE STRING "" FORCE)
    endif()
    add_subdirectory(${ENP_LLAMA_DIR} ${CMAKE_BINARY_DIR}/llama.cpp EXCLUDE_FROM_ALL)
    # The engine is always optimized, also in Debug builds of the app (the Android debug APK is
    # what gets installed for testing; unoptimized inference is many times slower).
    if(NOT MSVC)
        foreach(_t ggml-base ggml-cpu ggml llama llama-common llama-common-base)
            if(TARGET ${_t})
                target_compile_options(${_t} PRIVATE $<$<CONFIG:Debug>:-O2>)
            endif()
        endforeach()
    endif()
endif()

# ---- Windowing / GL per platform -----------------------------------------------------------
if(ENP_IS_DESKTOP)
    # 3.4+: the platform API (X11 vs Wayland) the custom title bar relies on; distros often ship 3.3.
    find_package(glfw3 3.4 CONFIG QUIET)
    if(NOT glfw3_FOUND)
        if(NOT ENP_FETCH_GLFW)
            message(FATAL_ERROR "GLFW not found; install it or enable ENP_FETCH_GLFW")
        endif()
        include(FetchContent)
        set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(glfw
            GIT_REPOSITORY https://github.com/glfw/glfw.git
            GIT_TAG 3.4
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(glfw)
    endif()

    add_library(glad STATIC ${ENP_THIRD_PARTY}/glad/src/glad.c)
    target_include_directories(glad PUBLIC ${ENP_THIRD_PARTY}/glad/include)
    if(UNIX)
        target_link_libraries(glad PUBLIC ${CMAKE_DL_LIBS})
    endif()

    find_package(OpenGL REQUIRED)
    target_sources(imgui PRIVATE ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp)
    target_link_libraries(imgui PUBLIC glfw OpenGL::GL)
elseif(ENP_PLATFORM STREQUAL "web")
    target_sources(imgui PRIVATE ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp)
    target_compile_options(imgui PUBLIC ${ENP_WEB_GLFW_FLAG})
elseif(ENP_PLATFORM STREQUAL "android")
    target_sources(imgui PRIVATE ${IMGUI_DIR}/backends/imgui_impl_android.cpp)
    target_compile_definitions(imgui PUBLIC IMGUI_IMPL_OPENGL_ES3)
    target_link_libraries(imgui PUBLIC android EGL GLESv3 log)
endif()

# ---- OS abstraction layer --------------------------------------------------------------------
set(ENP_OS_SOURCES
    src/os/common/Process.cpp
    src/os/common/Socket.cpp
    src/os/common/OsCommon.cpp
    src/os/common/Memory.cpp
    src/os/common/Dispatch.cpp
    src/os/common/Path.cpp
)
if(ENP_PLATFORM STREQUAL "windows")
    list(APPEND ENP_OS_SOURCES src/os/win32/OsWin32.cpp src/os/win32/PtyWin32.cpp src/os/win32/ImageWin32.cpp)
elseif(ENP_PLATFORM MATCHES "linux|macos")
    list(APPEND ENP_OS_SOURCES src/os/posix/FsPosix.cpp src/os/unix/OsUnix.cpp src/os/posix/PtyPosix.cpp)
elseif(ENP_PLATFORM STREQUAL "android")
    list(APPEND ENP_OS_SOURCES src/os/posix/FsPosix.cpp src/os/android/OsAndroid.cpp src/os/common/PtyNone.cpp src/os/android/ImageAndroid.cpp)
elseif(ENP_PLATFORM STREQUAL "web")
    list(APPEND ENP_OS_SOURCES src/os/posix/FsPosix.cpp src/os/web/OsWeb.cpp src/os/common/PtyNone.cpp)
endif()
# The platform's picture decoder (os/Image.h): WIC, AImageDecoder, ImageIO; on Linux the system's
# libjpeg-turbo when it has one (found at run time); none on the web.
if(ENP_PLATFORM STREQUAL "macos")
    list(APPEND ENP_OS_SOURCES src/os/unix/ImageApple.cpp)
elseif(ENP_PLATFORM STREQUAL "linux")
    list(APPEND ENP_OS_SOURCES src/os/unix/ImageTurbo.cpp)
elseif(ENP_PLATFORM STREQUAL "web")
    list(APPEND ENP_OS_SOURCES src/os/common/ImageNone.cpp)
endif()
if(ENP_PLATFORM STREQUAL "android")
    list(APPEND ENP_OS_SOURCES src/os/android/DownloadAndroid.cpp)
else()
    list(APPEND ENP_OS_SOURCES src/os/common/Download.cpp)
endif()

add_library(enp_os STATIC ${ENP_OS_SOURCES})
if(ENP_MEMORY_ACCOUNTING)
    target_compile_definitions(enp_os PUBLIC ENP_MEMORY_ACCOUNTING=1)
endif()
if(ENP_PLATFORM STREQUAL "macos" AND ENP_APPLE_MALLOC_PRESSURE_RELIEF)
    target_compile_definitions(enp_os PRIVATE ENP_APPLE_MALLOC_PRESSURE_RELIEF=1)
endif()
if(ENP_PLATFORM STREQUAL "macos" AND ENP_MEMORY_ACCOUNTING AND ENP_APPLE_LARGE_ALLOCATION_MMAP)
    target_compile_definitions(enp_os PRIVATE ENP_APPLE_LARGE_ALLOCATION_MMAP=1)
endif()
target_include_directories(enp_os PUBLIC include)
if(ENP_PLATFORM STREQUAL "macos")
    target_link_libraries(enp_os PRIVATE "-framework ImageIO" "-framework CoreGraphics" "-framework CoreFoundation")
endif()
if(ENP_PLATFORM STREQUAL "windows")
    target_link_libraries(enp_os PRIVATE comdlg32 shell32 user32 ws2_32 ole32 windowscodecs)
    target_compile_definitions(enp_os PRIVATE UNICODE _UNICODE)
elseif(ENP_PLATFORM MATCHES "linux|macos")
    find_package(Threads REQUIRED)
    target_link_libraries(enp_os PUBLIC Threads::Threads)
    if(ENP_PLATFORM STREQUAL "linux")
        target_link_libraries(enp_os PRIVATE util ${CMAKE_DL_LIBS}) # forkpty; dlopen (libturbojpeg)
    endif()
elseif(ENP_PLATFORM STREQUAL "android")
    add_library(native_app_glue STATIC ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
    target_include_directories(native_app_glue PUBLIC ${ANDROID_NDK}/sources/android/native_app_glue)
    target_link_libraries(native_app_glue PUBLIC android log)
    target_link_libraries(enp_os PUBLIC native_app_glue android log)
endif()

# ---- MVVM layers -----------------------------------------------------------------------------
# model: pure domain data and logic (no OS, no UI)
# libdeflate (pinned submodule): DEFLATE decompression for the document readers (PDF streams, zip
# packages). Decompression only; the processor's vector code is chosen at run time.
set(ENP_LIBDEFLATE_DIR ${ENP_THIRD_PARTY}/libdeflate)
if(NOT EXISTS ${ENP_LIBDEFLATE_DIR}/libdeflate.h)
    message(FATAL_ERROR "Submodule 3rdparty/libdeflate missing. Run: git submodule update --init --recursive")
endif()
add_library(libdeflate STATIC
    ${ENP_LIBDEFLATE_DIR}/lib/deflate_decompress.c
    ${ENP_LIBDEFLATE_DIR}/lib/utils.c
    ${ENP_LIBDEFLATE_DIR}/lib/x86/cpu_features.c
    ${ENP_LIBDEFLATE_DIR}/lib/arm/cpu_features.c)
target_include_directories(libdeflate PUBLIC ${ENP_LIBDEFLATE_DIR})
set_target_properties(libdeflate PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)

add_library(enp_model STATIC
    src/model/TextBuffer.cpp
    src/model/Document.cpp
    src/model/WorkspaceFile.cpp
    src/model/Diff.cpp
    src/model/Review.cpp
    src/model/TerminalScreen.cpp
    src/model/Outline.cpp
    src/model/Symbols.cpp
    src/model/CLibrary.cpp
    src/model/Markdown.cpp
    src/model/Math.cpp
    src/model/FileKind.cpp
    src/model/Emoji.cpp
    src/model/Csv.cpp
    src/model/DataTree.cpp
    src/model/BinaryInfo.cpp
    src/model/BinaryElf.cpp
    src/model/BinaryPe.cpp
    src/model/Language.cpp
    src/model/Highlighter.cpp
    src/model/Indent.cpp
    src/model/Search.cpp
    src/model/Hash.cpp
    src/model/Json.cpp
    src/model/LogicalMap.cpp
    src/model/Git.cpp
    src/model/AgentRoute.cpp
    # Documents read for viewing (plan 25): packages, then the readers
    src/model/package/Inflate.cpp
    src/model/package/Zip.cpp
    src/model/package/Xml.cpp
    src/model/viewer/ImagePack.cpp
    src/model/viewer/Viewer.cpp
    src/model/viewer/NumberFormat.cpp
    src/model/viewer/Ooxml.cpp
    src/model/viewer/Chart.cpp
    src/model/viewer/ConditionalFormat.cpp
    src/model/viewer/Shapes.cpp
    src/model/viewer/SheetTable.cpp
    src/model/viewer/SmartArt.cpp
    src/model/viewer/Docx.cpp
    src/model/viewer/Xlsx.cpp
    src/model/viewer/Pptx.cpp
    src/model/viewer/PdfObjects.cpp
    src/model/viewer/Pdf.cpp
)
target_include_directories(enp_model PUBLIC include)
target_include_directories(enp_model PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/stb)
target_link_libraries(enp_model PRIVATE llvm_demangle libdeflate)

# interfaces: service contracts (header only)
add_library(enp_interfaces INTERFACE)
target_include_directories(enp_interfaces INTERFACE include)
target_link_libraries(enp_interfaces INTERFACE enp_model)

# services: interface implementations on top of the OS layer. So far only the terminal; files,
# settings, the agent and the review tools still run on dummy data (DummyData.cpp).
add_library(enp_services STATIC
    src/services/TerminalService.cpp
    src/services/FileService.cpp
    src/services/AgentCliService.cpp
    src/services/ProcessService.cpp
    src/services/AutomationServer.cpp
    src/services/LocalEngineService.cpp
    src/services/DocumentReaderService.cpp
    src/services/ServiceContainer.cpp
    src/services/ImageDecoder.cpp
    src/services/ImageEncoder.cpp
    src/services/EmojiPack.cpp
)
target_include_directories(enp_services PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/stb)
target_link_libraries(enp_services PUBLIC enp_interfaces PRIVATE enp_os)
if(ENP_MEMORY_TRACE)
    target_sources(enp_services PRIVATE src/services/debug/MemoryTrace.cpp)
    target_compile_definitions(enp_services PUBLIC ENP_MEMORY_TRACE=1)
endif()
if(ENP_LOCAL_ENGINE)
    target_link_libraries(enp_services PRIVATE llama-common llama)
    target_compile_definitions(enp_services PRIVATE ENP_HAS_LOCAL_ENGINE=1)
    if(WIN32)
        target_link_libraries(enp_services PRIVATE psapi)
    endif()
else()
    target_compile_definitions(enp_services PRIVATE ENP_HAS_LOCAL_ENGINE=0)
endif()

# viewmodel: presentation logic, depends on interfaces + model only
add_library(enp_viewmodel STATIC
    src/viewmodel/CommandId.cpp
    src/viewmodel/DummyData.cpp
    src/viewmodel/AgentViewModel.cpp
    src/viewmodel/EngineLabViewModel.cpp
    src/viewmodel/ReviewViewModel.cpp
    src/viewmodel/TerminalViewModel.cpp
    src/viewmodel/ViewerViewModel.cpp
    src/viewmodel/LayersViewModel.cpp
    src/viewmodel/LogicalMapViewModel.cpp
    src/viewmodel/FileDialogViewModel.cpp
    src/viewmodel/Settings.cpp
    src/viewmodel/EditHistory.cpp
    src/viewmodel/PointerViewModel.cpp
    src/viewmodel/GitViewModel.cpp
    src/viewmodel/SyntaxColorSettings.cpp
    src/viewmodel/FindInFilesViewModel.cpp
    src/viewmodel/IntellisenseViewModel.cpp
    src/viewmodel/CodeAssistant.cpp
    src/viewmodel/NotepadViewModel.cpp
    src/viewmodel/NotepadCommands.cpp
    src/viewmodel/LineOperations.cpp
    src/viewmodel/MultiCursor.cpp
)
target_link_libraries(enp_viewmodel PUBLIC enp_interfaces)

# view: Dear ImGui rendering bound to the view-models
add_library(enp_view STATIC
    src/view/Theme.cpp
    src/view/Fonts.cpp
    src/view/DocumentFonts.cpp
    src/view/Icons.cpp
    src/view/RibbonView.cpp
    src/view/EditorView.cpp
    src/view/DiffView.cpp
    src/view/AgentPanel.cpp
    src/view/EngineLabView.cpp
    src/view/MarkdownView.cpp
    src/view/MarkdownImages.cpp
    src/view/EmojiCache.cpp
    src/view/DataViews.cpp
    src/view/LibraryView.cpp
    src/view/viewer/PagePainter.cpp
    src/view/viewer/ViewerView.cpp
    src/view/TableReset.cpp
    src/view/MathView.cpp
    src/view/CodeGrid.cpp
    src/view/TouchScroll.cpp
    src/view/Fit.cpp
    src/view/ReviewPanel.cpp
    src/view/TerminalView.cpp
    src/view/UiAutomation.cpp
    src/view/UiStyle.cpp
    src/view/LayersView.cpp
    src/view/MapView.cpp
    src/view/SyntaxColors.cpp
    src/view/PanelFrame.cpp
    src/view/FileDialogView.cpp
    src/view/SearchPanel.cpp
    src/view/GitView.cpp
    src/view/DocumentListView.cpp
    src/view/AgentCursorView.cpp
    src/view/MainView.cpp
)
target_link_libraries(enp_view PUBLIC enp_viewmodel imgui PRIVATE icon_font_headers enp_os enp_services)

# enp/Version.h: the version, commit and release flag, written at build time (cmake/Version.cmake).
set(ENP_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_custom_target(enp_version
    COMMAND ${CMAKE_COMMAND} -DVERSION=${PROJECT_VERSION} -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DOUTPUT=${ENP_GENERATED_DIR}/enp/Version.h -DRELEASE_TAG=${ENP_RELEASE_TAG}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Version.cmake
    BYPRODUCTS ${ENP_GENERATED_DIR}/enp/Version.h
    COMMENT "Version ${PROJECT_VERSION}"
    VERBATIM)
add_dependencies(enp_view enp_version)
target_include_directories(enp_view PRIVATE ${ENP_GENERATED_DIR})

# ---- Application -------------------------------------------------------------------------------
# automation: the MCP connector (agent access to the running app) and its command handlers
add_library(enp_automation STATIC
    src/app/AutomationController.cpp
    src/app/McpBridge.cpp
)
target_include_directories(enp_automation PUBLIC src/app)
target_link_libraries(enp_automation PUBLIC enp_view enp_services enp_viewmodel PRIVATE enp_os)

set(ENP_APP_SOURCES src/app/Application.cpp src/app/FrameCapture.cpp)

if(ENP_PLATFORM STREQUAL "android")
    add_library(easynotepad SHARED ${ENP_APP_SOURCES} src/app/MainAndroid.cpp)
    target_link_libraries(easynotepad PRIVATE enp_view enp_services enp_automation enp_os native_app_glue)
    target_link_options(easynotepad PRIVATE "-u" "ANativeActivity_onCreate")
else()
    add_executable(EasyNotepad ${ENP_APP_SOURCES} src/app/MainGlfw.cpp)
    target_link_libraries(EasyNotepad PRIVATE enp_view enp_services enp_automation enp_os)
    add_dependencies(EasyNotepad enp_version)
    target_include_directories(EasyNotepad PRIVATE ${ENP_GENERATED_DIR})
    if(WIN32)
        # Explorer's Properties > Details: the version, the product and its description.
        enable_language(RC)
        configure_file(cmake/EasyNotepad.rc.in ${ENP_GENERATED_DIR}/EasyNotepad.rc @ONLY)
        target_sources(EasyNotepad PRIVATE ${ENP_GENERATED_DIR}/EasyNotepad.rc)
    endif()

    if(ENP_IS_DESKTOP)
        # Custom title bar: native caption hit-testing on Windows, manual move/resize on X11.
        target_sources(EasyNotepad PRIVATE src/app/GlfwWindowFrame.cpp)
        target_link_libraries(EasyNotepad PRIVATE glad)
        if(WIN32)
            target_link_libraries(EasyNotepad PRIVATE dwmapi)
        endif()
        # Ship bundled resources next to the executable.
        add_custom_command(TARGET EasyNotepad POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    ${CMAKE_CURRENT_SOURCE_DIR}/resources $<TARGET_FILE_DIR:EasyNotepad>/resources
            COMMENT "Copying resources")
        install(TARGETS EasyNotepad RUNTIME DESTINATION bin)
        install(DIRECTORY resources/ DESTINATION share/easynotepad/resources)
    endif()

    if(ENP_PLATFORM STREQUAL "windows")
        if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug") # keep the console for Debug builds
            set_target_properties(EasyNotepad PROPERTIES WIN32_EXECUTABLE TRUE)
        endif()
        if(MSVC)
            target_link_options(EasyNotepad PRIVATE /ENTRY:mainCRTStartup)
        endif()
    elseif(ENP_PLATFORM STREQUAL "web")
        set_target_properties(EasyNotepad PROPERTIES SUFFIX ".html" OUTPUT_NAME "index")
        target_link_options(EasyNotepad PRIVATE
            ${ENP_WEB_GLFW_FLAG}
            -sALLOW_MEMORY_GROWTH=1
            -sMIN_WEBGL_VERSION=2
            -sMAX_WEBGL_VERSION=2
            -sEXPORTED_RUNTIME_METHODS=ccall
            "-sEXPORTED_FUNCTIONS=_main,_enp_web_file_dialog_result"
            "-sDEFAULT_LIBRARY_FUNCS_TO_INCLUDE=$UTF8ToString"
            --preload-file ${CMAKE_CURRENT_SOURCE_DIR}/resources@/assets
            --shell-file ${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html)
        set_property(TARGET EasyNotepad APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html)
    endif()
endif()

foreach(target enp_os enp_model enp_services enp_viewmodel enp_view enp_automation)
    enp_set_warnings(${target})
endforeach()
if(TARGET EasyNotepad)
    enp_set_warnings(EasyNotepad)
elseif(TARGET easynotepad)
    enp_set_warnings(easynotepad)
endif()

# ---- Tests ------------------------------------------------------------------------------------
if(ENP_BUILD_TESTS AND ENP_IS_DESKTOP)
    enable_testing()
    add_executable(enp_tests tests/TestMain.cpp)
    target_link_libraries(enp_tests PRIVATE enp_viewmodel enp_model enp_os enp_services enp_automation)
    target_include_directories(enp_tests PRIVATE tests src/app)
    target_compile_definitions(enp_tests PRIVATE ENP_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    enp_set_warnings(enp_tests)
    add_test(NAME enp_tests COMMAND enp_tests)

    # Headless UI test (no GPU): ImGui ID conflicts and title-bar drag regions.
    add_executable(enp_ui_tests tests/UiIdTest.cpp)
    target_link_libraries(enp_ui_tests PRIVATE enp_view enp_os)
    target_include_directories(enp_ui_tests PRIVATE tests)
    target_compile_definitions(enp_ui_tests PRIVATE ENP_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    enp_set_warnings(enp_ui_tests)
    add_custom_command(TARGET enp_ui_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_CURRENT_SOURCE_DIR}/resources $<TARGET_FILE_DIR:enp_ui_tests>/resources)
    add_test(NAME enp_ui_tests COMMAND enp_ui_tests)
endif()
