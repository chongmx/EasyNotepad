#include "enp/services/LocalEngineService.h"

#include "enp/model/Hash.h"
#include "enp/os/Dispatch.h"
#include "enp/os/Download.h"
#include "enp/os/Fs.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <sys/resource.h>
#include <sys/sysctl.h>
#endif
#if defined(__ANDROID__)
#include <sys/auxv.h>
#include <sys/system_properties.h>
#endif
#if defined(__x86_64__) || defined(_M_X64)
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

#if ENP_HAS_LOCAL_ENGINE
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function" // helpers defined in llama.cpp's headers
#endif
#include "build-info.h"
#include "chat.h"
#include "common.h"
#include "ggml-backend.h"
#include "llama.h"
#include "log.h"
#include "sampling.h"
#include "speculative.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#endif

namespace enp::services {

using interfaces::DownloadStatus;
using interfaces::EngineDevice;
using interfaces::EngineInfo;
using interfaces::EngineRunResult;
using interfaces::EngineRunSettings;

namespace {

// ---- What the device is ------------------------------------------------------------------------

#if !defined(_WIN32) && !defined(__APPLE__) // Linux and Android read /proc
std::string readText(const char* path)
{
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// "key : value" lines (/proc/cpuinfo, /proc/meminfo): the first value for `key`.
std::string procField(const std::string& text, const std::string& key)
{
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
        if (line.compare(0, key.size(), key) != 0)
            continue;
        const size_t colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        std::string value = line.substr(colon + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        return value;
    }
    return {};
}
#endif

#if defined(__ANDROID__)
std::string property(const char* name)
{
    char value[PROP_VALUE_MAX] = {};
    __system_property_get(name, value);
    return value;
}
#endif

std::string cpuName()
{
#if defined(_WIN32)
    char name[128] = {};
    DWORD size = sizeof(name);
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString",
                     RRF_RT_REG_SZ, nullptr, name, &size) == ERROR_SUCCESS)
        return name;
    return {};
#elif defined(__APPLE__)
    char name[128] = {};
    size_t size = sizeof(name);
    if (sysctlbyname("machdep.cpu.brand_string", name, &size, nullptr, 0) == 0)
        return name;
    return {};
#else
    const std::string info = readText("/proc/cpuinfo");
    for (const char* key : {"model name", "Hardware", "Processor"})
        if (std::string v = procField(info, key); !v.empty())
            return v;
    return {};
#endif
}

void memoryInfo(uint64_t& total, uint64_t& available)
{
#if defined(_WIN32)
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof(m);
    if (GlobalMemoryStatusEx(&m)) {
        total = m.ullTotalPhys;
        available = m.ullAvailPhys;
    }
#elif defined(__APPLE__)
    uint64_t size = 0;
    size_t len = sizeof(size);
    if (sysctlbyname("hw.memsize", &size, &len, nullptr, 0) == 0)
        total = size;
#else
    const std::string info = readText("/proc/meminfo");
    total = std::strtoull(procField(info, "MemTotal").c_str(), nullptr, 10) * 1024;
    available = std::strtoull(procField(info, "MemAvailable").c_str(), nullptr, 10) * 1024;
#endif
}

// The process's peak resident memory so far (bytes), or 0.
uint64_t peakMemory()
{
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return pmc.PeakWorkingSetSize;
    return 0;
#elif defined(__APPLE__)
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return uint64_t(usage.ru_maxrss);
#else
    return std::strtoull(procField(readText("/proc/self/status"), "VmHWM").c_str(), nullptr, 10) * 1024;
#endif
}

// Starts counting the peak again (Linux and Android allow it; elsewhere it is the process's peak).
void resetPeakMemory()
{
#if defined(__linux__)
    if (FILE* f = std::fopen("/proc/self/clear_refs", "w")) {
        std::fputs("5", f);
        std::fclose(f);
    }
#endif
}

// The engine is built for one CPU level (see CMakeLists.txt): AVX2 + FMA + F16C on x86-64,
// dot product + fp16 arithmetic on Android ARM64. Running it on less would crash the app.
bool cpuSupported(std::string& features, std::string& missing)
{
#if defined(__x86_64__) || defined(_M_X64)
    unsigned r1[4] = {}, r7[4] = {};
#if defined(_MSC_VER)
    int a[4];
    __cpuid(a, 1);
    std::memcpy(r1, a, sizeof(r1));
    __cpuidex(a, 7, 0);
    std::memcpy(r7, a, sizeof(r7));
#else
    __get_cpuid(1, &r1[0], &r1[1], &r1[2], &r1[3]);
    __get_cpuid_count(7, 0, &r7[0], &r7[1], &r7[2], &r7[3]);
#endif
    bool osAvx = false;
    if (r1[2] & (1u << 27)) { // OSXSAVE: the OS saves the AVX registers
#if defined(_MSC_VER)
        osAvx = (_xgetbv(0) & 6) == 6;
#else
        unsigned lo = 0, hi = 0;
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        osAvx = (lo & 6) == 6;
#endif
    }
    const bool fma = r1[2] & (1u << 12), f16c = r1[2] & (1u << 29);
    const bool avx2 = osAvx && (r7[1] & (1u << 5)), bmi2 = r7[1] & (1u << 8);
    const bool avx512 = osAvx && (r7[1] & (1u << 16)), vnni = r7[2] & (1u << 11);
    for (auto [on, name] : {std::pair{avx2, "AVX2"}, {fma, "FMA"}, {f16c, "F16C"}, {bmi2, "BMI2"}, {avx512, "AVX-512"},
                            {vnni, "VNNI"}})
        if (on)
            features += features.empty() ? name : std::string(" ") + name;
    if (!avx2 || !fma || !f16c || !bmi2)
        missing = "AVX2, FMA, F16C and BMI2 (2013 or newer processors)";
    return missing.empty();
#elif defined(__ANDROID__) && defined(__aarch64__)
    const unsigned long hw = getauxval(AT_HWCAP), hw2 = getauxval(AT_HWCAP2);
    const bool dot = hw & (1ul << 20), fp16 = hw & (1ul << 10), sve = hw & (1ul << 22);
    const bool i8mm = hw2 & (1ul << 13);
    for (auto [on, name] : {std::pair{dot, "dotprod"}, {fp16, "fp16"}, {i8mm, "i8mm"}, {sve, "SVE"}})
        if (on)
            features += features.empty() ? name : std::string(" ") + name;
    if (!dot || !fp16)
        missing = "ARMv8.2 dot product and fp16 (most phones from 2019 on)";
    return missing.empty();
#else
    features = "NEON";
    return true;
#endif
}

#if ENP_HAS_LOCAL_ENGINE
void initBackend()
{
    static const bool once = []() {
        // The lab shows what matters; nothing goes to stdout or stderr.
        llama_log_set([](ggml_log_level, const char*, void*) {}, nullptr);
        common_log_set_stdo(common_log_main(), false);
        llama_backend_init();
        return true;
    }();
    (void)once;
}

// A realistic code prefix to continue (the agent's main job), not random tokens.
const char* const kGeneratePrompt =
    "// C++17\n#include <string_view>\n#include <vector>\n\n"
    "// Splits text into lines. Handles \"\\r\\n\" and \"\\n\"; the last line may be empty.\n"
    "std::vector<std::string_view> splitLines(std::string_view text)\n{\n";

// Text for measuring prompt processing: varied prose and code, repeated as needed.
const char* const kPromptText =
    "The editor keeps each document as a piece table: an original buffer, an append buffer and a list of "
    "pieces that say which parts of either make up the text. Inserting splits one piece and adds another; "
    "deleting trims or removes pieces. Undo records the pieces changed, not a copy of the text.\n"
    "struct Piece { bool added; size_t start; size_t length; };\n"
    "size_t TextBuffer::lineCount() const { return lineStarts_.size(); }\n"
    "void TextBuffer::insert(size_t at, std::string_view text) {\n"
    "    auto [index, offset] = locate(at);\n    split(index, offset);\n"
    "    pieces_.insert(pieces_.begin() + index + 1, Piece{true, added_.size(), text.size()});\n"
    "    added_.append(text);\n    reindexLines(index);\n}\n";

using Clock = std::chrono::steady_clock;
double secondsBetween(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double>(b - a).count(); }

// Parameters shared by the lab's runs and the agent's chats.
common_params engineParams(const std::string& modelPath, const std::string& drafterPath, int threads, bool gpu,
                           int contextTokens, int batchTokens, const std::atomic<bool>* cancel)
{
    common_params params;
    params.model.path = modelPath;
    params.n_ctx = contextTokens;
    params.n_batch = std::max(512, batchTokens);
    params.n_ubatch = 512;
    params.cpuparams.n_threads = threads;
    params.cpuparams_batch.n_threads = threads;
    params.n_gpu_layers = gpu ? 999 : 0;
    params.load_progress_callback = [](float, void* flag) { return !static_cast<const std::atomic<bool>*>(flag)->load(); };
    params.load_progress_callback_user_data = const_cast<std::atomic<bool>*>(cancel);
    if (!drafterPath.empty()) {
        params.speculative.types = {COMMON_SPECULATIVE_TYPE_DRAFT_MTP};
        params.speculative.draft.mparams.path = drafterPath;
        params.speculative.draft.n_gpu_layers = gpu ? 999 : 0;
        const auto limits = common_speculative_get_output_limits(params.n_batch, params.n_parallel,
                                                                 common_speculative_n_max(&params.speculative));
        params.n_outputs_max = limits.total;
        params.n_outputs_max_per_seq = limits.per_seq;
    }
    return params;
}

// A model in memory, with its drafter when there is one.
struct Loaded {
    common_params params;
    common_init_result_ptr init;
    common_speculative_init_result_ptr specInit;
    common_chat_templates_ptr templates; // the agent's chat format (loaded when first needed)
    llama_context* ctxDraft = nullptr;
    llama_model* model() const { return init ? init->model() : nullptr; }
    llama_context* ctx() const { return init ? init->context() : nullptr; }
    bool drafter() const { return ctxDraft != nullptr; }
};

// Loads the model (and drafter); null with `error` set when it cannot.
std::unique_ptr<Loaded> loadModel(common_params params, std::string& error)
{
    auto m = std::make_unique<Loaded>();
    m->init = common_init_from_params(params);
    if (!m->model() || !m->ctx()) {
        error = "the model could not be loaded (file, memory or format)";
        return nullptr;
    }
    if (!params.speculative.draft.mparams.path.empty()) {
        common_params draftParams = common_base_params_to_speculative(params);
        m->specInit = common_speculative_init_from_params(draftParams, m->model(), m->ctx());
        m->ctxDraft = m->specInit ? m->specInit->context() : nullptr;
        if (!m->ctxDraft) {
            error = "the drafter could not be loaded with this model";
            return nullptr;
        }
        params.speculative.draft.ctx_tgt = m->ctx();
        params.speculative.draft.ctx_dft = m->ctxDraft;
    }
    m->params = std::move(params);
    return m;
}

struct Generated {
    int produced = 0;
    int drafted = 0;
    int accepted = 0;
    double firstTokenSeconds = 0; // the prompt processed and the first token sampled
    double decodeSeconds = 0;     // after the first token
};

// Generates up to `maxTokens` after the prompt `inp`, from an empty context, as llama.cpp's
// speculative example does: the drafter (when loaded) proposes tokens, the model checks them in
// one batch and keeps those it agrees with. Each piece of text goes to `onPiece` (this thread).
Generated generate(Loaded& m, const std::vector<llama_token>& inp, int maxTokens, const std::atomic<bool>& cancel,
                   const std::function<void(const std::string&)>& onPiece)
{
    Generated g;
    llama_context* ctx = m.ctx();
    llama_context* ctxDraft = m.ctxDraft;
    const llama_vocab* vocab = llama_model_get_vocab(m.model());
    llama_memory_clear(llama_get_memory(ctx), true);
    if (ctxDraft)
        llama_memory_clear(llama_get_memory(ctxDraft), true);
    if (inp.empty())
        return g;

    common_speculative* spec = nullptr;
    if (m.drafter()) {
        spec = common_speculative_init(m.params.speculative, 1);
        if (!spec)
            throw std::runtime_error("the drafter could not be used with this model");
    }
    struct SpecFree {
        common_speculative* s;
        ~SpecFree()
        {
            if (s)
                common_speculative_free(s);
        }
    } specFree{spec};
    const bool ckptTarget = common_context_can_seq_rm(ctx) == COMMON_CONTEXT_SEQ_RM_TYPE_FULL;
    const bool ckptDraft = ctxDraft && common_context_can_seq_rm(ctxDraft) == COMMON_CONTEXT_SEQ_RM_TYPE_FULL;

    const auto tFirst = Clock::now();
    common_sampler_ptr sampler(common_sampler_init(m.model(), m.params.sampling));
    const llama_seq_id seq = 0;
    {
        common_batch batch(ctx);
        for (size_t i = 0; i + 1 < inp.size(); ++i)
            batch.add(inp[i], llama_pos(i), seq, false);
        llama_process(ctx, LLAMA_PROCESS_TYPE_DECODE, batch.get());
        if (spec && !common_speculative_process(spec, batch))
            throw std::runtime_error("the drafter failed on the prompt");
    }
    llama_token last = inp.back();
    llama_tokens context(inp.begin(), inp.end() - 1);
    int past = int(inp.size()) - 1;
    if (spec)
        common_speculative_begin(spec, seq, context);
    common_batch batch(ctx);
    llama_tokens draft;
    common_prompt_checkpoint ckpt;
    bool first = true;
    Clock::time_point tDecode = Clock::now();
    while (g.produced < maxTokens && !cancel.load()) {
        if (spec && draft.empty()) {
            ckpt.update_pos(context.size(), llama_memory_seq_pos_min(llama_get_memory(ctx), seq),
                            llama_memory_seq_pos_max(llama_get_memory(ctx), seq));
            if (ckptDraft)
                ckpt.update_dft(ctxDraft, seq, LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY);
            const int room = std::max(0, std::min(int(llama_n_ctx(ctx)) - past - 2, maxTokens - g.produced - 1));
            common_speculative_get_draft_params(spec, seq) = {true, room, past, last, &context, &draft};
            common_speculative_draft(spec);
            if (!draft.empty() && ckptTarget)
                ckpt.update_tgt(ctx, seq, LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY);
            if (ctxDraft) {
                if (ckptDraft)
                    ckpt.load_dft(ctxDraft, seq, LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY);
                llama_memory_seq_rm(llama_get_memory(ctxDraft), seq, ckpt.pos_max + 1, -1);
            }
        }
        batch.clear();
        batch.add(last, past++, seq, true);
        for (size_t i = 0; i < draft.size(); ++i)
            batch.add(draft[i], llama_pos(past + int(i)), seq, true);
        llama_process(ctx, LLAMA_PROCESS_TYPE_DECODE, batch.get());
        if (spec && !common_speculative_process(spec, batch))
            throw std::runtime_error("the drafter failed");

        common_sampler_ptr saved;
        if (spec && ckptTarget)
            saved.reset(common_sampler_clone(sampler.get()));
        const size_t drafted = draft.size();
        std::vector<llama_token> ids = common_sampler_sample_and_accept_n(sampler.get(), ctx, draft);
        if (first) {
            g.firstTokenSeconds = secondsBetween(tFirst, Clock::now());
            tDecode = Clock::now();
            first = false;
        }
        if (spec && ckptTarget && ids.size() - 1 < drafted) {
            // Partly accepted and the context cannot drop a tail: go back to the checkpoint and
            // evaluate the accepted part as the next draft.
            draft = std::move(ids);
            ckpt.load_tgt(ctx, seq, LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY);
            llama_memory_seq_rm(llama_get_memory(ctx), seq, ckpt.pos_max + 1, -1);
            if (ctxDraft) {
                if (ckptDraft)
                    ckpt.load_dft(ctxDraft, seq, LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY);
                llama_memory_seq_rm(llama_get_memory(ctxDraft), seq, ckpt.pos_max + 1, -1);
            }
            context.resize(ckpt.n_tokens);
            sampler = std::move(saved);
            past = int(context.size());
            continue;
        }
        if (spec)
            common_speculative_accept(spec, seq, uint16_t(ids.size() - 1));
        past += int(ids.size()) - 1;
        g.drafted += int(drafted);
        g.accepted += int(ids.size()) - 1;
        g.produced += int(ids.size());
        bool end = false;
        for (llama_token id : ids) {
            context.push_back(last);
            last = id;
            if (llama_vocab_is_eog(vocab, id)) {
                end = true;
                break;
            }
            onPiece(common_token_to_piece(ctx, id));
        }
        draft.clear();
        llama_memory_seq_rm(llama_get_memory(ctx), seq, past, -1);
        if (ctxDraft)
            llama_memory_seq_rm(llama_get_memory(ctxDraft), seq, past, -1);
        if (end)
            break;
    }
    g.decodeSeconds = secondsBetween(tDecode, Clock::now());
    return g;
}

// A chat prompt as tokens: template text may hold special tokens, the user's text may not.
std::vector<llama_token> chatTokens(Loaded& m, const std::vector<interfaces::LocalChatMessage>& messages)
{
    if (!m.templates)
        m.templates = common_chat_templates_init(m.model(), "");
    common_chat_templates_inputs inputs;
    for (const interfaces::LocalChatMessage& message : messages) {
        common_chat_msg msg;
        msg.role = message.role;
        msg.content = message.text;
        inputs.messages.push_back(std::move(msg));
    }
    inputs.add_generation_prompt = true;
    inputs.enable_thinking = false; // quick answers; thinking used the whole budget in the trial
    const common_chat_params formatted = common_chat_templates_apply(m.templates.get(), inputs);
    const llama_vocab* vocab = llama_model_get_vocab(m.model());
    std::vector<llama_token> tokens;
    for (const common_chat_prompt_part& part : formatted.prompt.parts) {
        const std::vector<llama_token> t = common_tokenize(vocab, part.text, false, !part.is_input);
        tokens.insert(tokens.end(), t.begin(), t.end());
    }
    return tokens;
}
#endif

} // namespace

// The agent's model, kept loaded between questions.
struct LocalEngineService::ChatModel {
    std::mutex mutex; // held while a chat uses or (un)loads it
    std::string key;  // model, drafter, threads, GPU, context: a change means loading again
    std::chrono::steady_clock::time_point lastUse;
#if ENP_HAS_LOCAL_ENGINE
    std::unique_ptr<Loaded> loaded;
#endif
    bool isLoaded() const
    {
#if ENP_HAS_LOCAL_ENGINE
        return loaded != nullptr;
#else
        return false;
#endif
    }
    void unload()
    {
#if ENP_HAS_LOCAL_ENGINE
        loaded.reset();
#endif
        key.clear();
    }
};

LocalEngineService::LocalEngineService() : chat_(std::make_shared<ChatModel>()) {}

LocalEngineService::~LocalEngineService()
{
    alive_->store(false);
    cancel_->store(true);
}

EngineInfo LocalEngineService::info()
{
    if (info_)
        return *info_;
    EngineInfo i;
    i.cpuName = cpuName();
    i.cpuThreads = int(std::max(1u, std::thread::hardware_concurrency()));
    memoryInfo(i.memoryTotal, i.memoryAvailable);
#if defined(__ANDROID__)
    const std::string soc = property("ro.soc.manufacturer") + " " + property("ro.soc.model");
    i.socName = soc == " " ? property("ro.board.platform") : soc;
    if (i.cpuName.empty())
        i.cpuName = property("ro.product.model");
#endif
    std::string missing;
    const bool cpuOk = cpuSupported(i.cpuFeatures, missing);
#if ENP_HAS_LOCAL_ENGINE
    i.engineVersion = "llama.cpp b" + std::to_string(llama_build_number()) + " (" + llama_commit() + ")";
    if (!cpuOk) {
        i.unavailableReason = "This processor lacks " + missing + ", which this build of the engine needs.";
    } else {
        initBackend();
        i.available = true;
        for (size_t d = 0; d < ggml_backend_dev_count(); ++d) {
            ggml_backend_dev_t dev = ggml_backend_dev_get(d);
            EngineDevice e;
            e.name = ggml_backend_dev_name(dev);
            e.description = ggml_backend_dev_description(dev);
            switch (ggml_backend_dev_type(dev)) {
            case GGML_BACKEND_DEVICE_TYPE_CPU: e.kind = "CPU"; break;
            case GGML_BACKEND_DEVICE_TYPE_GPU: e.kind = "GPU"; break;
            case GGML_BACKEND_DEVICE_TYPE_IGPU: e.kind = "iGPU"; break;
            default: e.kind = "accelerator"; break;
            }
            size_t free = 0, total = 0;
            ggml_backend_dev_memory(dev, &free, &total);
            e.memoryFree = free;
            e.memoryTotal = total;
            i.devices.push_back(std::move(e));
        }
    }
#else
    (void)cpuOk;
    i.unavailableReason = "This build has no inference engine (32-bit and web builds leave it out).";
#endif
    info_ = i;
    return i;
}

void LocalEngineService::run(const EngineRunSettings& settings, std::function<void(const std::string& stage)> progress,
                             std::function<void(const EngineRunResult& result)> done)
{
    if (running_->exchange(true)) {
        EngineRunResult r;
        r.error = "a run is already in progress";
        done(r);
        return;
    }
    cancel_->store(false);
    auto alive = alive_;
    auto cancel = cancel_;
    auto running = running_;
    auto report = [alive, progress](std::string stage) {
        os::dispatch::postToMain([alive, progress, stage = std::move(stage)]() {
            if (alive->load() && progress)
                progress(stage);
        });
    };
    auto finish = [alive, running, done](EngineRunResult result) {
        os::dispatch::postToMain([alive, running, done, result = std::move(result)]() {
            running->store(false);
            if (alive->load() && done)
                done(result);
        });
    };
#if !ENP_HAS_LOCAL_ENGINE
    (void)settings;
    (void)report;
    (void)cancel;
    EngineRunResult r;
    r.error = info().unavailableReason;
    finish(r);
#else
    if (!info().available) {
        EngineRunResult r;
        r.error = info().unavailableReason;
        finish(r);
        return;
    }
    // One model in memory at a time: the agent's, if loaded, makes room (a phone has little spare).
    if (chatting_->load()) {
        EngineRunResult r;
        r.error = "the local agent is answering; try again when it is done";
        finish(r);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(chat_->mutex);
        chat_->unload();
    }
    std::thread([settings, report, finish, cancel]() {
        EngineRunResult r;
        r.drafter = !settings.drafterPath.empty();
        try {
            resetPeakMemory();
            common_params params = engineParams(settings.modelPath, settings.drafterPath, settings.threads, settings.useGpu,
                                                settings.contextTokens, settings.promptTokens, cancel.get());
            params.sampling.temp = 0.0f; // greedy: the same text every run

            report("Loading the model");
            const auto t0 = Clock::now();
            std::string error;
            std::unique_ptr<Loaded> m = loadModel(params, error);
            if (!m) {
                r.error = cancel->load() ? "cancelled" : error;
                finish(r);
                return;
            }
            r.loadSeconds = secondsBetween(t0, Clock::now());
            r.backend = "CPU, " + std::to_string(settings.threads) + " threads";
            for (size_t d = 0; settings.useGpu && d < ggml_backend_dev_count(); ++d) {
                ggml_backend_dev_t dev = ggml_backend_dev_get(d);
                if (ggml_backend_dev_type(dev) != GGML_BACKEND_DEVICE_TYPE_CPU) {
                    r.backend = std::string(ggml_backend_dev_name(dev)) + ": " + ggml_backend_dev_description(dev);
                    break;
                }
            }
            llama_context* ctx = m->ctx();

            // 1. Prompt processing: one batch of promptTokens tokens.
            report("Processing a " + std::to_string(settings.promptTokens) + "-token prompt");
            std::string text;
            std::vector<llama_token> tokens;
            while (int(tokens.size()) < settings.promptTokens) {
                text += kPromptText;
                tokens = common_tokenize(ctx, text, true, true);
            }
            tokens.resize(size_t(settings.promptTokens));
            {
                common_batch batch(ctx);
                for (size_t i = 0; i < tokens.size(); ++i)
                    batch.add(tokens[i], llama_pos(i), 0, i + 1 == tokens.size());
                const auto t = Clock::now();
                llama_process(ctx, LLAMA_PROCESS_TYPE_DECODE, batch.get());
                const double s = secondsBetween(t, Clock::now());
                r.promptTokens = int(tokens.size());
                r.promptTokensPerSecond = s > 0 ? tokens.size() / s : 0;
            }
            if (cancel->load()) {
                r.error = "cancelled";
                finish(r);
                return;
            }

            // 2. Generation, continuing a piece of code.
            report(r.drafter ? "Generating with the drafter" : "Generating");
            std::string sample;
            const Generated g = generate(*m, common_tokenize(ctx, kGeneratePrompt, true, true), settings.generateTokens,
                                         *cancel, [&sample](const std::string& piece) {
                                             if (sample.size() < 400)
                                                 sample += piece;
                                         });
            if (cancel->load()) {
                r.error = "cancelled";
                finish(r);
                return;
            }
            r.firstTokenSeconds = g.firstTokenSeconds;
            r.drafted = g.drafted;
            r.accepted = g.accepted;
            r.generatedTokens = g.produced;
            // The first token's time is in firstTokenSeconds; the rate is over the rest.
            r.generateTokensPerSecond = g.decodeSeconds > 0 && g.produced > 1 ? (g.produced - 1) / g.decodeSeconds : 0;
            r.sample = sample;
            r.peakMemory = peakMemory();
            r.ok = true;
        } catch (const std::exception& e) {
            r.error = std::string("engine error: ") + e.what();
        }
        finish(r);
    }).detach();
#endif
}

void LocalEngineService::chat(const interfaces::LocalChatRequest& request, std::function<void(const std::string& stage)> stage,
                              std::function<void(const std::string& text)> text,
                              std::function<void(const interfaces::LocalChatResult& result)> done)
{
    auto alive = alive_;
    auto chatting = chatting_;
    auto finish = [alive, chatting, done](interfaces::LocalChatResult result) {
        os::dispatch::postToMain([alive, chatting, done, result = std::move(result)]() {
            chatting->store(false);
            if (alive->load() && done)
                done(result);
        });
    };
#if !ENP_HAS_LOCAL_ENGINE
    (void)request;
    (void)stage;
    (void)text;
    chatting_->store(true);
    interfaces::LocalChatResult r;
    r.error = info().unavailableReason;
    finish(r);
#else
    // Refused at once (done is called before returning): no engine, a lab run, or still answering.
    if (!info().available || running_->load() || chatting_->exchange(true)) {
        interfaces::LocalChatResult r;
        r.error = !info().available ? info().unavailableReason
                  : running_->load() ? "the Engine Lab is running a test" : "the local agent is still answering";
        if (done)
            done(r);
        return;
    }
    cancel_->store(false);
    auto cancel = cancel_;
    auto model = chat_;
    auto post = [alive](std::function<void(const std::string&)> to, std::string what) {
        os::dispatch::postToMain([alive, to = std::move(to), what = std::move(what)]() {
            if (alive->load() && to)
                to(what);
        });
    };
    std::thread([request, stage, text, finish, cancel, model, post]() {
        interfaces::LocalChatResult r;
        try {
            std::lock_guard<std::mutex> lock(model->mutex);
            const std::string key = request.modelPath + "|" + request.drafterPath + "|" + std::to_string(request.threads) + "|" +
                                    (request.useGpu ? "gpu" : "cpu") + "|" + std::to_string(request.contextTokens);
            if (!model->loaded || model->key != key) {
                model->unload();
                post(stage, "Loading the local model");
                common_params params = engineParams(request.modelPath, request.drafterPath, request.threads, request.useGpu,
                                                    request.contextTokens, request.contextTokens, cancel.get());
                params.sampling.temp = 0.3f; // a little variety; greedy answers from a small model loop easily
                params.sampling.top_p = 0.95f;
                std::string error;
                model->loaded = loadModel(params, error);
                if (!model->loaded) {
                    r.error = cancel->load() ? "cancelled" : error;
                    finish(r);
                    return;
                }
                model->key = key;
                r.loaded = true;
            }
            Loaded& m = *model->loaded;
            const std::vector<llama_token> prompt = chatTokens(m, request.messages);
            r.promptTokens = int(prompt.size());
            if (int(prompt.size()) + request.maxTokens > int(llama_n_ctx(m.ctx()))) {
                r.error = "the conversation is too long for the local model: start a new chat";
                finish(r);
                return;
            }
            post(stage, "Reading " + std::to_string(prompt.size()) + " tokens");
            bool started = false;
            const Generated g = generate(m, prompt, request.maxTokens, *cancel, [&](const std::string& piece) {
                if (!started) {
                    started = true;
                    post(stage, "Writing");
                }
                post(text, piece);
            });
            model->lastUse = std::chrono::steady_clock::now();
            if (cancel->load()) {
                r.error = "stopped";
                finish(r);
                return;
            }
            r.generatedTokens = g.produced;
            r.firstTokenSeconds = g.firstTokenSeconds;
            r.tokensPerSecond = g.decodeSeconds > 0 && g.produced > 1 ? (g.produced - 1) / g.decodeSeconds : 0;
            r.ok = true;
        } catch (const std::exception& e) {
            r.error = std::string("engine error: ") + e.what();
        }
        finish(r);
    }).detach();
#endif
}

void LocalEngineService::unloadIfIdle(double idleSeconds)
{
    if (chatting_->load() || !chat_->isLoaded())
        return;
    std::unique_lock<std::mutex> lock(chat_->mutex, std::try_to_lock);
    if (!lock.owns_lock())
        return;
    if (std::chrono::duration<double>(std::chrono::steady_clock::now() - chat_->lastUse).count() >= idleSeconds)
        chat_->unload();
}


void LocalEngineService::cancel() { cancel_->store(true); }

std::string LocalEngineService::modelsDirectory() { return os::download::directory(); }

uint64_t LocalEngineService::freeSpace(const std::string& path)
{
    std::error_code ec;
    std::filesystem::path p = std::filesystem::u8path(path);
    while (!p.empty() && !std::filesystem::exists(p, ec))
        p = p.parent_path();
    const auto space = std::filesystem::space(p, ec);
    return ec ? 0 : uint64_t(space.available);
}

uint64_t LocalEngineService::fileSize(const std::string& path) { return os::fs::stat(path).size; }

bool LocalEngineService::startDownload(const std::string& url, const std::string& path, bool allowMetered, std::string* error)
{
    return os::download::start(url, path, allowMetered, error);
}

DownloadStatus LocalEngineService::downloadStatus()
{
    const os::download::Status s = os::download::status();
    DownloadStatus d;
    d.state = DownloadStatus::State(int(s.state));
    d.received = s.received;
    d.total = s.total;
    d.error = s.error;
    return d;
}

void LocalEngineService::cancelDownload() { os::download::cancel(); }

bool LocalEngineService::meteredNetwork() { return os::download::meteredNetwork(); }

bool LocalEngineService::canImport() { return os::download::canImport(); }

bool LocalEngineService::importFile(const std::string& path, std::string* error) { return os::download::importFile(path, error); }

void LocalEngineService::hashFile(const std::string& path, std::function<void(double fraction)> progress,
                                  std::function<void(const std::string& sha256)> done)
{
    auto alive = alive_;
    std::thread([alive, path, progress, done]() {
        std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
        std::string digest;
        if (in) {
            const uint64_t total = os::fs::stat(path).size;
            model::Sha256 hash;
            std::vector<char> buffer(4 << 20);
            uint64_t read = 0, reported = 0;
            while (in) {
                in.read(buffer.data(), std::streamsize(buffer.size()));
                const auto n = size_t(in.gcount());
                if (n == 0)
                    break;
                hash.update(buffer.data(), n);
                read += n;
                if (read - reported >= (64u << 20) && total > 0) {
                    reported = read;
                    const double fraction = double(read) / double(total);
                    os::dispatch::postToMain([alive, progress, fraction]() {
                        if (alive->load() && progress)
                            progress(fraction);
                    });
                }
            }
            digest = hash.hexDigest();
        }
        os::dispatch::postToMain([alive, done, digest]() {
            if (alive->load() && done)
                done(digest);
        });
    }).detach();
}

} // namespace enp::services
