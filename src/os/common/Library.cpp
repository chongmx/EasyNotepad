#include "enp/os/Library.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif !defined(__EMSCRIPTEN__)
#include <dlfcn.h>
#endif

namespace enp::os::lib {

std::string fileName(const std::string& base)
{
#if defined(_WIN32)
    return base + ".dll";
#elif defined(__APPLE__)
    return base + ".dylib";
#else
    return base + ".so";
#endif
}

#if defined(_WIN32)

namespace {

std::wstring widen(const std::string& s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
    return out;
}

std::string message(DWORD code)
{
    wchar_t* text = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::string out;
    if (n > 0 && text) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, text, int(n), nullptr, 0, nullptr, nullptr);
        out.assign(size_t(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text, int(n), out.data(), size, nullptr, nullptr);
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ' || out.back() == '.'))
            out.pop_back();
    }
    if (text)
        LocalFree(text);
    return out.empty() ? "error " + std::to_string(code) : out + " (error " + std::to_string(code) + ")";
}

} // namespace

void* open(const std::string& path, std::string* error)
{
    // No "the program can't start" dialog for a missing dependency: the error comes back instead.
    const UINT previous = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    HMODULE module = LoadLibraryExW(widen(path).c_str(), nullptr,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    const DWORD code = module ? 0 : GetLastError();
    SetErrorMode(previous);
    if (!module && error)
        *error = message(code);
    return reinterpret_cast<void*>(module);
}

void* symbol(void* library, const char* name)
{
    return library ? reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(library), name)) : nullptr;
}

void close(void* library)
{
    if (library)
        FreeLibrary(reinterpret_cast<HMODULE>(library));
}

#elif defined(__EMSCRIPTEN__)

void* open(const std::string&, std::string* error)
{
    if (error)
        *error = "libraries cannot be loaded here";
    return nullptr;
}
void* symbol(void*, const char*) { return nullptr; }
void close(void*) {}

#else

void* open(const std::string& path, std::string* error)
{
    void* library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!library && error) {
        const char* text = dlerror();
        *error = text ? text : "the library could not be loaded";
    }
    return library;
}

void* symbol(void* library, const char* name) { return library ? dlsym(library, name) : nullptr; }

void close(void* library)
{
    if (library)
        dlclose(library);
}

#endif

} // namespace enp::os::lib
