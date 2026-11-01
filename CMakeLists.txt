cmake_minimum_required(VERSION 3.22)

project(EasyNotepad VERSION 0.1.0 LANGUAGES C CXX)

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
if(NOT MSVC)
    target_compile_options(imgui PRIVATE -w)
endif()

# Font Awesome codepoint headers (header only).
add_library(icon_font_headers INTERFACE)
target_include_directories(icon_font_headers INTERFACE ${ENP_THIRD_PARTY}/IconFontCppHeaders)

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
    src/os/common/Dispatch.cpp
    src/os/common/Path.cpp
)
if(ENP_PLATFORM STREQUAL "windows")
    list(APPEND ENP_OS_SOURCES src/os/win32/OsWin32.cpp src/os/win32/PtyWin32.cpp)
elseif(ENP_PLATFORM MATCHES "linux|macos")
    list(APPEND ENP_OS_SOURCES src/os/posix/FsPosix.cpp src/os/unix/OsUnix.cpp src/os/posix/PtyPosix.cpp)
elseif(ENP_PLATFORM STREQUAL "android")
    list(APPEND ENP_OS_SOURCES src/os/posix/FsPosix.cpp src/os/android/OsAndroid.cpp src/os/common/PtyNone.cpp)
elseif(ENP_PLATFORM STREQUAL "web")
    list(APPEND ENP_OS_SOURCES src/os/posix/FsPosix.cpp src/os/web/OsWeb.cpp src/os/common/PtyNone.cpp)
endif()

add_library(enp_os STATIC ${ENP_OS_SOURCES})
target_include_directories(enp_os PUBLIC include)
if(ENP_PLATFORM STREQUAL "windows")
    target_link_libraries(enp_os PRIVATE comdlg32 shell32 ws2_32)
    target_compile_definitions(enp_os PRIVATE UNICODE _UNICODE)
elseif(ENP_PLATFORM MATCHES "linux|macos")
    find_package(Threads REQUIRED)
    target_link_libraries(enp_os PUBLIC Threads::Threads)
    if(ENP_PLATFORM STREQUAL "linux")
        target_link_libraries(enp_os PRIVATE util) # forkpty
    endif()
elseif(ENP_PLATFORM STREQUAL "android")
    add_library(native_app_glue STATIC ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
    target_include_directories(native_app_glue PUBLIC ${ANDROID_NDK}/sources/android/native_app_glue)
    target_link_libraries(native_app_glue PUBLIC android log)
    target_link_libraries(enp_os PUBLIC native_app_glue android log)
endif()

# ---- MVVM layers -----------------------------------------------------------------------------
# model: pure domain data and logic (no OS, no UI)
add_library(enp_model STATIC
    src/model/TextBuffer.cpp
    src/model/Document.cpp
    src/model/Diff.cpp
    src/model/Review.cpp
    src/model/TerminalScreen.cpp
    src/model/Outline.cpp
    src/model/Markdown.cpp
    src/model/Language.cpp
    src/model/Highlighter.cpp
    src/model/Indent.cpp
    src/model/Search.cpp
    src/model/Hash.cpp
    src/model/Json.cpp
    src/model/LogicalMap.cpp
    src/model/Git.cpp
)
target_include_directories(enp_model PUBLIC include)

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
    src/services/ServiceContainer.cpp
)
target_link_libraries(enp_services PUBLIC enp_interfaces PRIVATE enp_os)

# viewmodel: presentation logic, depends on interfaces + model only
add_library(enp_viewmodel STATIC
    src/viewmodel/CommandId.cpp
    src/viewmodel/DummyData.cpp
    src/viewmodel/AgentViewModel.cpp
    src/viewmodel/ReviewViewModel.cpp
    src/viewmodel/TerminalViewModel.cpp
    src/viewmodel/LayersViewModel.cpp
    src/viewmodel/LogicalMapViewModel.cpp
    src/viewmodel/FileDialogViewModel.cpp
    src/viewmodel/Settings.cpp
    src/viewmodel/EditHistory.cpp
    src/viewmodel/PointerViewModel.cpp
    src/viewmodel/GitViewModel.cpp
    src/viewmodel/SyntaxColorSettings.cpp
    src/viewmodel/FindInFilesViewModel.cpp
    src/viewmodel/NotepadViewModel.cpp
    src/viewmodel/NotepadCommands.cpp
    src/viewmodel/LineOperations.cpp
)
target_link_libraries(enp_viewmodel PUBLIC enp_interfaces)

# view: Dear ImGui rendering bound to the view-models
add_library(enp_view STATIC
    src/view/Theme.cpp
    src/view/Fonts.cpp
    src/view/Icons.cpp
    src/view/RibbonView.cpp
    src/view/EditorView.cpp
    src/view/DiffView.cpp
    src/view/AgentPanel.cpp
    src/view/MarkdownView.cpp
    src/view/ReviewPanel.cpp
    src/view/TerminalView.cpp
    src/view/UiAutomation.cpp
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
target_link_libraries(enp_view PUBLIC enp_viewmodel imgui PRIVATE icon_font_headers enp_os)
target_compile_definitions(enp_view PRIVATE ENP_VERSION="${PROJECT_VERSION}")

# ---- Application -------------------------------------------------------------------------------
# automation: the MCP connector (agent access to the running app) and its command handlers
add_library(enp_automation STATIC
    src/app/AutomationController.cpp
    src/app/McpBridge.cpp
)
target_include_directories(enp_automation PUBLIC src/app)
target_link_libraries(enp_automation PUBLIC enp_view enp_services enp_viewmodel PRIVATE enp_os)

set(ENP_APP_SOURCES src/app/Application.cpp)

if(ENP_PLATFORM STREQUAL "android")
    add_library(easynotepad SHARED ${ENP_APP_SOURCES} src/app/MainAndroid.cpp)
    target_link_libraries(easynotepad PRIVATE enp_view enp_services enp_automation enp_os native_app_glue)
    target_link_options(easynotepad PRIVATE "-u" "ANativeActivity_onCreate")
else()
    add_executable(EasyNotepad ${ENP_APP_SOURCES} src/app/MainGlfw.cpp)
    target_link_libraries(EasyNotepad PRIVATE enp_view enp_services enp_automation enp_os)
    target_compile_definitions(EasyNotepad PRIVATE ENP_VERSION="${PROJECT_VERSION}")

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
    enp_set_warnings(enp_tests)
    add_test(NAME enp_tests COMMAND enp_tests)

    # Headless UI test (no GPU): ImGui ID conflicts and title-bar drag regions.
    add_executable(enp_ui_tests tests/UiIdTest.cpp)
    target_link_libraries(enp_ui_tests PRIVATE enp_view enp_os)
    target_include_directories(enp_ui_tests PRIVATE tests)
    enp_set_warnings(enp_ui_tests)
    add_custom_command(TARGET enp_ui_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_CURRENT_SOURCE_DIR}/resources $<TARGET_FILE_DIR:enp_ui_tests>/resources)
    add_test(NAME enp_ui_tests COMMAND enp_ui_tests)
endif()
