---
name: close-reading
description: read a long or odd file closely, part by part, for an objective - every instance, exact figures, a file the other tools cannot make sense of
---

# Reading a file closely

`read_closely` goes through one file a small part at a time (a few thousand characters, numbered
lines, each part overlapping the last). With each part you get the objective, what the task keeps
in mind and your notes so far, and you write down only what is new. At the end it answers from the
notes. It is slower than the other tools (a few seconds a part), and more careful.

## When
- A long file, when what you need could be anywhere in it: a log, a long report, a transcript.
- A file the other tools cannot make sense of: an export, a jumbled PDF text, HTML, an odd format,
  text inside a binary file.
- Every instance matters ("every payment over 500", "all the dates the deadline moved"), or one
  exact figure buried in a long text.
Not for tables query_table reads (it totals rows exactly), nor a short file read_file shows whole.
To count lines (how many ERROR lines from one service), search_files counts exactly:
`search_files {"query": "ERROR", "also": "billing", "include": "*.log"}`. Reading misses some.

## The objective
Say what to find or collect and in what form, in the question's words, with every condition:
`read_closely {"path": "logs/app.log", "objective": "every ERROR from the billing service, with its time and message"}`
- `"mode": "find"` stops at the first part that has it (one fact: "the notice period").
- `"mode": "collect"` gives the notes only (a list to work on next, with calculate).
- `"window": "small"` for dense text with many small facts; `"large"` for long plain prose.

## Its notes
Each note carries the figures as written and the line they are on: `- Paid Acme 1,250.50 EUR on 3 May
(line 30)`. A note whose figure is not in the part it came from is left out, so copy figures exactly.
Totals and differences of the notes' figures: calculate, never in your head.
