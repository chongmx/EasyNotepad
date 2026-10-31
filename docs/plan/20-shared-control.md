# 20 · Shared control: agent pointers and taking turns

**Problem.** The app is meant to be shared by its user and any number of agents, all working through
the MCP connector. Today:
- There is one agent pointer, and nothing says whose it is.
- Any connected agent can act at any moment: two agents can click over each other, or click while the
  user types.
- Agents reach only controls with an automation id. They cannot use arbitrary spots, drags, scrolling,
  or keys in any focused field.
- Each tool call is a fresh connection, so the app cannot tell agents apart.

## Goal

- Every agent has its own pointer, never the user's mouse, and its own name and colour.
- Agents can do everything the user can.
- Only one agent acts at a time. The others wait in a visible queue.
- The user always comes first, and can see, pause, reorder or stop the agents at any time.

## Design

**Who is who.**
- `EasyNotepad --mcp` (the bridge each agent starts) makes a session id when it starts. It takes the
  agent's name from MCP `initialize` (`clientInfo.name`, or `EASYNOTEPAD_AGENT_NAME`), and every
  request it forwards carries `agent: {id, name}`.
- The app keeps a registry of agents: name, a colour picked for it, when it was last heard from, its
  state (acting, waiting, idle), and its recent actions.
- An agent silent for 60 s is "away". Its pointer fades, and it leaves the queue.

**Pointers.**
- One per agent: drawn by the app in the agent's colour, with a name tag and its message bubble.
- The OS cursor is never moved. Idle pointers fade after a few seconds and come back when the agent
  acts.

**The floor (taking turns).**
- **Free tools**, for any agent at any time: those that only look, namely `get_state`,
  `list_elements`, `read_*`, `git_status`/`git_diff`/`git_log`/`git_branches`, `list_commands`,
  `screenshot` and `control_status`.
- **Acting tools need the floor:** `click`, `execute_command`, `type_text`, `open_file`,
  `run_in_terminal`, `send_keys`, the git actions, and the pointer input below.
- `control_status` gives the holder, the queue, your place in it, and whether the user is working.
- `request_control` (with an optional `reason`, shown to the user) answers **granted**, or
  **queued #n**. `release_control` hands the floor to the next agent.
- **Convenience:** acting while nobody holds the floor takes it automatically. Acting while another
  agent holds it fails with "*X* is working; call request_control to queue".
- **Fairness:**
  - The holder keeps the floor while active.
  - After 20 s without an action, it is released.
  - When others are waiting, a turn lasts at most 2 min. The current action finishes; the next
    acting call answers "your turn ended" and re-queues.
- **The user comes first:**
  - While the user is using the mouse or keyboard in the app (and for 1.5 s after), agents' acting
    calls wait, up to 5 s, then answer "the user is working".
  - **Pause agents** freezes the floor: reading still works, acting does not.

**Reaching the whole app.**
- Pointer input at window coordinates (from `list_elements` rects or a screenshot):
  - `pointer_move {x, y}`;
  - `pointer_click {x, y, button, double}`;
  - `pointer_drag {from, to}`;
  - `scroll {x, y, lines}`;
  - `press_keys {keys}` and `type {text}`, both to the focused field.
- How it works:
  - These are fed to ImGui's input queue, not the OS, over a few frames, and only while the user's
    own mouse and keyboard are idle.
  - The user's mouse position is put back right after.
  - The agent's pointer shows the move and the click.
- `screenshot` returns the window as a PNG, so an agent can see anything, labelled or not.

**What the user sees.**
- An **Agents** chip in the title bar shows who has the floor, in that agent's colour, and how many
  wait: "claude-code is working · 1 waiting".
- Clicking it opens a list of agents, each with its state, its reason, and its last actions. From
  there the user can:
  - **Pause all** / **Resume**;
  - **Take back** the floor;
  - **Let *X* go next**;
  - **Block** an agent for this session.
- An activity log keeps the last 200 actions, by agent.
- Status messages caused by an agent are prefixed with its name.

**Safety.**
- Confirmations that lose work are for the user only: discard, delete, force push, reset hard, drop
  stash, and close without saving. When an agent clicks their confirm button, the click answers
  "needs the user", and the dialog stays open for the user.
- A setting, **Agents may confirm destructive actions**, is off by default.
- **Esc pressed twice quickly** (or the chip's Pause) stops every agent at once.

## Tests

**Unit:**
- Floor state machine:
  - grant when free;
  - queue order;
  - release hands over;
  - idle release;
  - turn limit with waiters;
  - pause;
  - away agents leave the queue;
  - user activity defers acting calls.
- Free tools pass without the floor; acting tools are refused while another agent holds it.

**Integration:**
- Two bridges with different names: the second queues, then gets the floor when the first releases.
- A pointer click at coordinates presses a button that has no automation id.
- A drag moves a splitter.
- A destructive confirm clicked by an agent answers "needs the user".

**UI:** the sweep with two agents, one acting and one waiting; the chip and its list; a screenshot
with two coloured pointers.
