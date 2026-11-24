---
name: memory
description: remember the user's preferences, learn from mistakes, correct and compact what you remember
---

# Memory

What you remember is kept in a file between conversations and shown in your guide with every
question (`#id [kind] text`). Use it: follow the preferences, avoid the mistakes in the lessons.

## Kinds
- **preference** - how the user likes things: "Answers in short bullet points", "Dates as 2026-11-20".
- **lesson** - a mistake not to repeat, and what works instead: "query_table: column names must be
  exact; list them first".
- **fact** - about the user's work that will matter again: "The budget sheet is data/budget.xlsx".
- **note** - for the coming days (kept 7 days): "Waiting for Lisa's numbers".
- **temporary** - only for this session: steps of a long task, figures you are collecting.

## When to remember
- The user says "I prefer", "always", "never", "from now on", or corrects how you did something:
  `remember {"text": "...", "kind": "preference"}`.
- You made a mistake and found the fix (a tool call that failed, a wrong number the user pointed out):
  `remember {"text": "what went wrong and what to do instead", "kind": "lesson"}`.
- Keep each memory to one plain sentence. Do not store secrets, passwords or personal data the
  user did not ask you to keep.

## Keeping it right
- A memory turned out wrong or out of date: `amend_memory {"id": 3, "text": "..."}`.
- No longer true: `forget_memory {"id": 3}`.
- The list is long or repeats itself (the guide says when): compact one kind at a time.
  1. `recall {"kind": "lesson"}`
  2. merge duplicates, drop what no longer matters, keep each line short
  3. `rewrite_memories {"kind": "lesson", "lines": "first\nsecond\n..."}`
- Tell the user briefly when you remember or change something about them.
