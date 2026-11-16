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

## Execution: hardware engines as run-time libraries (from 16 Nov 2026)

**Today.** llama.cpp is linked into the app statically, CPU only (AVX2 on PCs, ARMv8.2 dot product on
phones), and the model runs inside the app's own process. On this PC (RTX 5060 Laptop, 8 GB, sm_120)
that is about 20 tokens/s; the spike measured 116 with ggml CUDA (plan 23).

**Aim.** Each accelerator is a library loaded at run time: **TensorRT for NVIDIA RTX first, then the
Qualcomm Hexagon NPU**. The CPU engine stays built in. **When a library is absent, or present but
unusable, the local agent runs on the CPU**, as it does today: no error dialog, only the reason, shown
in the Engine Lab and the status bar ("Local agent: on the CPU; TensorRT is not installed").

**Done first (16 Nov 2026): ggml CUDA as the first library**, ahead of TensorRT, because it reads
the same GGUF as the CPU and was already measured on this PC:
- `include/enp/engine/EnpEngine.h`: the C interface (ABI 1; JSON requests and results).
- `src/engine/LlamaRunner.cpp`: the model code, shared by the CPU engine in the app and the library.
- `engines/cuda/`: `enp-engine-cuda` (`.dll` / `.so`), its own CMake project (nvcc needs MSVC on
  Windows), built by CI's `engine-cuda` job for Windows x64 (with the cuBLAS DLLs) and Linux x64.
- `os::lib` (load a library), `os::process::InputStream` (a program fed while it runs).
- `EngineHelper` and `EasyNotepad --local-engine <library>`: the helper process (part of 21.E2: no
  watchdog of token rates yet, but a helper silent for 3 minutes, or not stopping 15 s after a
  cancel, is ended).
- `LocalEngineService`: looks for the library next to the executable; uses it for chats and for
  the Engine Lab's GPU runs; falls back to the CPU in every case of the table below, and keeps
  using the CPU until the app restarts. Tests: `engine_library_answers_and_anything_wrong_falls_back_to_the_cpu`
  with a fake library (`tests/FakeEngine.cpp`) that answers, fails, crashes, hangs or will not start.
- Measured on this PC (WSL, CUDA 13.2, sm_120, through the helper): 105 tok/s writing, 185 with
  the drafter (61% of drafts accepted), prompt 7,417 tok/s; the CPU 19 tok/s. A chat answer: 198
  tok/s. The library is 635 MB on Linux (cuBLAS linked in; it needs only the NVIDIA driver); on
  Windows cuBLAS's DLLs sit beside it.

**Shape.**
```
 EasyNotepad (UI)                   EasyNotepad --local-agent (helper)
 LocalAgentService ──JSON lines──►  EngineHost
                                      ├─ built-in CPU engine (llama.cpp, static)  ◄── always there
                                      └─ os::lib::open("<appdata>/local-agent/engines/<rung>/enp-engine-<rung>.dll|so")
                                           enp_engine_abi / describe / open / generate / score / close
```
- One small C interface for every engine (plan 23, `enp_engine.h`), so a library built with MSVC and
  the CUDA/TensorRT SDK loads into the MinGW-built app, and memory never crosses it.
- Loading goes through the OS layer: `LoadLibraryExW` with the library's own folder on the search
  path (its `nvinfer`, `cudart` beside it) on Windows, `dlopen(RTLD_NOW | RTLD_LOCAL)` elsewhere.
- Libraries load only in the helper process: a driver fault kills the helper, not the editor, and
  the helper's exit gives back everything the vendor runtime held (CUDA does not unload cleanly).

**When the CPU is used instead** (each is a test with a fake engine library):

| Case | Result |
|---|---|
| No library in the engine folder | CPU; "not installed" |
| Library fails to load (a missing dependency DLL, wrong architecture) | CPU; the loader's message |
| `enp_engine_abi()` differs, or an export is missing | CPU; "built for another version" |
| `enp_engine_open` fails: no NVIDIA GPU, driver too old, too little VRAM, no Hexagon DSP | CPU; the engine's `last_error` |
| The engine for this GPU and driver cannot be built (TensorRT) | CPU; the build log kept |
| Verify fails: wrong answers, or not 20% faster than the CPU | CPU; the measurements |
| Later: two crashes in 10 minutes, NaN output, a hang | back to the CPU, marked bad for this fingerprint until *Try again* |

**Steps.**

| # | Step | Done when |
|---|---|---|
| 21.E1 | **The seam.** `enp_engine.h` (ABI 1); the built-in CPU engine behind it (today's `LocalEngineService` code, unchanged in behaviour); `os::lib` (`open`, `symbol`, `close`, last error) for Windows, POSIX and Android; `EngineHost` picks a library or the CPU by the table above | unit tests with fake libraries built by the test build: good, missing, wrong ABI, missing export, failing open, crashing open; each ends on the CPU with its reason |
| 21.E2 | **The helper process.** `EasyNotepad --local-agent` hosts `EngineHost`; `LocalAgentService` speaks JSON lines to it (requests, streamed text, cancel); the watchdog (no token for 10 s), restarts, three faults in five minutes turn it off. Android: a second service process | the safety-net tests of this plan with a fake helper; the memory cycle shows the helper's memory gone after an idle unload |
| 21.E3 | **Probe, small first.** NVIDIA through `nvml` loaded at run time (name, compute capability, VRAM, driver); Qualcomm through the SoC model and the Hexagon version; the fingerprint (GPU, driver, OS build). Writes `environment.json` | this PC reports the RTX 5060, sm_120, 8 GB, the driver; a phone reports its Snapdragon and Hexagon version; no NVIDIA or Qualcomm means no rung, not an error |
| 21.E4 | **TensorRT spike: an INT4 model that fits 8 GB.** fp16 does not fit (plan 23 spike: 11 GB working set, 16 tok/s). First try, CPU only: repack Google's QAT `w4a16` checkpoint (compressed-tensors) into the AWQ / GPTQ layout TensorRT-Edge-LLM reads; the weights are already 4-bit, so nothing is re-quantized. Else quantize once with ModelOpt on a rented 16–24 GB GPU and publish the checkpoint. Also answers: does Edge-LLM's C++ runtime build with MSVC on Windows, or does Windows need TensorRT-RTX | an INT4 engine runs on this PC under 8 GB with no shared-memory spill; its speed and its agreement with the CPU written into plan 23 |
| 21.E5 | **`enp-engine-tensorrt`.** The library: Edge-LLM's runtime for the LLM part only (no vision or audio), Gemma 4's per-layer embeddings kept on the host, our sampler and stop rules; built by MSVC (Windows x64) and GCC (Linux x64) in CI into a pack: the library, the TensorRT runtime it needs, a manifest with SHA-256s. Downloaded from Preferences ▸ Local agent ▸ Engine like the model, never inside the app | the pack downloads, checks and loads on this PC; with the pack deleted, the agent answers on the CPU |
| 21.E6 | **Engine built on the device.** The first open builds the TensorRT engine for this GPU (batch 1, context 4,096), cancellable, with progress; kept under `engines/tensorrt/<fingerprint>/`; a new driver or GPU builds again, and a failed build leaves the CPU in use | built once, then cached starts; a faked fingerprint change rebuilds or falls back |
| 21.E7 | **Verify and switch.** Teacher-forced agreement with the CPU engine on the golden texts (top-1 ≥ 92%, mean KL < 0.05, no NaN; the weights differ), the task checks, the speed rule (≥ 20% faster at writing), 2 minutes of mixed load; then new requests go to it and the CPU stays the fallback | on this PC TensorRT is chosen; a fake engine that is wrong, slow or crashes never is, or is dropped |
| 21.E8 | **Shown to the user.** Preferences ▸ Local agent ▸ Engine: automatic / pin / CPU only; what each rung needs, whether it is installed, its measurements, why it is or is not used; *Try again*. The Engine Lab runs any installed engine | UI tests for each state, on desktop and phone layouts |
| 21.E9 | **Hexagon (Android first).** `enp-engine-hexagon`: llama.cpp with its Hexagon backend (experimental upstream; ops it lacks run on the CPU), built in CI with Qualcomm's Snapdragon toolchain image. **Shipped inside the APK** (Google Play does not allow downloading native code), with the HTP libraries for v73, v75, v79 and v81; no FastRPC, an older DSP or a failed open means the CPU. Reads the same GGUF, so there is nothing to prepare. Measured on the S24 Ultra (Snapdragon 8 Gen 3, v75) against its 30 tok/s on the CPU; kept only if verify passes and it is faster or saves power | chosen on a Snapdragon phone that gains; the CPU everywhere else, including phones without Hexagon |
| 21.E10 | Later: Windows on Snapdragon (Hexagon needs a signed driver package there), ggml CUDA and Vulkan as further rungs on the same seam (plan 23) | — |

**Risks.**
- **The INT4 model (21.E4)** decides whether TensorRT is worth it on an 8 GB card. If neither
  route works, ggml CUDA is the NVIDIA rung on the same seam (measured 116 tok/s, 165 with the
  drafter on this GPU, and it reads the GGUF the CPU uses, so verify is simple).
- **TensorRT on Windows**: the spike ran Edge-LLM in WSL. If its runtime does not build with MSVC,
  the Windows library uses TensorRT-RTX directly.
- **Size**: the TensorRT runtime is hundreds of megabytes, so it is an optional download with the
  engine, like the model.
- **Hexagon is experimental upstream**: Gemma 4 E2B may run only partly on the DSP. Measured, not
  assumed.

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
