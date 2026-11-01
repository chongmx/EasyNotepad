# 23 · Local inference engines: start anywhere, then climb to the fastest one the device has

**Problem.** Plan 21 puts a small model (Gemma 4 E2B) inside the app. How fast it runs depends almost
entirely on the hardware it reaches. The range is wide:
- a plain CPU gives a few tokens a second;
- a desktop GPU or a phone NPU gives tens to hundreds.

The ways to reach that hardware are many, and each is tied to a vendor, an OS, a driver and an SDK.
No single build can carry them all. They are hundreds of megabytes each, many need the exact
driver, and some (TensorRT engines) must be built on the very machine that runs them.

## Goal

1. **Works everywhere first.** On first use, the local agent runs on the most compatible engine
   there is: the CPU. Nothing to install beyond the model.
2. **Then it looks around.** It scans the device: CPU features, memory, GPUs, NPUs, drivers, and any
   SDKs or compilers already installed.
3. **Then it climbs.** It picks the next faster engine the device supports. It builds that engine
   on the machine when the tools are there, or downloads a prebuilt one when not. It prepares the
   model for it (exports and compiles where the engine needs it), and checks it against the CPU
   engine. Only then does it switch over, without interrupting the user.
4. **Never worse.** If the faster engine is wrong, slower, unstable or breaks after a driver update,
   the agent drops back one rung, automatically, and says why.
5. **Start simple.** The first version has exactly two engine families:
   - **ggml (llama.cpp):** the CPU bootstrap and the cross-platform GPU (Vulkan);
   - **TensorRT:** a fixed NVIDIA path.
   Everything else is a later rung on the same ladder.

## Hardware map: what exists, and how it is reached

| Hardware | Where | Runtime(s) that reach it | Gemma 4 E2B status (Nov 2026) | Rung |
|---|---|---|---|---|
| **Any CPU** (x86-64 SSE→AVX-512, ARM64 NEON/SVE/i8mm) | everywhere | ggml CPU, with runtime-selected variants | yes | **R0 (phase 1)** |
| **Any Vulkan GPU**: NVIDIA, AMD, Intel, Qualcomm Adreno, Arm Mali, **Samsung Xclipse** (Exynos), PowerVR | Windows, Linux, Android | ggml Vulkan | yes | **R1 (phase 1)** |
| **NVIDIA RTX 30xx and newer** | Windows x64, Linux x64 | **TensorRT for RTX** through **TensorRT-Edge-LLM** (Apache 2.0, C++ runtime, Gemma 4 E2B since 0.9; Windows x64 with TensorRT-RTX since 0.11) | yes | **R2 (phase 1)** |
| NVIDIA older or non-RTX (GTX 10/16, Turing T-series) | Windows, Linux | ggml CUDA | yes | R3 |
| Apple GPU (M-series, A-series) | macOS, iOS | ggml Metal; MLX (4-bit MLX builds exist) | yes | R3 |
| Qualcomm Adreno GPU | Android, Windows on Snapdragon | ggml OpenCL (Adreno-tuned); LiteRT-LM GPU | yes (LiteRT-LM reports about 52 tok/s on Android GPU) | R4 |
| Mali / Xclipse / other phone GPUs | Android | Vulkan (R1); LiteRT-LM GPU (OpenCL) | yes | R4 |
| **Qualcomm Hexagon NPU** | Android (8 Gen 3 and newer), Windows on Snapdragon X | ggml Hexagon (experimental, v73–v81); LiteRT-LM QNN (per-SoC model files); ONNX Runtime QNN | partly (LiteRT-LM file for QCS8275 only; ggml Hexagon is experimental) | R5 |
| Google Tensor NPU (G5, G6) | Pixel | LiteRT-LM NPU (per-SoC model files exist) | yes | R5 |
| MediaTek APU | Android | LiteRT-LM NeuroPilot | no Gemma 4 build seen | R5 |
| **Samsung Exynos NPU** | Galaxy (Exynos models) | LiteRT "Exynos AI LiteCore" | **not confirmed** for LiteRT-LM or Gemma 4; use Xclipse via Vulkan meanwhile | R5 (research) |
| **AMD Ryzen AI NPU** (XDNA 2) | Windows; Linux on kernel 7.0 or the amdxdna backport | FastFlowLM (an NPU-first runtime of about 16 MB; a Gemma 4 E2B NPU model is published); Ryzen AI ONNX Runtime GenAI (VitisAI) | yes (FastFlowLM) — **licence to check** | R5 |
| Intel NPU / iGPU (Lunar Lake, Panther Lake) | Windows, Linux | OpenVINO GenAI; LiteRT-LM (Intel LNL/PTL model files exist); ggml Vulkan or SYCL for the iGPU | yes | R5 |
| AMD GPU (compute) | Linux, Windows | ROCm/HIP (ggml HIP); Vulkan already covers it | yes | R6 (only if clearly faster than R1) |
| **Any NPU or GPU on Windows 11 24H2+** | Windows | **Windows ML**: downloads the vendor's ONNX Runtime provider on demand (NvTensorRtRtx, OpenVINO, QNN, VitisAI) | through ONNX builds (onnx-community) | R5 alternative: one integration for four vendors |
| Browser GPU | web build | **WebGPU**: ggml WebGPU backend (2026 paper: +45–69% decode), wllama, LiteRT.js (a `web.litertlm` file exists), ONNX Runtime Web | yes | R7 |
| WebGL | web build | — | **not viable** | none |

**On WebGL:** WebGL 2 has no compute shaders and no storage buffers. An LLM on it must pack weights
into textures and run as fragment shaders, which every serious runtime has abandoned. **WebGPU is the
web's GPU rung.** Where WebGPU is missing, the web build falls back to the WASM CPU engine (R0 built
by Emscripten), or to the remote agent.

**On "build it locally":** a typical machine has none of the needed tools, and phones cannot build
at all. So "acquire an engine" has two equal routes driven by the same recipe:
- **build here**, when the probe finds the tools;
- otherwise **download** what our CI built from that recipe.

This PC is a developer machine and shows a third case:
- **Windows side:** an RTX 5060 Laptop GPU (Blackwell, compute 12.0, 8 GB, driver 616.92), an Intel
  iGPU, 15.6 GB of memory and w64devkit. No CUDA toolkit, no Vulkan SDK, no MSVC.
- **WSL (Ubuntu 24.04):**
  - **CUDA 13.2** (`/usr/local/cuda*/bin`), gcc 13.3, CMake 3.28 and the Vulkan headers;
  - the GPU is visible through the Windows driver;
  - 11 GB of memory, 839 GB of disk;
  - no `glslc`, Ninja, TensorRT or PyTorch.

**The WSL route (Windows only).** A library built in WSL is a Linux `.so`, so the Windows app cannot
load it. Nor can WSL's `nvcc` build Windows code: on Windows, `nvcc` needs MSVC as its host
compiler. But the local agent already runs in a *separate helper process* that talks over a pipe.
So on Windows the helper itself may be a **Linux build running in WSL**: the app starts
`wsl.exe -e <path>/easynotepad-local-agent`, and the JSON-lines protocol runs over its stdin and
stdout, unchanged.
- **Probe:** checks `wsl.exe --status`, then inside WSL runs `nvidia-smi`, and looks for
  `/usr/local/cuda*/bin/nvcc`, `gcc`, `cmake` and free memory.
- **Model location:** the model is kept on WSL's own disk (`~/.local/share/easynotepad/`), not read
  through `/mnt/c`, which is far too slow for memory-mapping 3 GB.
- **What it opens up:**
  - **R3 ggml CUDA built here**, with the user's own CUDA toolkit and `-DCMAKE_CUDA_ARCHITECTURES=native`;
  - the **TensorRT export** (R2's CI step) on a maintainer's machine.
- **Costs:**
  - WSL's memory limit: 11 GB here, raised in `.wslconfig` if needed;
  - a slower first start (WSL boot, about 1–2 s).

  Verify measures both, so the WSL route is used only when it wins.
- **Not useful:** WSL's own Vulkan (Mesa "dzn", over Direct3D 12). It is slower than Windows' native
  Vulkan, so R1 always runs on the Windows side. TensorRT is the exception that helps: its engine *must* be compiled on the device, and
TensorRT for RTX does that with only the driver, no CUDA toolkit and no compiler.

## The model files: what we ship and where from

All weights are **Gemma 4 E2B instruction-tuned, Apache 2.0, ungated** on Hugging Face. Google
publishes **quantization-aware-trained (QAT)** versions: trained to keep near-bf16 quality at 4 bits.
They are the right 4-bit choice over community re-quantizations.

| Use | File (Hugging Face) | Size | Notes |
|---|---|---|---|
| **Bootstrap and every ggml rung** | ours: made from `google/gemma-4-E2B-it-qat-q4_0-gguf`, with the embedding tables at Q4_K | **2.64 GB** (Google's file: 3.35 GB) | Official QAT blocks at Q4_0, kept exactly. Q4_0 is also the format every ggml backend accelerates best (Vulkan, Metal, CUDA, Adreno OpenCL, Hexagon). The tables are smaller with unchanged perplexity (see the spike results). **This is the one download**, together with the drafter. |
| **Drafter (in the same pack)** | ours: `google/gemma-4-E2B-it-qat-q4_0-unquantized-assistant`, converted and quantized to Q4_0 | 77 MB | Google's QAT MTP drafter: +10–84% decode speed. |
| Low-memory phones (later) | `unsloth/gemma-4-E2B-it-qat-GGUF` / `-qat-mobile-GGUF` → `UD-Q2_K_XL` | 2.19 GB | Smaller, lower quality; offered only when memory requires it. |
| **TensorRT source** (CI only) | `google/gemma-4-E2B-it-qat-q4_0-unquantized` | about 10 GB | Google: "half-precision weights extracted from the QAT pipeline … for custom downstream compilation". CI exports INT4 ONNX from these. |
| TensorRT alternative source (CI) | `google/gemma-4-E2B-it-qat-w4a16-ct` | — | The same QAT, as compressed-tensors with 4-bit weights and 16-bit activations; try it first if Edge-LLM imports it directly. |
| **Reference** (CI and tests only) | `google/gemma-4-E2B-it` (bf16 safetensors), `ggml-org/gemma-4-E2B-it-GGUF` → `BF16.gguf` | 9.3 GB | Golden outputs and quality checks. **Never shipped to users.** |
| (not used) | `mtp-…Q4_0.gguf` (ggml-org), `google/gemma-4-E2B-it-assistant` | 59 MB | Drafters made from the non-QAT model. A drafter must match the target's QAT precision, so we use the QAT drafter above. |
| Android NPU / GPU (later) | `litert-community/gemma-4-E2B-it-litert-lm` | 2.0–3.3 GB | GPU, web, Tensor G5/G6, Qualcomm QCS8275, Intel LNL/PTL builds. |
| ONNX paths (later) | `onnx-community/gemma-4-E2B-it-ONNX` (q4f16) | about 3.1 GB | Windows ML / ONNX Runtime Web. |
| AMD NPU (later) | `FastFlowLM/Gemma4-E2B-IT-NPU2` | — | XDNA 2 only. |

**bf16 and fp16 are not shipped:**
- the bf16 GGUF is 9.3 GB;
- the fp16 per-layer embedding table alone is 4.7 GB.

They exist only in CI, as the reference and the export source. An fp16 TensorRT engine is a later
option for GPUs with 12 GB or more.

**Why 3.35 GB and not "1.3 GB" (plan 21 had this wrong).** E2B has 2.3 B *effective* parameters,
but 5.1 B with its per-layer embeddings (PLE). The PLE tables are large but are only *looked up*,
one row per token per layer, never multiplied:
- **ggml:** the file is memory-mapped, so the tables stay in pages the OS loads on demand;
- **GPU engines:** the tables stay in host memory, and only the 2.3 B compute weights (about
  1.2–1.5 GB at 4 bits) plus the KV cache go to VRAM.

That is why it fits a 6 GB phone and an 8 GB GPU. **Spike question for R2:** does TensorRT-Edge-LLM
keep PLE on the host, or put it in VRAM?

**On Android from Google Play, the weights come in a Play Asset Delivery pack** instead of a
download:
- A *fast-follow* pack installs right after the app, and Play handles the transfer, resuming and
  storage.
- The pack is unpacked to plain files, so llama.cpp memory-maps it like any other file.
  *Install-time* packs stay inside the APK, where a 3 GB file cannot be mapped.
- A pack holds at most 1.5 GB, so the model is split with llama.cpp's `gguf-split` into parts
  (`…-00001-of-00002.gguf`), which llama.cpp loads as one model. The 2.64 GB file needs two packs;
  the drafter goes into the first.
- CI builds the AAB with the packs.

Sideloaded APKs (testing) and desktops have no asset packs, and keep the in-app download below.

**Downloads** come from Hugging Face directly, pinned by **repository commit and SHA-256** in our
engine catalogue. Nothing is fetched by name alone, and nothing that is not in the catalogue is
fetched at all. Downloads resume, and the user can cancel or remove them.

## Workflow: bootstrap → probe → choose → acquire → prepare → verify → switch → watch

```
 first use                                                        later, in the background
 ─────────                                                        ────────────────────────
 [download model + R0]──►[R0 running: agent usable, slow]──►[probe]──►[choose rung]──(user OK)──┐
                                     ▲                                                          ▼
                                     │ drop back           [switch]◄──[verify vs R0]◄──[prepare model]◄──[acquire engine]
                                     └──────[watch]◄───────┘    (build here | download prebuilt)
```

1. **Bootstrap.** Preferences ▸ Local agent ▸ Download. The pack holds the Q4_0 GGUF, plus the R0
   engine library for this OS and architecture (a few MB). The exe itself stays about 8 MB. The
   helper process (`EasyNotepad --local-agent`, plan 21) loads R0, and the agent is usable at once.
   R0 is ggml CPU built with *all CPU variants*: one library carries SSE4.2, AVX2, AVX-512 and
   ARM i8mm code, and ggml picks the best one at load. So R0 is already tuned to this CPU, without
   compiling anything.
2. **Probe** (deterministic code, no model; under a second). It writes
   `<appdata>/local-agent/environment.json` with:
   - OS, its version and architecture; CPU model and features; total and free memory; disk free;
     power source and battery saver.
   - **GPUs:**
     - Windows: DXGI / DXCore adapters (DXCore also lists NPUs), with driver version and dedicated
       memory;
     - Vulkan: the loader, `vkEnumeratePhysicalDevices`, device type, API version and memory heaps;
     - NVIDIA: `nvml` loaded at run time for compute capability and VRAM;
     - Linux: `/sys/class/drm`;
     - Android: Vulkan, plus `Build.SOC_MODEL` / `SOC_MANUFACTURER`;
     - macOS: the Metal device.
   - **NPUs:** DXCore NPU adapters (Windows); `/dev/accel/*` with `amdxdna` / `intel_vpu` (Linux);
     the SoC model (Android).
   - **Toolchains already installed:**
     - CMake, Ninja;
     - a C++ compiler (MSVC via `vswhere`, `gcc`/`clang` on PATH, w64devkit);
     - CUDA (`CUDA_PATH`, `nvcc`), the Vulkan SDK (`VULKAN_SDK`, `glslc`), the Hexagon SDK.
   - **On Windows, WSL too:**
     - whether WSL 2 is installed, and its distribution;
     - inside WSL: `nvidia-smi`, `/usr/local/cuda*/bin/nvcc` and its version, `gcc`, `cmake`, and
       WSL's memory limit.

     This is a short `wsl.exe -e sh -c …` call, with a 5 s timeout, and run only when WSL is present.
   - A **fingerprint**: the GPU, the driver version and the OS build. A change of fingerprint (for
     example a driver update) re-runs steps 3–7 for the active rung.
3. **Choose.** The catalogue (`resources/agent/engines.json`) lists each rung's requirements, for
   example:
   - R2: an NVIDIA GPU with compute 8.6 or more, driver at least the version TensorRT-RTX needs, at
     least 6 GB of VRAM, Windows x64 or Linux x64.

   Rules filter the catalogue to the rungs this device can run, and rank them by a speed estimate
   (later, by measured speed from step 6). The local agent explains the choice in plain words, and
   **the user approves**. Rungs that need a download or a build always ask unless "Upgrade engines
   automatically" is on.
4. **Acquire the engine.** One recipe per rung (below), run either:
   - **here:** only if the probe found every tool it needs. The app runs CMake through its existing
     process service, cancellable, with output streamed to the activity view;
   - **prebuilt:** the same recipe's output from our CI, a signed and checksummed zip.

   Every engine is a shared library with the same small **C interface** (below). That is what makes
   them swappable, and lets an MSVC-built TensorRT engine load into a MinGW-built app.
5. **Prepare the model** for the engine:
   - **ggml rungs:** nothing. They read the same GGUF.
   - **TensorRT:** the pack holds the INT4 ONNX exported in CI. On the device, TensorRT-RTX compiles
     it into an engine for *this* GPU, with a fixed profile (batch 1, context 4,096 to start). The
     engine and TensorRT-RTX's JIT cache go to `<appdata>/local-agent/engines/<rung>/<fingerprint>/`.
     This takes from seconds to a few minutes, with progress shown and cancellable.
6. **Verify**, in a *separate* helper process, so a crash costs nothing:
   - **Correctness, teacher-forced against R0 on this same machine.** Feed both engines the same 20
     golden texts (code, prose and tool-call JSON, about 4,000 tokens) and compare their predictions
     position by position. Thresholds:
     - top-1 agreement at least 92% (the two use different 4-bit layouts, so not 100%);
     - mean KL divergence of the next-token distribution below 0.05;
     - no NaN or Inf.

     Teacher forcing (always feeding the golden text, not the engine's own output) stops one early
     difference from cascading, so the numbers measure the engine and not luck.
   - **Task checks:** 10 short prompts with exact expected answers (a completion, a JSON tool call,
     a commit message format) must pass.
   - **Speed:** prompt processing tokens per second, decode tokens per second, time to first token,
     and peak memory, over 3 runs. The rung must be **at least 20% faster** at decode than the
     current one, or it is not worth switching.
   - **Stability:** 2 minutes of mixed load, with cancels in the middle.
   - The results are saved next to the engine and shown in Preferences ▸ Local agent.
7. **Switch.** Start a helper on the new engine, warm it up, then route *new* requests to it. The
   old helper finishes its request in flight and exits. The active rung is saved in the settings.
   The previous rung stays installed as the fallback.
8. **Watch** (plan 21's safety net, per rung). The active rung **drops back one rung** and is marked
   bad for this fingerprint when it:
   - crashes twice in 10 minutes;
   - produces NaN output;
   - hangs (the watchdog);
   - fails to load after a fingerprint change and re-verify.

   The user sees "Local agent: TensorRT stopped working after the driver update; using Vulkan".
   Re-trying a bad rung is a button.

## The engine interface (C ABI)

A plain C interface, so every engine can be built with whatever compiler its SDK demands (MSVC for
CUDA/TensorRT, the NDK for Android) and still load into the helper:

```c
// enp_engine.h — every engine library exports exactly these.
#define ENP_ENGINE_ABI 1
typedef struct enp_engine enp_engine;
typedef int (*enp_token_fn)(void* user, const char* utf8_piece, int token_id); // return 0 to stop

int          enp_engine_abi(void);                           // == ENP_ENGINE_ABI
const char*  enp_engine_describe(void);                      // JSON: name, version, devices it sees
enp_engine*  enp_engine_open(const char* config_json);       // model paths, device, context, threads
int          enp_engine_generate(enp_engine*, const char* request_json,   // prompt or tokens, sampling,
                                 enp_token_fn, void* user,                // grammar, max tokens
                                 const volatile int* cancel);
int          enp_engine_score(enp_engine*, const char* tokens_json,       // teacher-forced log-probs
                              float* out_top1_logprob, int* out_top1_id,  // (used by verify)
                              float* out_kl_ref, const char* ref_json);
const char*  enp_engine_last_error(enp_engine*);
void         enp_engine_close(enp_engine*);
```

- Memory never crosses the boundary: strings returned are owned by the engine and valid until the
  next call.
- The helper loads exactly one engine library per process.
- Tokenization is the engine's concern, but every engine must agree with R0's tokenizer on the
  golden texts (checked in verify).

## Compile recipes (phase 1)

Shared rules:
- **llama.cpp** is a submodule at `third_party/llama.cpp`, pinned to a release tag, with **no
  `branch =`** in `.gitmodules` (see plan 22 for the same locking).
- The engine wrappers live in `engines/<family>/`, with recipes in `cmake/engines/<rung>.cmake`.
- Recipes and engines are **not** under `resources/`, which ships. Only the catalogue and the skill
  texts are.
- Each recipe has a **CI job** that builds the prebuilt pack, and runs verify against R0 on a runner
  with the hardware where one exists.

**R0 · ggml CPU (the bootstrap).**
- Options:
  ```
  BUILD_SHARED_LIBS=ON  GGML_BACKEND_DL=ON  GGML_CPU_ALL_VARIANTS=ON (x86-64, ARM64)  GGML_NATIVE=OFF
  LLAMA_CURL=OFF  LLAMA_BUILD_TOOLS=OFF  LLAMA_BUILD_EXAMPLES=OFF  LLAMA_BUILD_TESTS=OFF
  ```
- `engines/ggml/enp_engine_ggml.cpp` wraps llama.cpp's C API and calls `ggml_backend_load_all()`
  on open (a library built with `GGML_BACKEND_DL` must do this itself, or it finds no devices).
- Toolchains: MSVC x64 and ARM64, w64devkit x64, clang on Linux and macOS, NDK `arm64-v8a`,
  Emscripten (the web's CPU fallback).
- No 32-bit Windows: the model's 3.35 GB mapping does not fit a 32-bit address space.
- Building it here: needs only CMake and a compiler. With `GGML_NATIVE=ON` it gains little over the
  all-variants build, so R0 is always the prebuilt pack. A local R0 build is only a test that the
  local-build route works on this machine.

**R1 · ggml Vulkan (the cross-platform GPU).**
- `GGML_VULKAN=ON`, in the same tree and the same wrapper. It adds `ggml-vulkan.dll` / `.so`, which
  `ggml_backend_load_all()` finds.
- Build needs the Vulkan headers and `glslc` (shaders are compiled at build time):
  - CI: the Vulkan SDK on Windows and Linux;
  - Android: the NDK's own `shader-tools/glslc`.
- Run needs only the GPU driver's Vulkan loader (`vulkan-1.dll`, `libvulkan.so`).
- Covers NVIDIA, AMD, Intel, Adreno, Mali and Samsung Xclipse, on Windows, Linux and Android.
- Device choice: the discrete GPU first; integrated only when it beats R0 in verify. On phones,
  Vulkan drivers vary in quality, which is exactly what verify is for.
- Offload: all layers (`n_gpu_layers = all`); PLE and the token embedding stay host-mapped.
- Building it here: only when the probe finds the Vulkan SDK (rare); otherwise the prebuilt pack
  (about 30–60 MB).

**R2 · TensorRT (fixed, NVIDIA RTX).**
- **Fixed** means one pinned version of everything, chosen once in the spike and changed only
  deliberately:
  - the TensorRT-Edge-LLM release;
  - the TensorRT-RTX release;
  - the export settings (INT4 weights, 16-bit activations);
  - the engine profile (batch 1, context 4,096, KV cache 16-bit).
- **In CI** (Linux x64 with a GPU runner, or a maintainer's machine; at first, this PC's WSL with
  CUDA 13.2, after `pip install` of Edge-LLM's exporter and PyTorch in a virtual environment):
  1. download `gemma-4-E2B-it-qat-q4_0-unquantized` (or the `w4a16-ct` build) at its pinned
     commit;
  2. run Edge-LLM's export to ONNX (INT4 weights; text decoder only; PLE kept as a host-side
     lookup if supported);
  3. validate the ONNX with golden tensors against the bf16 reference, following the
     `tensorrt-model-export` skill's method (real weights, golden-tensor checks, staged export if
     memory demands);
  4. publish the ONNX pack (expected about 1.5–2 GB) with its SHA-256.
- **Runtime pack** (CI, Windows with MSVC, Linux with gcc):
  - `engines/tensorrt/enp_engine_trt.cpp` wraps the Edge-LLM C++ runtime behind the C interface;
  - bundled with the TensorRT-RTX runtime libraries, which needs a **redistribution licence
    check** in the spike.
- **On the device:** no compiler and no CUDA toolkit, only the driver. TensorRT-RTX builds the
  engine from the ONNX for this GPU (ahead-of-time, then JIT-specialised on first run, cached).
- **Requires:** Windows x64 or Linux x64, an RTX 30xx or newer, at least 6 GB of VRAM, and the
  driver TensorRT-RTX needs. This PC qualifies (RTX 5060 Laptop, 8 GB, driver 616.92), and is the
  first test machine.
- Sampling, stop strings and grammar: Edge-LLM's sampler where it has one. JSON tool calls are
  validated after generation and retried once, because there may be no grammar-constrained decoding
  here, unlike ggml.

## Skills given to the local agent

A 2 B model is good at short, well-framed jobs and poor at open-ended debugging. So each skill is a
**short card** sent as its system prompt:
- when it applies;
- the inputs it gets;
- the **only** tools it may call;
- the exact output it must produce (a JSON schema; on ggml it is *enforced* with a GBNF grammar);
- its budget;
- two short examples.

Cards live in `resources/agent/skills/local/<name>.md` and ship with the app. Rules decide; the model
explains, drafts, or classifies inside a closed set. **The local agent never runs a command, a build
or a download itself**: it can only *propose* a catalogue action, which the app runs after the
user's approval.

**Engine skills (new in this plan):**

| Skill | Input | Tools | Output | Escalates when |
|---|---|---|---|---|
| `explain-environment` | the probe summary (about 30 lines) | none | 3–5 plain sentences: what this device can accelerate, and what it cannot and why | — |
| `propose-engine` | the rungs the rules allow, with requirements, sizes, time estimates, and measured speeds where known | `propose_engine_action {rung, route: build\|download}` | the proposal plus a one-paragraph reason for the user | never: the rules have already removed impossible rungs; the model only explains and orders ties |
| `diagnose-build` | the recipe name, the probe summary, the last 80 lines of the build or engine-compile log | none | `{cause: missing_sdk\|driver_too_old\|out_of_disk\|out_of_memory\|compiler_mismatch\|network\|vram_too_small\|unknown, advice}` | `unknown`, or the same cause twice |
| `explain-verify` | the verify report (agreement, KL, speeds, pass/fail) | none | 2 sentences: switched or not, and why | — |
| `brief-engine-problem` | probe, recipe, log tail, verify report | `escalate {brief}` | a brief for Claude Code (plan 21's escalation), sent only with the user's OK | — |

**The agent's other skills (from plan 21, now as cards):**
- `complete-code`: fill-in-the-middle if the spike finds Gemma 4 handles FIM well, otherwise
  prefix-only completion;
- `commit-message`;
- `explain-selection`;
- `summarize-symbol`;
- `route-task` (small/big);
- `judge-request` (plan 20's requests);
- `watch-agent` (explanation only: counters and timers decide);
- `write-brief`.

**Tools the local agent can call (the whole list):**
- `probe_summary`, `list_rungs`;
- `propose_engine_action`;
- `job_status`, `cancel_job`;
- `verify_report`;
- `escalate`;
- plus plan 20's *read* tools.

## The optimization ladder after phase 1

Each step is its own small item, done when phase 1 is solid, in roughly this order (biggest gain for
the most users first):
1. **Speculative decoding** is already in R0 and R1 (spike: +10–84%). What remains here:
   - Edge-LLM's `--spec-type gemma4_mtp` on R2, once R2 is INT4;
   - tuning the draft length per rung.
2. **R3 · ggml Metal** (macOS) and **ggml CUDA**:
   - for NVIDIA GPUs that R2 does not support;
   - built here wherever a CUDA toolkit is found, including **through WSL on Windows**;
   - prebuilt otherwise.
3. **R4 · Android GPU:** ggml OpenCL for Adreno, and LiteRT-LM GPU compared against Vulkan per
   device.
4. **R5 · NPUs**, each behind the same C interface:
   - Windows ML on Windows 11 24H2+: one integration for the NVIDIA, Intel, Qualcomm and AMD
     providers;
   - LiteRT-LM NPU on Android (Tensor, Qualcomm, MediaTek; Samsung Exynos once Google lists it);
   - FastFlowLM for AMD XDNA 2, after its licence check;
   - ggml Hexagon when it leaves "experimental".
5. **R7 · WebGPU** for the web build.
6. **Engine profiles:** a longer context, an fp16 TensorRT engine on 12 GB+ GPUs, E4B on strong
   machines.

## Phases (phase 1 = this plan's first work)

| # | Work | Done when |
|---|---|---|
| 23.0 | **Linux part done (1 Nov 2026):** see "Spike results". **Remaining:** Windows side, an Android phone, R2 INT4. **Original scope:** on this PC, then a mid-range Android phone. By hand:<br>• R0 on Windows;<br>• ggml CUDA in WSL (CUDA 13.2, `sm_120`), as the speed reference;<br>• R2: export in WSL, then the engine on Windows.<br>Measures speed, memory, quality on our code, and FIM. Also answers: whether TensorRT-Edge-LLM imports `w4a16-ct` or needs our export; whether PLE stays on the host; the TensorRT-RTX redistribution terms; and whether the 11 GB WSL limit is enough for the export. | Numbers written into this plan; the pinned versions chosen |
| 23.1a | **Engine lab** (done, 1 Nov 2026): a hidden developer page, reached by tapping the version in About seven times. It has R0 inside the app, the model and drafter downloads (SHA-256, free-space check, DownloadManager on Android), runs with or without the drafter, results and history. Windows CPU (Core 5 210H): **21.1 tok/s, or 34.8 with the drafter**. llama.cpp is pinned and patched at configure time (`patches/llama.cpp`). | Measured on a phone |
| 23.1 | C interface, helper loading an engine library, **R0** (prebuilt pack + GGUF download with resume and SHA-256) | The local agent answers offline on Windows, Linux, macOS and Android |
| 23.2 | **Probe** → `environment.json` on every platform (with WSL on Windows), and the fingerprint | Probe test passes on all CI runners. This PC reports the RTX 5060, the Intel iGPU, no CUDA on the Windows side, and CUDA 13.2 in WSL |
| 23.3 | **Verify and switch** (teacher-forced scoring, task checks, benchmark, hot swap, drop back) | A fake engine that is wrong, slow or crashing is never switched to, or is dropped from |
| 23.4 | **R1 Vulkan** (CI packs; local build when the SDK is found) | Switches on this PC's RTX and on an Android phone; drops back where slower |
| 23.5 | **R2 TensorRT** (CI ONNX export and runtime pack; on-device engine build) | Switches on this PC; survives a simulated driver change by rebuilding or dropping back |
| 23.6 | **Engine skills** and the Preferences ▸ Local agent ▸ Engine page | The user can see the ladder, the measurements, and why each rung is or is not used |

## Spike results: Linux first (WSL 2, 1 Nov 2026)

**Machine:**
- Intel Core 5 210H, 6 threads used;
- RTX 5060 Laptop GPU (sm_120, 8 GB, driver 616.92);
- WSL 2 with Ubuntu 24.04, 11 GB of memory and CUDA 13.2.

Scripts and logs are kept in `~/enp-spike` in WSL.

**Pinned versions:**

| Component | Version |
|---|---|
| llama.cpp | b12030 (`a91eab6`) |
| Main model | `google/gemma-4-E2B-it-qat-q4_0-gguf` @ `675cff42`, SHA-256 `fa401b55…6634` (verified) |
| Drafter | `google/gemma-4-E2B-it-qat-q4_0-unquantized-assistant` @ `ebc7e1a2` |
| TensorRT export source | `google/gemma-4-E2B-it-qat-q4_0-unquantized` @ `6befbaca` |
| TensorRT-Edge-LLM | 0.11.1 |
| TensorRT | 11.4.0.106 (`tensorrt-cu13`) |

**Speed (Gemma 4 E2B, QAT Q4_0):**

| Engine | Prompt (pp512) | Generation (tg128) | Peak memory |
|---|---|---|---|
| R0 ggml CPU, all variants | 125 tok/s | **20.3 tok/s** | — (memory-mapped) |
| ggml CUDA (R3, built in WSL) | 5,982 tok/s | **116 tok/s** | about 2.4 GB VRAM (PLE stays on the host) |
| R2 TensorRT, **fp16** (Edge-LLM) | — | **16 tok/s** | 7.75 GB VRAM **+ 3.56 GB spilled to shared system memory** |

**Agreement (plan 23's verify, teacher-forced, 12×512 tokens of our own code):**
- CUDA scored against R0: **top-1 98.9%**, mean KL divergence **0.0021**, perplexity 6.29.
- Both engines use the same 4-bit weights, so the thresholds can be tighter for same-weights rungs:
  at least 97% and below 0.01. The 92% / 0.05 thresholds remain for rungs with different weights.

**Real use (llama-server, CUDA / CPU):**
- **Commit message** (about 710-token prompt): 0.36 s on CUDA. The answer was good: "Implement
  text and button layout functions."
- **Completion** with plan 21's context (1,669 tokens before the caret):
  - CUDA: 0.55 s cold, **0.23 s warm** (the prompt cache leaves 4 new tokens).
  - CPU: **16 s cold**, 1.4 s warm.

  So on R0, completion needs a short context (about 256 tokens), or runs only after the cache is
  warm. A cold 1.5 k-token prefill is too slow for typing.
- **Gemma 4 has no fill-in-the-middle tokens**: `/infill` reports that the prefix, suffix and middle
  tokens are missing. Completion is prefix-only, or a chat-style "complete this" prompt with the
  suffix given as context.
- **Thinking is on by default** in the chat template, and it used the whole budget with no answer.
  Every skill sends `enable_thinking: false`; only the escalation brief may think.
- **The model called "Discard all changes" harmless** (thinking off, JSON schema enforced).
  TensorRT, on its own sampler, called it "changes". This confirms plan 21: **rules decide risk**,
  and the model only explains.

**Speculative decoding (Google's QAT MTP drafter, `--spec-type draft-mtp`):**

| | Prose | Code | Commit message |
|---|---|---|---|
| CUDA | 101 → 112 tok/s (+10%), 31% accepted | 101 → **165** (+63%), 60% accepted | 99 → 121 (+22%), 37% accepted |
| CPU (R0) | 20 → 27 (+37%) | 20 → **37** (+84%) | 20 → 32 (+57%) |

- The drafter is converted by llama.cpp's own `convert_hf_to_gguf.py` from Google's checkpoint (not
  a community file), then quantized to Q4_0: **76.5 MB**.
- Its acceptance and speed match the bf16 drafter (170 MB), so Q4_0 ships.
- Speculation helps most where it matters most for us: code, and the slow CPU rung. **It becomes
  part of R0**, no longer a later rung.

**TensorRT (R2), what we learned:**
- **Builds on the device without a CUDA toolkit:** the `llm` engine alone took 3 min 4 s (3.4 GB
  host memory). `trtedge serve` builds every component (LLM, vision, audio) in about 9 min, and
  cached starts take about 105 s.
- **The engine holds no weights** (a 60 MB `llm.engine`). The runtime binds them from the
  checkpoint, so the shipped model *is* the checkpoint. The PLE table stays in the checkpoint as an
  engine input (`ple_token_embeds_i`), which answers the spike question: yes, PLE stays on the host.
- **fp16 does not fit 8 GB.** The runtime's working set is about 11 GB. Windows silently spills the
  rest to shared memory (measured with the GPU memory counters), and generation drops to 16 tok/s,
  7× slower than ggml CUDA. **R2 needs an INT4 (or NVFP4 on Blackwell) checkpoint.**
- **Google's `w4a16-ct` checkpoint is not readable:** Edge-LLM reads AWQ, GPTQ and ModelOpt formats,
  not compressed-tensors. `trtedge quantize llm --quantization int4_awq` (or `nvfp4`) needs a GPU at
  least as large as the fp16 checkpoint (about 10–12 GB), more than this laptop has.
- **So R2's quantization moves to CI** (or a 12 GB+ GPU), as planned: publish one INT4-AWQ
  checkpoint, and the device only builds the engine. Expected size: about 1.2 GB of compute weights
  plus the PLE and embedding tables.
- Edge-LLM keeps Gemma 4's `down_proj` in fp16 by default for accuracy.
- `serve` has no text-only mode; our C++ wrapper should build and load `llm` only.

**Download size:**
- **Zip barely helps 4-bit weights**:
  - the model zips from 3,350 to 3,183 MB (−5%, 3 min at `-9`);
  - the drafter from 76.5 to 62.1 MB (−19%);
  - slices compress 4–16% under deflate or xz.
- The pack is still a zip, for one file, one checksum and resume, but compression is no lever.
- **Where the bytes are:**
  - `per_layer_token_embd`: **1,927 MB, 58%**, at Q6_K;
  - `token_embd`: 330 MB, 10%, at Q6_K;
  - all transformer blocks: about 1,050 MB at Q4_0.

  The real saving is storing the two lookup tables at fewer bits:

  | Embedding tables at | File | Saved | Top-1 vs Google's file | Mean KLD | Perplexity |
  |---|---|---|---|---|---|
  | Q6_K (Google's file) | 3,350 MB | — | — | — | 6.286 |
  | control: blocks re-quantized Q4_0→Q4_0, tables kept | 3,350 MB | 0 | 100% | 0 | 6.286 (re-quantizing the QAT blocks is exact) |
  | Q5_K | 2,984 MB | −366 MB (−11%) | 98.7% | 0.0026 | 6.276 |
  | **Q4_K** | **2,640 MB** | **−710 MB (−21%)** | 97.5% | 0.0062 | 6.283 |
  | Q4_0 | 2,640 MB | −710 MB | 98.0% | 0.0059 | 6.326 (+0.6%) |

- **Decision (pending the task checks):** ship **QAT Q4_0 blocks + Q4_K embedding tables, 2.64 GB**.
  - Perplexity is unchanged.
  - The shift is about 3× the CUDA-vs-CPU difference, and well inside the 92% / 0.05 thresholds.
  - Q5_K (2.98 GB) is the fallback if the task checks find any regression.
- **How it is made:**
  - CI makes it from Google's file with `llama-quantize --allow-requantize --token-embedding-type q4_k
    --tensor-type per_layer_token_embd=q4_k … Q4_0`.
  - Better still, it quantizes the tables straight from the QAT half-precision checkpoint, avoiding
    Q6_K→Q4_K double rounding.
  - The file is ours, so it is hosted with the engine packs under the catalogue's SHA-256 (Apache
    2.0 allows redistribution).
- **The drafter is shipped in the same pack:**
  - **2.64 GB + 77 MB**, zipped (store-only for the model, deflate for the rest), against Google's
    3.35 GB;
  - **−20% download, and smaller in memory too.**

**What changes in this plan:**
- The bootstrap file becomes ours: QAT Q4_0 with Q4_K tables, plus the Q4_0 drafter.
- **MTP speculation is part of R0 and R1** from the start.
- **R2 needs an INT4 checkpoint quantized in CI**, because fp16 overflows 8 GB GPUs.
- Completion on R0 uses a short context.
- No FIM.
- Thinking is off for skills.
- Same-weights verify thresholds are tightened to 97% / 0.01.

**Still to measure (next spike runs):**
- the same on the Windows side (MinGW and MSVC R0, Vulkan R1);
- an Android phone (R0 and Vulkan);
- R2 INT4 on a 12 GB+ GPU or CI;
- the task checks on the Q4_K-table file.

## Settings (Preferences ▸ Local agent ▸ Engine)

- **Engine:**
  - Automatic (default): climb the ladder;
  - or pin one rung;
  - or CPU only.
- **Upgrade engines automatically:** off by default. When off, every download or build asks.
- **Allow local builds:** on by default, used only when the tools are found. Off means always
  prebuilt.
- **GPU:** automatic, or choose one (when several are found).
- **Show the ladder:**
  - each rung: available or why not, installed, verify results, speed;
  - Re-verify, Remove, Retry.
- **Disk used** by engines and models, with Remove all.

## Tests

**Unit:**
- Catalogue rules against canned `environment.json` files: an RTX desktop, this laptop, an AMD iGPU
  laptop, an M-series Mac, a Snapdragon phone, an Exynos phone, and a no-GPU VM.
- The fingerprint detecting a driver change.
- Verify metrics against fixed log-prob arrays.
- Drop-back state machine.
- Skill output schemas.

**With fake engines** (a test library behind the same C interface that is wrong / slow / hangs /
crashes / NaNs on command):
- verify rejects each;
- the active rung drops back;
- the editor's frame times stay flat throughout.

**Integration (on demand, real hardware):**
- R0 → R1 → R2 on this PC: each verify report saved, and each switch made without a dropped
  request.
- R0 → R1 on an Android phone.
- A simulated driver-version change triggers re-verify.

**CI:**
- R0 and R1 packs built on every platform;
- R2 runtime pack built on Windows and Linux;
- the ONNX export job run manually, since it needs the GPU and the weights.

Sources (Nov 2026):
- [TensorRT-Edge-LLM](https://github.com/NVIDIA/TensorRT-Edge-LLM)
- [TensorRT for RTX](https://docs.nvidia.com/deeplearning/tensorrt-rtx/latest)
- [LiteRT-LM NPU](https://developers.google.com/edge/litert/next/litert_lm_npu)
- [LiteRT NPU (Exynos)](https://developers.google.com/edge/litert/next/npu)
- [Windows ML providers](https://learn.microsoft.com/windows/ai/new-windows-ml/supported-execution-providers)
- [FastFlowLM](https://github.com/FastFlowLM/FastFlowLM)
- [ggml WebGPU paper](https://www.alphaxiv.org/abs/2605.20706)
- [Gemma 4 E2B](https://huggingface.co/google/gemma-4-E2B-it)
- [QAT Q4_0 GGUF](https://huggingface.co/google/gemma-4-E2B-it-qat-q4_0-gguf)
- [LiteRT-LM files](https://huggingface.co/litert-community/gemma-4-E2B-it-litert-lm)
