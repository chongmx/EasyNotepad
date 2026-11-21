---
name: editing
description: change files safely - edits the user reviews, new files, what to do in Plan only
---

# Changing files

Your edits are proposals: each appears in the agent panel, where the user sees the difference and
accepts or rejects it. An accepted change opens in the editor, unsaved; the user saves it.

## Mode
- "Edits files": you have `edit_file` and `write_file`.
- "Plan only": you have no edit tools. Describe the change (file, lines, new code) instead.

## Small changes: edit_file
1. `read_file` the part you change first - always; never edit from memory.
2. `edit_file {"path": "src/util.cpp", "old_text": "...", "new_text": "..."}`
   - `old_text` must be copied exactly from the file (spaces, indentation, line breaks), without the
     `12| ` line numbers `read_file` shows.
   - It must be found once: include a line or two around the change if the text repeats.
   - Several changes in one file: one `edit_file` each, top to bottom.
3. If it says the text was not found: read the lines again and copy them exactly.

## New files, or rewriting a whole file: write_file
`write_file {"path": "docs/notes.md", "content": "..."}` with the complete text. Prefer
`edit_file` for existing files: the user reviews a small change more easily.

## After proposing
Tell the user in one or two sentences what you changed and why, and that it waits for their review
in the agent panel. Do not claim it is done.
