# 26 · AI features: what is connected, and what is not yet

**Why.** The app has many AI entry points: the Agent panel, ribbon commands, review, the map, git,
IntelliSense, the MCP connector and the local model. Some reach a real model end to end. Some still
show the sample workspace's made-up data, or have a button and no model behind it. This item lists
each one's state as the code stands (main at `50401fe`, 15 Nov 2026), so the gaps can be closed in
order.

**How this was checked.** Each feature was traced from its button or shortcut (`RibbonView`,
`MainView`, `AgentPanel`), through its command (`NotepadCommands.cpp`) and view-model, to the
service that runs the model (`AgentCliService`, `LocalEngineService`, `CodeAssistant`). Every MCP
tool the connector advertises (`automationTools()`) was matched to its handler
(`handleAutomation`).

## The two models, and where they run

| Model | What runs it | Windows, Linux, macOS | Android | Web |
|---|---|---|---|---|
| **Claude Code** (or any agent command) | `AgentCliService`: `claude -p --output-format stream-json`, in the workspace folder, with the app's MCP connector | Yes, when `claude` is installed and logged in | No (Claude Code does not run there) | No |
| **Local model** (Gemma 4 E2B) | `LocalEngineService` (llama.cpp, plan 23) | 64-bit, CPU (AVX2) | Yes, CPU (ARMv8.2 dot product) | No engine |

The build turns on no GPU backend of its own: ggml's defaults apply (Metal on macOS). Vulkan,
TensorRT and the climb between engines (plan 23) are not built yet.

## Status

**Connected** means a real model answers in a real folder. **Sample only** means it works in the
sample workspace but does nothing real in a real folder.

### Agent panel

| Feature | Status | Notes |
|---|---|---|
| Chat with Claude Code: streaming, steps, one conversation, New chat, Stop | Connected | `AgentViewModel::askStream`; test `agent_streams_a_claude_code_conversation` |
| Model (Opus, Sonnet, Haiku) and mode (*Edits files*, *Plan only*) | Connected | `--model`, `--permission-mode acceptEdits` or `plan` |
| Claude operates the app (pointer, clicks) | Connected | `--mcp-config` with `EasyNotepad --mcp`; *App control* switch |
| Agent set-up: find, install, log in, agent command | Connected | `agentSetup()`; opens by itself on a missing CLI or login |
| Who answers: Auto, Claude, Local; fallback to the local model; *Ask Claude instead* | Connected | `route()`, `model::routeTask`; tests `agent_auto_route_rules`, `agent_chooses_local_or_claude_and_falls_back` |
| Local answers: selection or the 40 lines round the caret, streaming, unload after 5 minutes | Connected | `agentContextText()`, `LocalEngineService::chat` |
| **The plan list** (tasks) | Sample only | `goLive()` clears `tasks_` and nothing fills it again: a real folder always says "No plan yet". Claude Code's to-do list (`TodoWrite` in the stream) could fill it. |
| **Proposed changes: Accept, Reject, Accept all, Reject all, Compare agent changes** | Sample only | `goLive()` clears `changes_` and nothing adds to it: Claude edits files directly and the editor reloads them. The buttons stay disabled in a real folder. The edits in the stream (`Edit`, `Write`) or `git diff` of the touched files could fill it. |

### Commands that ask the agent

| Feature | Status | Notes |
|---|---|---|
| Ask (Ctrl+I), Explain, Refactor, Generate tests, Fix findings | Connected | `askAgent()` → the Agent panel, routed as above |
| **Dissect Project** (Map view) | Connected, Claude Code only | `LogicalMapViewModel::dissectWithAgent` uses the agent command alone: no local model and no fallback, so nothing on a phone |
| **Agent Review** (Review tab) | Connected, Claude Code only | `ReviewViewModel::runTool(AgentReview)`: the same; findings parsed from `path:line: severity: message` |
| **Write commit message** (Git) | Connected, Claude Code only | `GitViewModel::writeMessage`: without an agent it lists the files. Plan 21 names commit messages a local task; Auto's "commit messages" applies only to questions typed in the chat. |
| **Summarize file** (Read › Folding, wand icon) | Not connected | It only clears the cached summaries and reads them again from comments and structure. No model is asked, though its icon says AI. |

### IntelliSense

| Feature | Status | Notes |
|---|---|---|
| Code from a comment (Enter after a comment, Ctrl+Shift+Space) with Local, Claude or a command | Connected (off until turned on) | `CodeAssistant`; test `code_assistant_writes_code_with_the_chosen_model` |
| **AI completion while typing** (inline, plan 21 item 1) | Not connected | The provider seam exists, but `CodeAssistant` sets `completes = false`: a model is never asked while a name is typed. Inline faint text and Tab are not built. FEATURES.md › "For AI completion" describes the seam, not a working feature. |

### Other agents (the MCP connector)

| Feature | Status | Notes |
|---|---|---|
| `EasyNotepad --mcp`: tools to read the state, list, point, click, hover, run commands, open files and folders, read documents and outlines, type, terminal, search, git, screenshot | Connected (desktop) | All 38 advertised tools have a handler; `screenshot` is answered after the frame is drawn. Tests `mcp_session_speaks_json_rpc`, `automation_endpoint_end_to_end`. Android and web have no connector (`--mcp` is in `MainGlfw` only; automation is off on the web). |
| `ask_agent`, `read_agent_chat` | Connected | Another agent drives the app's own Agent panel |
| **`read_document` on Word, Excel, PowerPoint, PDF** | Not connected | An open viewer document has an empty text buffer, so `read_document` returns `"text": ""`; one not open returns only its format. `model::viewer::plainText()` already gives the text in reading order. |
| Shared control: limits, acting agents, the task manager, `agents_status` (plan 20) | Not built | One pointer and no queue: any connected agent can act at any time |

### The local agent's other jobs (plan 21)

| Feature | Status |
|---|---|
| Answering in the Agent panel, fallback, Auto routing | Connected |
| Judging requests from other agents (needs plan 20) | Not built |
| Watching remote agents (stuck, looping) | Not built |
| Small tasks itself: commit messages, renames | Not built (chat answers only) |
| Safety net: hangs, runaway output, memory | Partly: the engine runs off the UI thread, Stop cancels and an idle model unloads. Watchdog limits are not built. |

### Documents (Word, Excel, PowerPoint, PDF)

| Feature | Status |
|---|---|
| The agent on a viewer document | **Not connected.** The local model gets no text from it (`agentContextText()` reads the empty buffer), and Explain sends "Explain this code." with `file:1` as the place. Claude Code can still open the file itself. |
| Agents reading a document as structure (plan 27, step 27.2) | Not built: see `read_document` above |

### Developer tools

| Feature | Status |
|---|---|
| Engine Lab: device, model files (SHA-256, resumable downloads), runs, drafter comparison, history | Connected (64-bit desktop and Android); test `engine_lab_downloads_checks_and_compares` |

## To connect, in order

Each step is small and stands alone; the order puts what users meet first, first.

1. **Documents in AI.** `read_document` returns `model::viewer::plainText()` for an open viewer
   document (with its format and page count). `agentContextText()` gives the local model the page or
   slide in view, and Explain asks "Explain this page." for a document. Tests: an open PDF read
   through the connector; a local answer about a PDF gets its text.
2. **The plan and the changes in a real folder.** Read `TodoWrite` from the stream into the plan
   list. Record each `Edit`/`Write` from the stream as a change already applied, with its diff, so
   *Compare agent changes* shows it and *Reject* restores the file's previous text. Or hide both
   lists in a real folder until then. Tests: a recorded stream with a to-do list and two edits.
3. **The local model for the three Claude-only tasks.** Commit message, Agent Review and Dissect
   Project ask the local model when Claude Code is not there (always on a phone), with the same
   prompts cut to fit its context. Tests: each with a fake local engine.
4. **Summarize file.** Either ask the model for each region's summary (the local model by default),
   or drop the AI icon and call it *Refresh summaries*.
5. **Inline completion** (plan 21 item 1): `completes = true` for the local provider behind a delay
   and a setting, faint inline text, Tab to accept.
6. **Plan 20, then plan 21 item 2** (judging requests, watching agents).
7. **Engines** (plan 23): Vulkan, then the climb, verify and drop-back.

## Tests to add with each step

- A connected feature has a test that reaches the service with a fake (CLI or engine) and checks
  the answer lands where the user sees it.
- A feature that is sample only has a test that a real folder does not show sample data (as
  `real_workspace_uses_real_agent_and_tools` does for the chat).
- No test starts the real agent CLI (it runs Claude Code on the user's account).
