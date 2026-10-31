# 06 · Highlighting cost independent of file size

**Problem.** Each frame the editor (and the minimap) highlights every line from the top of the file
down to the first visible line, only to know the state (block comment, string, fence) there:
scrolling to the end of a 100k-line file costs 100k lines per frame.

**Design.** `model::HighlightCache`: the lexer state at the start of every line, computed lazily and
kept per document; invalidated from the first changed line (TextBuffer reports the lowest line touched
since a mark) or entirely when the language changes. The editor and the minimap ask it for the state
of their first line, then highlight only what they draw.

**Tests.** Unit: states match a full re-scan after edits at the start, middle and end, after
language changes, and across block comments opened/closed by an edit. Timing check: a 200k-line file
scrolled to the end highlights only the visible lines.
