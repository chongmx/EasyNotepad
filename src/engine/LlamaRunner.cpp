#include "enp/engine/LlamaRunner.h"

#include <algorithm>
#include <atomic>
#include <stdexcept>

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

namespace enp::engine {

using interfaces::EngineDevice;
using interfaces::EngineRunResult;
using interfaces::EngineRunSettings;

namespace {

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

// What the model loader asks while it reads: stop? (and how far it is, for the stage line).
struct LoadWatch {
    const Cancelled* cancelled = nullptr;
    const Report* stage = nullptr;
    std::string label;
    int lastTenth = -1;
};

bool loadProgress(float progress, void* user)
{
    LoadWatch& w = *static_cast<LoadWatch*>(user);
    const int tenth = int(progress * 10.0f);
    if (w.stage && *w.stage && !w.label.empty() && tenth != w.lastTenth && tenth > 0 && tenth < 10) {
        w.lastTenth = tenth;
        (*w.stage)(w.label + " (" + std::to_string(tenth * 10) + "%)");
    }
    return !(w.cancelled && *w.cancelled && (*w.cancelled)());
}

// Parameters shared by the lab's runs and the agent's chats.
common_params engineParams(const std::string& modelPath, const std::string& drafterPath, int threads, bool gpu,
                           int contextTokens, int batchTokens, LoadWatch* watch)
{
    common_params params;
    params.model.path = modelPath;
    params.n_ctx = contextTokens;
    params.n_batch = std::max(512, batchTokens);
    params.n_ubatch = 512;
    params.cpuparams.n_threads = threads;
    params.cpuparams_batch.n_threads = threads;
    params.n_gpu_layers = gpu ? 999 : 0;
    params.load_progress_callback = loadProgress;
    params.load_progress_callback_user_data = watch;
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
struct Model {
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
std::unique_ptr<Model> loadModel(common_params params, std::string& error)
{
    auto m = std::make_unique<Model>();
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
    params.load_progress_callback = nullptr; // its watch lives only while loading
    params.load_progress_callback_user_data = nullptr;
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
Generated generate(Model& m, const std::vector<llama_token>& inp, int maxTokens, const Cancelled& cancelled,
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
    while (g.produced < maxTokens && !cancelled()) {
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
std::vector<llama_token> chatTokens(Model& m, const std::vector<interfaces::LocalChatMessage>& messages)
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
} // namespace

void initBackend()
{
    static const bool once = []() {
        // The lab and the agent show what matters; nothing goes to stdout or stderr.
        llama_log_set([](ggml_log_level, const char*, void*) {}, nullptr);
        common_log_set_stdo(common_log_main(), false);
        llama_backend_init();
        return true;
    }();
    (void)once;
}

std::string version() { return "llama.cpp b" + std::to_string(llama_build_number()) + " (" + llama_commit() + ")"; }

std::vector<EngineDevice> devices()
{
    std::vector<EngineDevice> out;
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
        out.push_back(std::move(e));
    }
    return out;
}

struct Runner::Loaded {
    std::unique_ptr<Model> model;
};

Runner::Runner() = default;
Runner::~Runner() = default;

void Runner::unload()
{
    loaded_.reset();
    key_.clear();
}

bool Runner::loaded() const { return loaded_ != nullptr; }

interfaces::LocalChatResult Runner::chat(const interfaces::LocalChatRequest& request, const Report& stage, const Report& text,
                                         const Cancelled& cancelled)
{
    interfaces::LocalChatResult r;
    try {
        initBackend();
        const std::string key = request.modelPath + "|" + request.drafterPath + "|" + std::to_string(request.threads) + "|" +
                                (request.useGpu ? "gpu" : "cpu") + "|" + std::to_string(request.contextTokens);
        if (!loaded_ || key_ != key) {
            unload();
            stage("Loading the local model");
            LoadWatch watch{&cancelled, &stage, "Loading the local model"};
            common_params params = engineParams(request.modelPath, request.drafterPath, request.threads, request.useGpu,
                                                request.contextTokens, request.contextTokens, &watch);
            params.sampling.temp = 0.3f; // a little variety; greedy answers from a small model loop easily
            params.sampling.top_p = 0.95f;
            std::string error;
            std::unique_ptr<Model> m = loadModel(params, error);
            if (!m) {
                r.error = cancelled() ? "cancelled" : error;
                return r;
            }
            loaded_ = std::make_unique<Loaded>();
            loaded_->model = std::move(m);
            key_ = key;
            r.loaded = true;
        }
        Model& m = *loaded_->model;
        const std::vector<llama_token> prompt = chatTokens(m, request.messages);
        r.promptTokens = int(prompt.size());
        if (int(prompt.size()) + request.maxTokens > int(llama_n_ctx(m.ctx()))) {
            r.error = "the conversation is too long for the local model: start a new chat";
            return r;
        }
        stage("Reading " + std::to_string(prompt.size()) + " tokens");
        bool started = false;
        const Generated g = generate(m, prompt, request.maxTokens, cancelled, [&](const std::string& piece) {
            if (!started) {
                started = true;
                stage("Writing");
            }
            text(piece);
        });
        lastUse_ = std::chrono::steady_clock::now();
        if (cancelled()) {
            r.error = "stopped";
            return r;
        }
        r.generatedTokens = g.produced;
        r.firstTokenSeconds = g.firstTokenSeconds;
        r.tokensPerSecond = g.decodeSeconds > 0 && g.produced > 1 ? (g.produced - 1) / g.decodeSeconds : 0;
        r.ok = true;
    } catch (const std::exception& e) {
        r.error = std::string("engine error: ") + e.what();
    }
    return r;
}

EngineRunResult Runner::run(const EngineRunSettings& settings, const Report& stage, const Cancelled& cancelled)
{
    EngineRunResult r;
    r.drafter = !settings.drafterPath.empty();
    unload(); // one model in memory at a time (a phone has little spare)
    try {
        initBackend();
        LoadWatch watch{&cancelled, nullptr, {}};
        common_params params = engineParams(settings.modelPath, settings.drafterPath, settings.threads, settings.useGpu,
                                            settings.contextTokens, settings.promptTokens, &watch);
        params.sampling.temp = 0.0f; // greedy: the same text every run

        stage("Loading the model");
        const auto t0 = Clock::now();
        std::string error;
        std::unique_ptr<Model> m = loadModel(params, error);
        if (!m) {
            r.error = cancelled() ? "cancelled" : error;
            return r;
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
        stage("Processing a " + std::to_string(settings.promptTokens) + "-token prompt");
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
        if (cancelled()) {
            r.error = "cancelled";
            return r;
        }

        // 2. Generation, continuing a piece of code.
        stage(r.drafter ? "Generating with the drafter" : "Generating");
        std::string sample;
        const Generated g = generate(*m, common_tokenize(ctx, kGeneratePrompt, true, true), settings.generateTokens, cancelled,
                                     [&sample](const std::string& piece) {
                                         if (sample.size() < 400)
                                             sample += piece;
                                     });
        if (cancelled()) {
            r.error = "cancelled";
            return r;
        }
        r.firstTokenSeconds = g.firstTokenSeconds;
        r.drafted = g.drafted;
        r.accepted = g.accepted;
        r.generatedTokens = g.produced;
        // The first token's time is in firstTokenSeconds; the rate is over the rest.
        r.generateTokensPerSecond = g.decodeSeconds > 0 && g.produced > 1 ? (g.produced - 1) / g.decodeSeconds : 0;
        r.sample = sample;
        r.ok = true;
    } catch (const std::exception& e) {
        r.error = std::string("engine error: ") + e.what();
    }
    return r;
}

} // namespace enp::engine
