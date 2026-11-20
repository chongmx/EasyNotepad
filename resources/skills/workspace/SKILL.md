---
name: workspace
description: find your way in the open folder - list folders, find text, read code and notes
---

# Working in the open folder

Paths are relative to the open folder (the guide names it). You cannot leave it.

## Find before you read
1. Not sure where something is? `search_files` with a word you expect in it (a function name, an
   error message, a heading). Narrow with `include`, e.g. `*.cpp;*.h` or `*.md`.
2. To see what a project contains, `list_files` with no path, then the folders that matter
   (`src/`, `docs/`). Do not list every folder: pick by name.
3. Then `read_file` the files you found, at the lines the search gave (`start_line` a few lines
   before the match, `end_line` 60-100 lines after).

## Reading code
- `read_file` gives 200 lines when you give no range, numbered `12| text`. Long files: read the part
  you need; read on with the next range only if the answer needs it.
- An open file is read as the user has it, unsaved edits included.
- Quote line numbers in your answer (`src/main.cpp:42`), so the user can go there.

## Not text?
`read_file` says so. Use `read_document` for PDF, Word, Excel, PowerPoint; `look_at_image` for
pictures; `listen_to` for recordings.

## Example
User: "Where is the config parsed?"
1. `search_files {"query": "parse", "include": "*.cpp;*.h"}`
2. `read_file {"path": "src/Config.cpp", "start_line": 30, "end_line": 110}`
3. Answer: "In `src/Config.cpp:41`, `Config::load` reads ..."
