#!/bin/sh
# Rebuilds the binary fixtures of the library viewer tests from fixture.cpp (needs clang, lld-link,
# llvm-lib and llvm-dlltool; the -14 suffix is Ubuntu's). The outputs are committed: the tests do
# not need these tools.
set -e
cd "$(dirname "$0")"
CLANG=${CLANG:-clang}
LLD=${LLD:-lld-link}
LIB=${LIB:-llvm-lib-14}
DLLTOOL=${DLLTOOL:-llvm-dlltool-14}
# Windows, MSVC flavour: an object, an import library for kernel32, the DLL, and its import library.
cat > kernel32.def <<'DEF'
LIBRARY KERNEL32.dll
EXPORTS
GetTickCount
GetProcessHeap
DEF
$DLLTOOL -m i386:x86-64 -d kernel32.def -l kernel32.lib
printf 'LIBRARY USER32.dll\nEXPORTS\nMessageBeep\n' > user32.def
$DLLTOOL -m i386:x86-64 -d user32.def -l user32.lib
$CLANG --target=x86_64-pc-windows-msvc -O1 -fno-exceptions -c fixture.cpp -o fixture.obj
$LLD /dll /noentry /nodefaultlib /out:fixture.dll /implib:fixture.lib fixture.obj kernel32.lib user32.lib /export:forwarded=NTDLL.RtlGetVersion /export:by_ordinal=plain_add,@40,NONAME /delayload:USER32.dll
rm -f kernel32.def kernel32.lib user32.def user32.lib fixture.exp
# Linux: a shared library and a static library holding the object.
$CLANG -O1 -fPIC -shared -nostdlib -fvisibility=hidden -Wl,-soname,libfixture.so.1 fixture.cpp -o libfixture.so
$CLANG -O1 -fPIC -c fixture.cpp -o fixture.o
rm -f libfixture.a && ar rcs libfixture.a fixture.o && rm -f fixture.o
ls -l fixture.obj fixture.dll fixture.lib libfixture.so libfixture.a
