# 21 · A local agent on the device (Gemma 4 E2B)

**Problem.**
- Every AI feature needs a remote agent: Claude Code, a network connection and a paid account.
  Simple things wait seconds and cost tokens. For example: finish this line, name this commit, may
  this agent click that button?
- The task manager (plan 20) can queue and order agents, but it cannot judge anything. It cannot
  say whether a request is safe, whether a remote agent is stuck, or whether a task is small enough
  to do at once.
- Without a network, there is no AI at all.

## Goal

A small model runs inside the app, on the device, as the **local agent**:
1. **Code completion:** inline, as you type, in a fraction of a second, offline.
2. **Agent management:** it helps the task manager judge requests and watch the other agents.
3. **Small tasks itself:** commit messages, short explanations, renames, quick answers.
4. **Escalation:** it hands anything bigger to a remote agent (Claude Code), with a prepared brief,
   and keeps the user informed.

A local **safety net** catches the local agent's own faults: hangs, runaway output, crashes, memory
exhaustion. Nothing it does can freeze or crash the editor.

## The model

- **Gemma 4 E2B** (Google, April 2026, Apache 2.0, so it may be shipped, including commercially).
  - About 2.3 B effective parameters.
  - About 2.6 GB as Google's on-device LiteRT-LM build, about 1.3 GB quantized to 4 bits.
  - Text in, text out. Its image and audio input is not needed at first.
- **Not inside the exe.** It is an optional download, made on first use from Preferences ▸ Local
  agent, with:
  - progress, resume, and a checksum check;
  - storage in the app's data folder;
  - a "remove" button.
  The app stays about 8 MB and works fully without it.
- **Model files are swappable.** Gemma 4 E4B for stronger machines, or a smaller model for weak
  ones. The code depends on an interface, not a model.

## Runtime

- **One choice, made early by a spike.** Both candidates are permissively licensed and C/C++
  embeddable:
  - **LiteRT-LM:** Google's runtime for exactly these models, with CPU, GPU and Android NPU.
  - **llama.cpp:** GGUF models, with CPU (AVX2 / NEON), Vulkan, Metal and CUDA. Mature
    fill-in-the-middle support for completion.
- **The spike measures, on this PC and a mid-range Android phone:**
  - time to first token;
  - tokens per second;
  - memory;
  - completion quality on our own code;
  - build size added;
  - how cleanly the runtime builds with MSVC, MinGW (x64 and x86), clang (Linux, macOS) and the NDK.
- **Platforms:** Windows, Linux and macOS desktops; Android (phones with 6 GB of memory or more).
  - The web build and 32-bit Windows get no local agent: not enough memory, and no SIMD worth having.
  - There, the features fall back to the remote agent or are hidden.

## Design

**Process isolation (the core of the safety net).**
- The model runs in a separate helper process, `EasyNotepad --local-agent`: the same exe in another
  mode, like `--mcp`.
- It talks to the app over a pipe, with JSON lines: requests, streamed tokens, cancel.
- If the helper hangs, runs out of memory or crashes, the app kills it and starts a new one. The
  editor never notices beyond a status message.
- On Android it is a separate process too (a second Android service).

**Service.** `ILocalAgentService`:
- `complete(prefix, suffix, language)` → a stream of tokens;
- `chat(messages, tools)` → a stream of tokens;
- `classify(request)` → `{risk, size, decision}`;
- `cancel(id)`.

`LocalAgentService` implements it over the helper process, like `AgentCliService` does over
`claude`.

**1. Code completion.**
- After a short pause in typing (about 250 ms), the editor sends the text around the caret: up to
  about 1,500 tokens before and 300 after, plus the file name and language.
- The suggestion shows as faint inline text: **Tab** accepts it, **Esc** or typing on dismisses it.
  It is single-line at first, multi-line later.
- A new keystroke cancels the request in flight, so only the newest one ever runs.
- Off by default per language until measured. On/off and the delay are in Preferences.

**2. Agent management** (plugs into plan 20's task manager).
- **Requests:** with "let the local agent decide simple ones" on, it classifies each
  `request_action`:
  - *read-like* or *harmless* (open a file, show a panel, point at something): approved;
  - *changes text or files*: asks the user;
  - *destructive*: always asks the user.
  Every decision is shown in the Agents panel with its reason, and the user can overrule it.
- **Watching agents:**
  - it notices a remote agent that loops (the same failing call again and again), stalls (no
    progress for minutes), or drifts (acting outside the task it stated);
  - it pauses that agent and tells the user, in plain words, what it saw.
  These are rules first: counters and timers. The model only adds the explanation.
- **Routing:** the Agent panel prompt goes to the local agent first, which decides whether it is:
  - **small**: it answers itself;
  - **big**: it escalates to the remote agent.
  The user can always force either: **Local** / **Claude** / **Auto** (default) in the panel.

**3. Small tasks itself:**
- the commit message (today a remote call or a rule-based guess);
- explaining a selection briefly;
- a function or file summary for the fold summaries and the logical map;
- short questions about the open file.

**4. Escalation.**
- When a task is beyond it, the local agent writes a brief: the goal, the files involved, and what
  it already found.
- It starts the remote agent with that brief. The user sees "Asked Claude Code: …".
- The remote agent's answer streams into the same chat. The local agent keeps managing (watching,
  approving simple requests) while the remote one works.
- **Signals for "too big":**
  - more than one file to change;
  - unfamiliar APIs;
  - the user asked for a plan;
  - the local model's own low confidence;
  - a failed first attempt.

**The safety net (local, rule-based, always on).**
- **Watchdog:**
  - no token for 10 s while generating means a hang: kill and restart the helper, and answer
    "the local agent stopped responding";
  - three restarts in five minutes disables the local agent until the user turns it back on.
- **Budgets:** a cap on tokens per answer and on wall time per task. Completion gets about 1 s and
  64 tokens; chat gets about 60 s and 1,024 tokens.
- **Loops:** the same 40-token stretch repeated three times stops generation.
- **Memory:** the helper's memory is watched. Going over its budget (by default half of the free
  memory at start) restarts it with a smaller context.
- **Output checks:**
  - a completion must be valid UTF-8 and must not repeat the text after the caret;
  - a tool call must name a real tool with arguments that parse.
  Anything else is dropped, never applied.
- **Thermal and battery (Android):** completion pauses on battery saver or when the phone is hot.
- Every intervention is logged and shown briefly in the status bar. Nothing is silent.

## Settings (Preferences ▸ Local agent)

- **Model:** none (default) / Gemma 4 E2B / Gemma 4 E4B, with download, remove and the size on disk.
- **Use for:**
  - code completion (and which languages);
  - commit messages and summaries;
  - deciding simple agent requests;
  - first answer in the Agent panel.
- **Hardware:** automatic / CPU only / GPU, plus a thread count.
- **Budgets:** the watchdog and token limits, with sensible defaults; most users never touch them.

## Tests

**Unit:**
- Routing (small vs big) on a fixed set of prompts.
- The request classifier on a fixed set of requests: harmless, changes and destructive.
- Output checks.
- The loop detector.
- Budgets.

**Safety net, with a fake helper that hangs, floods, loops, crashes or eats memory on command:**
- each fault is caught;
- the editor stays responsive throughout (frame times checked);
- the helper restarts;
- three faults disable it.

**Integration (real model, run on demand, not in every CI run):**
- Completion latency and acceptance on a sample project.
- A small task done locally.
- A big one escalated with a sensible brief.

**UI:**
- The sweep with inline completion showing.
- The Local / Claude / Auto switch.
- The download dialog.
