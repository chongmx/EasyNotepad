# LLVM Demangle (vendored)

The name demangler of the LLVM project, copied unchanged from
[llvm-project](https://github.com/llvm/llvm-project) tag `llvmorg-19.1.7`:

- `include/llvm/Demangle/` from `llvm/include/llvm/Demangle/`
- `lib/` from `llvm/lib/Demangle/`
- `LICENSE.TXT` from `llvm/LICENSE.TXT` (Apache License 2.0 with LLVM Exceptions)

It demangles C++ names of GCC/Clang (Itanium ABI) and MSVC, Rust (v0) and D, and needs nothing
else from LLVM. EasyNotepad builds it as the `llvm_demangle` target and calls it from
`src/model/BinaryInfo.cpp` (the library view).

To update, download the same files from a newer tag and rebuild; the tests
`names_are_demangled_and_marked_by_scheme` and
`libraries_list_functions_imports_sections_and_characteristics` cover the use.
