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
#include "build-info.h"
#include "common.h"
#include "ggml-backend.h"
#include "llama.h"
#include "log.h"
#include "sampling.h"
#include "speculative.h"
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
#endif

} // namespace

LocalEngineService::LocalEngineService() = default;

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
    std::thread([settings, report, finish, cancel]() {
        EngineRunResult r;
        r.drafter = !settings.drafterPath.empty();
        try {
            using clock = std::chrono::steady_clock;
            auto seconds = [](clock::time_point a, clock::time_point b) { return std::chrono::duration<double>(b - a).count(); };
            resetPeakMemory();

            common_params params;
            params.model.path = settings.modelPath;
            params.n_ctx = settings.contextTokens;
            params.n_batch = std::max(512, settings.promptTokens);
            params.n_ubatch = 512;
            params.cpuparams.n_threads = settings.threads;
            params.cpuparams_batch.n_threads = settings.threads;
            params.n_gpu_layers = settings.useGpu ? 999 : 0;
            params.sampling.temp = 0.0f; // greedy: the same text every run
            params.load_progress_callback = [](float, void* flag) { return !static_cast<std::atomic<bool>*>(flag)->load(); };
            params.load_progress_callback_user_data = cancel.get();
            if (r.drafter) {
                params.speculative.types = {COMMON_SPECULATIVE_TYPE_DRAFT_MTP};
                params.speculative.draft.mparams.path = settings.drafterPath;
                params.speculative.draft.n_gpu_layers = settings.useGpu ? 999 : 0;
                const auto limits = common_speculative_get_output_limits(params.n_batch, params.n_parallel,
                                                                         common_speculative_n_max(&params.speculative));
                params.n_outputs_max = limits.total;
                params.n_outputs_max_per_seq = limits.per_seq;
            }

            report("Loading the model");
            const auto t0 = clock::now();
            auto init = common_init_from_params(params);
            llama_model* model = init ? init->model() : nullptr;
            llama_context* ctx = init ? init->context() : nullptr;
            if (!model || !ctx) {
                r.error = cancel->load() ? "cancelled" : "the model could not be loaded (file, memory or format)";
                finish(r);
                return;
            }
            r.loadSeconds = seconds(t0, clock::now());
            r.backend = settings.useGpu ? "GPU" : "CPU";
            for (size_t d = 0; d < ggml_backend_dev_count(); ++d) {
                ggml_backend_dev_t dev = ggml_backend_dev_get(d);
                const bool gpu = ggml_backend_dev_type(dev) != GGML_BACKEND_DEVICE_TYPE_CPU;
                if (settings.useGpu && gpu) {
                    r.backend = std::string(ggml_backend_dev_name(dev)) + ": " + ggml_backend_dev_description(dev);
                    break;
                }
            }
            if (!settings.useGpu)
                r.backend = "CPU, " + std::to_string(settings.threads) + " threads";
            const llama_vocab* vocab = llama_model_get_vocab(model);

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
                const auto t = clock::now();
                llama_process(ctx, LLAMA_PROCESS_TYPE_DECODE, batch.get());
                const double s = seconds(t, clock::now());
                r.promptTokens = int(tokens.size());
                r.promptTokensPerSecond = s > 0 ? tokens.size() / s : 0;
            }
            llama_memory_clear(llama_get_memory(ctx), true);
            if (cancel->load()) {
                r.error = "cancelled";
                finish(r);
                return;
            }

            // 2. Generation, as llama.cpp's speculative example does it (the drafter, when there is
            //    one, proposes tokens; the model checks them in one batch and keeps those it agrees with).
            report(r.drafter ? "Generating with the drafter" : "Generating");
            common_speculative_init_result_ptr specInit;
            llama_context* ctxDraft = nullptr;
            common_speculative* spec = nullptr;
            if (r.drafter) {
                common_params draftParams = common_base_params_to_speculative(params);
                specInit = common_speculative_init_from_params(draftParams, model, ctx);
                params.speculative.draft.ctx_tgt = ctx;
                params.speculative.draft.ctx_dft = specInit ? specInit->context() : nullptr;
                ctxDraft = params.speculative.draft.ctx_dft;
                spec = common_speculative_init(params.speculative, 1);
                if (!spec) {
                    r.error = "the drafter could not be used with this model";
                    finish(r);
                    return;
                }
            }
            const bool ckptTarget = common_context_can_seq_rm(ctx) == COMMON_CONTEXT_SEQ_RM_TYPE_FULL;
            const bool ckptDraft = ctxDraft && common_context_can_seq_rm(ctxDraft) == COMMON_CONTEXT_SEQ_RM_TYPE_FULL;

            const auto tFirst = clock::now();
            std::vector<llama_token> inp = common_tokenize(ctx, kGeneratePrompt, true, true);
            common_sampler_ptr sampler(common_sampler_init(model, params.sampling));
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
            std::string generated;
            int produced = 0;
            bool first = true;
            clock::time_point tDecode = clock::now();
            while (produced < settings.generateTokens && !cancel->load()) {
                if (spec && draft.empty()) {
                    ckpt.update_pos(context.size(), llama_memory_seq_pos_min(llama_get_memory(ctx), seq),
                                    llama_memory_seq_pos_max(llama_get_memory(ctx), seq));
                    if (ckptDraft)
                        ckpt.update_dft(ctxDraft, seq, LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY);
                    const int room = std::max(0, std::min(int(llama_n_ctx(ctx)) - past - 2, settings.generateTokens - produced - 1));
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
                    r.firstTokenSeconds = seconds(tFirst, clock::now());
                    tDecode = clock::now(); // generation speed excludes the prompt
                    first = false;
                }
                if (spec && ckptTarget && ids.size() - 1 < drafted) {
                    // Partly accepted and the context cannot drop a tail: go back to the checkpoint
                    // and evaluate the accepted part as the next draft.
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
                r.drafted += int(drafted);
                r.accepted += int(ids.size()) - 1;
                produced += int(ids.size());
                bool end = false;
                for (llama_token id : ids) {
                    context.push_back(last);
                    last = id;
                    if (llama_vocab_is_eog(vocab, id)) {
                        end = true;
                        break;
                    }
                    if (generated.size() < 400)
                        generated += common_token_to_piece(ctx, id);
                }
                draft.clear();
                llama_memory_seq_rm(llama_get_memory(ctx), seq, past, -1);
                if (ctxDraft)
                    llama_memory_seq_rm(llama_get_memory(ctxDraft), seq, past, -1);
                if (end)
                    break;
            }
            const double decode = seconds(tDecode, clock::now());
            if (spec)
                common_speculative_free(spec);
            if (cancel->load()) {
                r.error = "cancelled";
                finish(r);
                return;
            }
            r.generatedTokens = produced;
            // The first token's time is in firstTokenSeconds; the rate is over the rest.
            r.generateTokensPerSecond = decode > 0 && produced > 1 ? (produced - 1) / decode : 0;
            r.sample = generated;
            r.peakMemory = peakMemory();
            r.ok = true;
        } catch (const std::exception& e) {
            r.error = std::string("engine error: ") + e.what();
        }
        finish(r);
    }).detach();
#endif
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
