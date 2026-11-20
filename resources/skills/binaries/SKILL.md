---
name: binaries
description: read programs and libraries (.dll .exe .so) - exported and imported functions, sections, raw bytes
---

# Programs and libraries

`inspect_binary` shows what EasyNotepad's binary view shows: its kind (PE32+ DLL, ELF shared
library, ...), the overview, and lists.
- `inspect_binary {"path": "bin/plugin.dll"}` - the overview and how many exports and imports.
- `"list": "exports"` - the functions it offers, with readable C++ names
  (`sample::Widget::area(int, int) const`, the mangled name after it).
- `"list": "imports"` - what it uses, and from which library (KERNEL32.dll, libc.so.6).
- `"list": "libraries"`, `"list": "sections"`, `"list": "functions"` (exported functions only).
- `"filter": "widget"` - only names containing that.

`read_bytes {"path": "...", "offset": 0, "length": 256}` shows raw bytes as hex and text, for any
file: headers, magic numbers, embedded strings.

Answer from what the lists show: what the library is for, its API, what it depends on. Say when a
name is only a guess from its spelling.
