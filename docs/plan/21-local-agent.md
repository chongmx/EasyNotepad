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
  - 2.3 B effective parameters, 5.1 B with its per-layer embeddings. The embeddings are only looked
    up, so they stay memory-mapped on the host.
  - The download is Google's own **quantization-aware-trained Q4_0 GGUF**, 3.35 GB. bf16 and fp16
    are only used in CI, as the reference. See [plan 23](23-local-inference-engines.md) for every
    file and why.
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

**Decided in [plan 23](23-local-inference-engines.md): not one runtime but a ladder of engines.**
- Every engine sits behind one small C interface.
- The agent starts on the most compatible one: ggml CPU (llama.cpp).
- It probes the device, acquires a faster engine (builds it locally when the tools are there,
  otherwise downloads a prebuilt one), verifies it against the CPU engine, and switches over. It
  drops back if the faster engine fails.
- Phase 1 has two families:
  - **ggml:** CPU, and Vulkan for any GPU;
  - **TensorRT:** fixed, NVIDIA RTX.
- Later rungs: Metal, CUDA, Android GPU, the NPUs (Qualcomm, Google Tensor, MediaTek, Samsung, AMD,
  Intel), and WebGPU.

**Platforms:**
- Windows, Linux and macOS desktops; Android (phones with 6 GB of memory or more).
- 32-bit Windows gets no local agent: the 3.35 GB model does not fit its address space.
- The web build gets one later, through WebGPU (plan 23, R7).
- Where there is no local agent, the features fall back to the remote agent or are hidden.

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
- **Code from comments is done** (FEATURES.md › IntelliSense › Code suggestions from comments):
  `CodeAssistant` asks the local model (or Claude, or any command, as the user chooses) for the code a
  comment describes. Inline completion while typing is still to do.
- **Built on IntelliSense** (`IntellisenseViewModel`, see FEATURES.md › IntelliSense): the model is
  a completion *provider*. It receives a `Request` (the text before and after the caret, the file and
  language, and the context snippets IntelliSense chose: declarations matching the typed word, those
  the nearby code uses, those of the file, ranked within a budget) and answers with `provide()`. A
  newer keystroke or a closed list drops older answers by serial. What context a model sees is
  decided in one place, `IntellisenseViewModel::contextFor()`; new kinds of snippets go there.

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
- **Engine:** automatic (climb the ladder) / pin a rung / CPU only, plus a thread count. Details in
  plan 23's Engine page.
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
