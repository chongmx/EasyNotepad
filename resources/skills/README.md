# Skills of the local agent

The local model in the agent panel (Gemma 4 E2B, offline) works in the open folder through the
app's tools (`src/app/LocalAgentTools.cpp`). Its guide lists these skills by name and description
(`index.json`); it reads a skill's `SKILL.md` with the `read_skill` tool before a task of that kind.

| Skill | For |
|---|---|
| [workspace](workspace/SKILL.md) | the open folder: listing, finding, reading text files |
| [documents](documents/SKILL.md) | PDF, Word, Excel and PowerPoint: text by page, pages as pictures |
| [office-work](office-work/SKILL.md) | summaries, spreadsheet figures, letters, minutes, checking numbers |
| [data-and-calculations](data-and-calculations/SKILL.md) | query_table and calculate: filters, groups, totals, exact sums |
| [pictures-and-sound](pictures-and-sound/SKILL.md) | how carefully to look (detail and token cost), pages, listening |
| [meetings](meetings/SKILL.md) | transcribing recordings into a text file, minutes, action items |
| [binaries](binaries/SKILL.md) | programs and libraries: exports, imports, sections, raw bytes |
| [memory](memory/SKILL.md) | preferences, lessons from mistakes, correcting and compacting memories |
| [editing](editing/SKILL.md) | proposing changes and new files for the user to review |
| [easynotepad](easynotepad/SKILL.md) | the app: what the user sees, showing them things |

The app itself uses one more, not listed in `index.json`: [planning](planning/SKILL.md), the
instructions with which the model splits a question into steps before the work starts (the app
then gives it the steps one at a time, `AgentViewModel::planLocal`).

Adding a skill: a folder with a `SKILL.md` (front matter `name` and `description`, then the
instructions), and a line in `index.json`. Keep each short and concrete: the model is small, and a
skill is read into its context whole.
