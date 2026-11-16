// Libraries loaded at run time (the engine libraries of plan 21: enp-engine-cuda next to the app).
#pragma once

#include <string>

namespace enp::os::lib {

// The file name of a library on this platform: "enp-engine-cuda" -> "enp-engine-cuda.dll" on
// Windows, ".dylib" on macOS, ".so" elsewhere.
std::string fileName(const std::string& base);

// Loads a library by its full path; null with `error` (in words) when it cannot. On Windows the
// library's own folder is searched for what it needs (cuBLAS beside enp-engine-cuda.dll).
void* open(const std::string& path, std::string* error);
// An exported function, or null.
void* symbol(void* library, const char* name);
void close(void* library);

} // namespace enp::os::lib
