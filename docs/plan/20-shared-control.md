# 20 · Shared control: connected agents, acting agents and the task manager

**Problem.** The app is shared by its user and by agents working through the MCP connector. Today:
- There is one agent pointer, and nothing says whose it is.
- Any connected agent can act at any moment: two agents can click over each other, or click while
  the user types.
- There is no limit on how many agents connect.
- Each tool call is a fresh connection, so the app cannot tell agents apart.
- Agents reach only controls with an automation id. They cannot use arbitrary spots, drags,
  scrolling, or keys in any focused field.
- The agent in the app's own Agent panel (Claude Code) also works through the connector, so the
  user's own chat competes with outside agents for the same pointer.

## Goal

- A limited number of agents may connect. Default 16, set by the user.
- Every connected agent may **read** at any time.
- Only a few of them may **act**: the acting agents. Default 2, set by the user.
- The other connected agents **request** actions. They do not act themselves.
- One component, the **task manager**, alone drives the screen pointer and every action that
  changes something. Acting agents and requests all go through it, one action at a time.
- The user always comes first, and can see, pause, reorder, promote or stop agents at any time.

## Roles

| Role | How many | Read tools | Acting tools | Pointer |
|---|---|---|---|---|
| **User** | 1 | yes | yes | the real mouse |
| **Acting agent** | up to *acting limit* (default 2) | yes | submitted to the task manager, run in turn | its own, coloured, drawn by the app |
| **Connected agent** | up to *connection limit* (default 16) | yes | `request_action` only: queued until approved | none |
| Over the limit | — | refused: "EasyNotepad accepts N agents; disconnect one or raise the limit" | — | — |

- The Agent panel's own agent is always one of the acting agents while it runs. The user started
  it, so it never waits behind outside agents for a slot.
- Acting slots go first come, first served. The user can **promote** a connected agent (it takes a
  free slot or swaps with one) or **demote** an acting one.
- Both limits are in Preferences ▸ Agents. Lowering a limit demotes or disconnects the newest
  agents first, never the Agent panel's own.

## Design

**Who is who.**
- `EasyNotepad --mcp` (the bridge each agent starts) keeps one connection open to the app for its
  whole life, instead of one per tool call.
- It sends a hello with a session id, the agent's name (`clientInfo.name` from MCP `initialize`, or
  `EASYNOTEPAD_AGENT_NAME`) and its version. Every request after that is that session's.
- The app keeps a registry of sessions: name, colour, role, last heard from, current task, and
  recent actions.
- A closed connection, or 60 s of silence, ends the session: its slot frees, its pointer fades, its
  queued tasks are dropped.

**Tool classes.**
- **Read**, for every connected agent at any time, without queueing:
  - `get_state`, `list_elements`, `list_commands`, `read_*` (including `read_agent_chat`);
  - `git_status`, `git_diff`, `git_log`, `git_branches`;
  - `screenshot`, `agents_status`.
- **Act**, for acting agents, through the task manager:
  - `click`, `point`, `execute_command`, `type_text`, `open_file`, `run_in_terminal`, `send_keys`;
  - `ask_agent` and the git actions;
  - the pointer input below.
- **Request**, for connected agents: `request_action {tool, arguments, reason}`. The request is
  queued for the user (and later, plan 21, the local agent) to **approve once**, **approve for this
  session** or **decline**. An approved request runs through the task manager like any action.
- Calling an acting tool without an acting slot answers: "read-only: call `request_action`, or ask
  the user to promote you".

**The task manager.**
- It owns the action queue, the screen pointer and the single executor. Nothing else changes the UI
  on an agent's behalf.
- It runs one action at a time, on the UI thread, at frame boundaries. Each action moves the
  agent's own pointer, acts, and reports the result back to the agent.
- **Order:**
  - the user's own input first;
  - then the Agent panel's agent;
  - then acting agents, round robin, one action each, so neither starves;
  - then approved requests.
- **Turns:** an agent can ask for a short exclusive turn (`begin_turn` … `end_turn`, at most 30 s)
  for steps that must not be interleaved, such as typing into a field it just focused. Others wait
  meanwhile.
- **The user comes first:** while the user is using the mouse or keyboard in the app (and for 1.5 s
  after), queued actions wait, up to 5 s, then answer "the user is working".
- **Pause agents** stops the executor. Reading still works; queued actions wait.
- **Timeouts:** an action that does not complete within 10 s is cancelled and reported, and the
  executor moves on. One agent's failing actions cannot block the others.
- **Status:** `agents_status` gives the roles, the queue, your place in it, and whether the user is
  working or has paused agents.

**Pointers.**
- One per acting agent: drawn in the agent's colour, with a name tag and its message bubble.
- The OS cursor is never moved. Idle pointers fade after a few seconds and come back when the agent
  acts.

**Reaching the whole app** (acting tools, through the task manager).
- Pointer input at window coordinates, from `list_elements` rectangles or a screenshot:
  - `pointer_move {x, y}`;
  - `pointer_click {x, y, button, double}`;
  - `pointer_drag {from, to}`;
  - `scroll {x, y, lines}`;
  - `press_keys {keys}` and `type {text}`, both to the focused field.
- These are fed to ImGui's input queue over a few frames, never to the OS. The user's mouse position
  is restored right after.
- `screenshot` returns the window as a PNG, so an agent can see anything, labelled or not.

**What the user sees.**
- An **Agents** chip in the title bar: "2 acting · 5 connected · 1 request". It is in the colour of
  the agent acting now, and pulses when a request waits.
- Clicking it opens the **Agents** panel:
  - every session with its role, state, current task and last actions;
  - **Promote / Demote**, **Pause all / Resume**, **Disconnect**, **Block for this session**;
  - the requests, each with Approve once / Approve for session / Decline.
- An activity log keeps the last 200 actions, by agent.
- Status messages caused by an agent are prefixed with its name.

**Safety.**
- Confirmations that lose work are for the user only: discard, delete, force push, reset hard, drop
  stash, and close without saving. An agent clicking their confirm button gets "needs the user", and
  the dialog stays open for the user.
- The setting **Agents may confirm destructive actions** is off by default.
- **Esc pressed twice quickly** (or Pause on the chip) stops every agent at once.

## Settings (Preferences ▸ Agents)

- **Connected agents**, 1–64, default 16.
- **Acting agents**, 1–4, default 2.
- **Requests:** ask me (default) / approve automatically for promoted agents. Plan 21 adds "let the
  local agent decide simple ones".
- **Agents may confirm destructive actions**, off.
- **User idle before agents act**, default 1.5 s.

## Tests

**Unit:**
- **Registry:**
  - the connection limit;
  - roles assigned first come, first served;
  - promote and demote;
  - lowering a limit demotes the newest first and never the panel's agent;
  - silence ends a session.
- **Tool classes:**
  - reads pass for every role;
  - acting tools are refused for connected agents;
  - `request_action` queues.
- **Task manager:**
  - one action at a time;
  - the order (user, panel agent, round robin, requests);
  - exclusive turns and their limit;
  - user activity defers actions;
  - pause;
  - an action that times out is cancelled without blocking the queue.

**Integration (real bridges):**
- 17 bridges: the 17th is refused.
- Three bridges: two act, the third's acting call is refused, and its request runs once approved.
- A pointer click at coordinates presses a button with no automation id; a drag moves a splitter.
- A destructive confirm clicked by an agent answers "needs the user".

**UI:**
- The sweep with two acting agents and a waiting request.
- The chip and the Agents panel.
- A screenshot with two coloured pointers.
