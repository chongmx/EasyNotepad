# enp-engine-cuda: the local model on llama.cpp's CUDA backend, as a library the app loads at run
# time when it is next to the executable (plan 21). A project of its own, because it is built with
# the CUDA toolkit and, on Windows, MSVC (nvcc does not take MinGW), while the app is built as usual.
#
#   cmake -S engines/cuda -B build/engine-cuda -DCMAKE_BUILD_TYPE=Release [-DCMAKE_CUDA_ARCHITECTURES=120]
#   cmake --build build/engine-cuda --config Release
#
# The result goes to bin/engine-cuda/: enp-engine-cuda.dll (Windows) or enp-engine-cuda.so (Linux).
# Copy it next to EasyNotepad; on Windows also cublas64_*.dll and cublasLt64_*.dll from the toolkit.
cmake_minimum_required(VERSION 3.21)
project(enp_engine_cuda LANGUAGES C CXX)

set(ENP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    # No Visual C++ redistributable needed next to the library.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/utf-8>)
endif()

if(NOT EXISTS ${ENP_ROOT}/3rdparty/llama.cpp/CMakeLists.txt)
    message(FATAL_ERROR "Submodule 3rdparty/llama.cpp missing. Run: git submodule update --init 3rdparty/llama.cpp")
endif()
# The same patched llama.cpp as the app's CPU engine (patches/llama.cpp).
include(${ENP_ROOT}/cmake/PatchedSource.cmake)
enp_patched_source(llama.cpp SOURCE ${ENP_ROOT}/3rdparty/llama.cpp PATCHES ${ENP_ROOT}/patches/llama.cpp
                   OUT ENP_LLAMA_DIR PATHS CMakeLists.txt LICENSE cmake common ggml include licenses src tools/mtmd vendor)
string(REGEX REPLACE "^b" "" LLAMA_BUILD_NUMBER "${ENP_LLAMA_DIR_TAG}")
set(LLAMA_BUILD_COMMIT ${ENP_LLAMA_DIR_COMMIT})

set(BUILD_SHARED_LIBS OFF) # llama.cpp and ggml inside the one library
foreach(_opt LLAMA_BUILD_TESTS LLAMA_BUILD_TOOLS LLAMA_BUILD_EXAMPLES LLAMA_BUILD_SERVER LLAMA_BUILD_APP
             LLAMA_BUILD_UI LLAMA_OPENSSL LLAMA_SUBPROCESS LLAMA_ALL_WARNINGS GGML_OPENMP GGML_NATIVE LLAMA_COMMON_DOWNLOAD)
    set(${_opt} OFF CACHE BOOL "" FORCE)
endforeach()
set(LLAMA_BUILD_COMMON ON CACHE BOOL "" FORCE) # speculative decoding lives there
set(LLAMA_BUILD_MTMD ON CACHE BOOL "" FORCE) # images and sound for the model (tools/mtmd, without its video)
set(GGML_CUDA ON CACHE BOOL "" FORCE)
# The CUDA runtime linked in; on Linux cuBLAS too, so the library needs only the NVIDIA driver.
# Windows has no static cuBLAS: cublas64_*.dll and cublasLt64_*.dll go next to the library.
set(GGML_STATIC ON CACHE BOOL "" FORCE)
# The CPU part (what stays on the host, such as the per-layer embeddings) at the app's CPU level.
foreach(_opt GGML_AVX GGML_AVX2 GGML_FMA GGML_F16C GGML_BMI2)
    set(${_opt} ON CACHE BOOL "" FORCE)
endforeach()
add_subdirectory(${ENP_LLAMA_DIR} ${CMAKE_BINARY_DIR}/llama.cpp EXCLUDE_FROM_ALL)
target_compile_definitions(mtmd PRIVATE _USE_MATH_DEFINES) # M_PI in strict C++17 (MinGW, MSVC)

add_library(enp_engine_cuda SHARED
    EngineCuda.cpp
    ${ENP_ROOT}/src/engine/LlamaRunner.cpp
    ${ENP_ROOT}/src/engine/EngineJson.cpp
    ${ENP_ROOT}/src/model/Base64.cpp
    ${ENP_ROOT}/src/model/Json.cpp)
target_include_directories(enp_engine_cuda PRIVATE ${ENP_ROOT}/include)
target_link_libraries(enp_engine_cuda PRIVATE mtmd llama-common llama)
set_target_properties(enp_engine_cuda PROPERTIES
    OUTPUT_NAME enp-engine-cuda PREFIX ""
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON
    RUNTIME_OUTPUT_DIRECTORY ${ENP_ROOT}/bin/engine-cuda
    LIBRARY_OUTPUT_DIRECTORY ${ENP_ROOT}/bin/engine-cuda)
foreach(_cfg Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER ${_cfg} _CFG)
    set_target_properties(enp_engine_cuda PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${_CFG} ${ENP_ROOT}/bin/engine-cuda
                                                     LIBRARY_OUTPUT_DIRECTORY_${_CFG} ${ENP_ROOT}/bin/engine-cuda)
endforeach()
if(UNIX AND NOT APPLE)
    # Only the enp_engine_* functions are exported; libstdc++ comes along, so any Linux runs it.
    target_link_options(enp_engine_cuda PRIVATE -Wl,--exclude-libs,ALL -static-libstdc++ -static-libgcc)
endif()
