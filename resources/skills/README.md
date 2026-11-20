# Skills of the local agent

The local model in the agent panel (Gemma 4 E2B, offline) works in the open folder through the
app's tools (`src/app/LocalAgentTools.cpp`). Its guide lists these skills by name and description
(`index.json`); it reads a skill's `SKILL.md` with the `read_skill` tool before a task of that kind.

| Skill | For |
|---|---|
| [workspace](workspace/SKILL.md) | the open folder: listing, finding, reading text files |
| [documents](documents/SKILL.md) | PDF, Word, Excel and PowerPoint: text by page, pages as pictures |
| [pictures-and-sound](pictures-and-sound/SKILL.md) | looking closely at pictures and pages, listening to recordings |
| [editing](editing/SKILL.md) | proposing changes and new files for the user to review |
| [easynotepad](easynotepad/SKILL.md) | the app: what the user sees, showing them things |

Adding a skill: a folder with a `SKILL.md` (front matter `name` and `description`, then the
instructions), and a line in `index.json`. Keep each short and concrete: the model is small, and a
skill is read into its context whole.
