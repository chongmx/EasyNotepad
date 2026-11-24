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
#include "mtmd-helper.h"
#include "mtmd.h"
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
constexpr int kMinFitContext = 16384; // the least context a GPU fit keeps: the agent's prompt, documents, answers

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
    // On the GPU, llama.cpp fits the layers and the context to free memory (both left to it: -1). The
    // context comes first, down to kMinFitContext: the agent's opening prompt alone is about 5,000
    // tokens, so a model that fills the card (Gemma 4 12B on 8 GB) keeps some layers on the CPU
    // rather than a context too short to answer in (it had got 4,096 and fell back to the CPU).
    params.n_gpu_layers = gpu ? -1 : 0;
    params.fit_params_min_ctx = kMinFitContext;
    params.load_progress_callback = loadProgress;
    params.load_progress_callback_user_data = watch;
    if (!drafterPath.empty()) {
        params.speculative.types = {COMMON_SPECULATIVE_TYPE_DRAFT_MTP};
        params.speculative.draft.mparams.path = drafterPath;
        params.speculative.draft.n_gpu_layers = gpu ? -1 : 0; // fitted with the model
        const auto limits = common_speculative_get_output_limits(params.n_batch, params.n_parallel,
                                                                 common_speculative_n_max(&params.speculative));
        params.n_outputs_max = limits.total;
        params.n_outputs_max_per_seq = limits.per_seq;
    }
    return params;
}

// The tokens one batch must hold for the projector's pictures: its largest picture when it reads a
// picture all at once (non-causal attention), 0 when it reads them in any number of batches. Read from
// the projector file alone (nothing loaded).
int wholePictureTokens(const std::string& projectorPath)
{
    if (projectorPath.empty())
        return 0;
    mtmd_context_params mp = mtmd_context_params_default();
    mp.use_gpu = false;
    mp.warmup = false;
    mp.print_timings = false;
    const mtmd_memory_usage usage = mtmd_get_memory_usage(projectorPath.c_str(), mp);
    if (!usage.use_non_causal)
        return 0;
    return usage.image_max_tokens > 0 ? usage.image_max_tokens : 2048;
}

// A model in memory, with its drafter when there is one.
struct Model {
    common_params params;
    common_init_result_ptr init;
    common_speculative_init_result_ptr specInit;
    common_chat_templates_ptr templates; // the agent's chat format (loaded when first needed)
    llama_context* ctxDraft = nullptr;
    // The projector (the model's eyes and ears), loaded with the first picture or sound: about 1 GB
    // a chat without them does not need.
    mtmd::context_ptr media;
    std::string mediaPath;
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

// What the model reads before it answers: tokens, or, with pictures and sounds, mtmd's chunks
// (text tokens and the projector's embeddings in turn).
struct Prompt {
    std::vector<llama_token> tokens;
    mtmd::input_chunks_ptr chunks;
    size_t size() const { return chunks ? mtmd_helper_get_n_tokens(chunks.get()) : tokens.size(); }
};

struct Generated {
    int produced = 0;
    int drafted = 0;
    int accepted = 0;
    double firstTokenSeconds = 0; // the prompt processed and the first token sampled
    double decodeSeconds = 0;     // after the first token
    bool looped = false;          // stopped going round in a loop
};

// What reading the model's own output threw (a tool call that is not JSON, brackets nested more
// than 64 deep): the model's fault, not the engine's, so the app keeps the GPU and asks again.
struct ModelOutputError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
template <class Read>
decltype(auto) readingOutput(Read read)
{
    try {
        return read();
    } catch (const std::exception& e) {
        throw ModelOutputError(e.what());
    }
}

// Whether the last of `tokens` go round in a loop: one block of 1 to 300 tokens over and over, at
// least four times and 200 tokens in all. A small model at temperature 0 can write the same lines
// until its limit (8,192 tokens: ten minutes on a slow GPU, an hour on a CPU).
bool loops(const std::vector<llama_token>& tokens)
{
    const size_t n = tokens.size();
    for (size_t period = 1; period <= 300 && period * 4 <= n; ++period) {
        const size_t span = std::max<size_t>(period * 4, 200);
        if (span > n)
            continue;
        bool same = true;
        for (size_t i = n - span; i + period < n && same; ++i)
            same = tokens[i] == tokens[i + period];
        if (same)
            return true;
    }
    return false;
}

// Generates up to `maxTokens` after the prompt `inp`, from an empty context, as llama.cpp's
// speculative example does: the drafter (when loaded) proposes tokens, the model checks them in
// one batch and keeps those it agrees with. Each piece of text goes to `onPiece` (this thread).
// A prompt with pictures or sounds is read by mtmd, and the drafter sits out (it cannot follow the
// projector's embeddings); the first token is then sampled from mtmd's last logits.
Generated generate(Model& m, const Prompt& prompt, int maxTokens, common_params_sampling sampling,
                   const Cancelled& cancelled, const std::function<void(const std::string&, llama_token)>& onPiece)
{
    Generated g;
    llama_context* ctx = m.ctx();
    llama_context* ctxDraft = m.ctxDraft;
    const llama_vocab* vocab = llama_model_get_vocab(m.model());
    llama_memory_clear(llama_get_memory(ctx), true);
    if (ctxDraft)
        llama_memory_clear(llama_get_memory(ctxDraft), true);
    if (prompt.size() == 0)
        return g;
    const std::vector<llama_token>& inp = prompt.tokens;
    const bool media = prompt.chunks != nullptr;

    common_speculative* spec = nullptr;
    if (m.drafter() && !media) {
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
    common_sampler_ptr sampler(common_sampler_init(m.model(), sampling));
    const llama_seq_id seq = 0;
    llama_token last = LLAMA_TOKEN_NULL;
    llama_tokens context;
    int past = 0;
    bool first = true;
    Clock::time_point tDecode = Clock::now();
    if (media) {
        // A picture read all at once must fit one batch: refused here, where llama.cpp would abort.
        for (size_t i = 0, n = mtmd_input_chunks_size(prompt.chunks.get()); i < n; ++i) {
            const mtmd_input_chunk* chunk = mtmd_input_chunks_get(prompt.chunks.get(), i);
            if (mtmd_input_chunk_get_type(chunk) != MTMD_INPUT_CHUNK_TYPE_TEXT &&
                mtmd_decode_use_non_causal(m.media.get(), chunk) &&
                mtmd_input_chunk_get_n_tokens(chunk) > size_t(llama_n_ubatch(ctx)))
                throw std::runtime_error("a picture is larger than the model reads at once (" +
                                         std::to_string(mtmd_input_chunk_get_n_tokens(chunk)) + " tokens)");
        }
        llama_pos newPast = 0;
        if (mtmd_helper_eval_chunks(m.media.get(), ctx, prompt.chunks.get(), 0, seq, int32_t(llama_n_batch(ctx)), true,
                                    &newPast) != 0)
            throw std::runtime_error("the pictures or sounds could not be read");
        if (cancelled())
            return g;
        last = common_sampler_sample(sampler.get(), ctx, -1);
        common_sampler_accept(sampler.get(), last, true);
        g.firstTokenSeconds = secondsBetween(tFirst, Clock::now());
        tDecode = Clock::now();
        first = false;
        g.produced = 1;
        past = int(newPast);
        if (llama_vocab_is_eog(vocab, last)) {
            g.decodeSeconds = 0;
            return g;
        }
        onPiece(common_token_to_piece(ctx, last), last);
    } else {
        // The prompt (all but its last token) in pieces of the context's batch size: a batch holds no
        // more (a longer one aborted in llama.cpp).
        const size_t step = std::max<size_t>(1, llama_n_batch(ctx));
        common_batch batch(ctx);
        for (size_t from = 0; from + 1 < inp.size() && !cancelled(); from += step) {
            batch.clear();
            for (size_t i = from; i < std::min(from + step, inp.size() - 1); ++i)
                batch.add(inp[i], llama_pos(i), seq, false);
            llama_process(ctx, LLAMA_PROCESS_TYPE_DECODE, batch.get());
            if (spec && !common_speculative_process(spec, batch))
                throw std::runtime_error("the drafter failed on the prompt");
        }
        if (cancelled())
            return g;
        last = inp.back();
        context.assign(inp.begin(), inp.end() - 1);
        past = int(inp.size()) - 1;
    }
    if (spec)
        common_speculative_begin(spec, seq, context);
    common_batch batch(ctx);
    llama_tokens draft;
    llama_tokens written; // the answer's tokens, for loops()
    common_prompt_checkpoint ckpt;
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
            onPiece(common_token_to_piece(ctx, id), id);
            written.push_back(id);
            if (written.size() % 32 == 0 && loops(written)) {
                g.looped = end = true;
                break;
            }
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

constexpr int kCpuContextTokens = 32768; // the CPU's context when the largest that fits is asked for

// Where a message's picture or sound goes in its text (the projector's tokens replace it). Not a
// token of the model's, and not text anyone types.
const char* const kMediaMarker = "<\x01enp-media\x01>";

bool hasMedia(const std::vector<interfaces::LocalChatMessage>& messages)
{
    return std::any_of(messages.begin(), messages.end(), [](const interfaces::LocalChatMessage& m) { return !m.media.empty(); });
}

// The conversation in the template's terms: each picture or sound is a marker before the text.
common_chat_templates_inputs chatInputs(const interfaces::LocalChatRequest& request)
{
    common_chat_templates_inputs inputs;
    for (const interfaces::LocalChatMessage& message : request.messages) {
        common_chat_msg msg;
        msg.role = message.role;
        for (size_t i = 0; i < message.media.size(); ++i)
            msg.content += kMediaMarker;
        msg.content += message.text;
        for (const interfaces::LocalToolCall& call : message.toolCalls)
            msg.tool_calls.push_back({call.name, call.arguments, call.id});
        msg.tool_name = message.toolName;
        msg.tool_call_id = message.toolCallId;
        inputs.messages.push_back(std::move(msg));
    }
    for (const interfaces::LocalTool& tool : request.tools)
        inputs.tools.push_back({tool.name, tool.description, tool.parameters});
    inputs.parallel_tool_calls = !request.tools.empty();
    inputs.add_generation_prompt = true;
    inputs.enable_thinking = request.thinking; // off for quick answers (thinking used the whole budget in a trial); on for plans
    inputs.reasoning_format = COMMON_REASONING_FORMAT_AUTO; // thinking it writes anyway goes apart, not into the answer
    return inputs;
}

// The messages' pictures and sounds as mtmd's bitmaps, in order (a sound decoded to the encoder's
// samples: WAV, MP3, FLAC).
std::vector<mtmd::bitmap_ptr> mediaBitmaps(Model& m, const std::vector<interfaces::LocalChatMessage>& messages)
{
    std::vector<mtmd::bitmap_ptr> bitmaps;
    for (const interfaces::LocalChatMessage& message : messages)
        for (const interfaces::LocalMedia& media : message.media) {
            mtmd_bitmap* b = nullptr;
            if (media.kind == interfaces::LocalMedia::Kind::Image) {
                if (!mtmd_support_vision(m.media.get()))
                    throw std::runtime_error("this model's projector cannot see pictures");
                if (media.width <= 0 || media.height <= 0 || media.data.size() != size_t(media.width) * size_t(media.height) * 3)
                    throw std::runtime_error("the picture " + media.name + " is not whole");
                b = mtmd_bitmap_init(uint32_t(media.width), uint32_t(media.height),
                                     reinterpret_cast<const unsigned char*>(media.data.data()));
            } else {
                if (!mtmd_support_audio(m.media.get()))
                    throw std::runtime_error("this model's projector cannot hear sounds");
                b = mtmd_helper_bitmap_init_from_buf(m.media.get(), reinterpret_cast<const unsigned char*>(media.data.data()),
                                                     media.data.size(), false, mtmd_helper_init_opt_default())
                        .bitmap;
                if (!b)
                    throw std::runtime_error("the sound " + media.name + " could not be read (WAV, MP3 and FLAC can)");
            }
            bitmaps.emplace_back(b);
        }
    return bitmaps;
}

// The prompt the template made, as tokens (template text may hold special tokens, the user's text
// may not), or with `bitmaps` at the media markers as mtmd chunks.
Prompt chatPrompt(Model& m, const common_chat_prompt& formatted, const std::vector<mtmd::bitmap_ptr>& bitmaps, int& mediaTokens)
{
    Prompt p;
    const llama_vocab* vocab = llama_model_get_vocab(m.model());
    if (bitmaps.empty()) {
        for (const common_chat_prompt_part& part : formatted.parts) {
            const std::vector<llama_token> t = common_tokenize(vocab, part.text, false, !part.is_input);
            p.tokens.insert(p.tokens.end(), t.begin(), t.end());
        }
        return p;
    }

    // Text and media in order: the strings first, so the parts can point into them.
    struct Segment {
        std::string text;
        bool special = false;
        int bitmap = -1;
    };
    std::vector<Segment> segments;
    int next = 0;
    const std::string marker = kMediaMarker;
    for (const common_chat_prompt_part& part : formatted.parts) {
        size_t from = 0;
        for (size_t at = part.text.find(marker); at != std::string::npos; at = part.text.find(marker, from)) {
            if (at > from)
                segments.push_back({part.text.substr(from, at - from), !part.is_input, -1});
            if (next >= int(bitmaps.size()))
                throw std::runtime_error("the conversation has more media markers than pictures and sounds");
            segments.push_back({std::string(), false, next++});
            from = at + marker.size();
        }
        if (from < part.text.size())
            segments.push_back({part.text.substr(from), !part.is_input, -1});
    }
    if (next != int(bitmaps.size()))
        throw std::runtime_error("a picture or sound was left out of the conversation");
    std::vector<mtmd_input_text> texts(segments.size());
    std::vector<mtmd_input_part> parts(segments.size());
    std::vector<const mtmd_input_part*> partPointers(segments.size());
    for (size_t i = 0; i < segments.size(); ++i) {
        if (segments[i].bitmap >= 0) {
            parts[i] = {nullptr, bitmaps[size_t(segments[i].bitmap)].get()};
        } else {
            texts[i] = {segments[i].text.c_str(), segments[i].text.size(), false, segments[i].special};
            parts[i] = {&texts[i], nullptr};
        }
        partPointers[i] = &parts[i];
    }
    p.chunks.reset(mtmd_input_chunks_init());
    if (mtmd_tokenize_from_parts(m.media.get(), p.chunks.get(), partPointers.data(), partPointers.size(), false) != 0)
        throw std::runtime_error("the pictures or sounds could not be prepared for the model");
    mediaTokens = 0;
    for (size_t i = 0; i < mtmd_input_chunks_size(p.chunks.get()); ++i) {
        const mtmd_input_chunk* chunk = mtmd_input_chunks_get(p.chunks.get(), i);
        if (mtmd_input_chunk_get_type(chunk) != MTMD_INPUT_CHUNK_TYPE_TEXT)
            mediaTokens += int(mtmd_input_chunk_get_n_tokens(chunk));
    }
    return p;
}
// The answer without a thinking channel Gemma writes now and then even with thinking off
// ("<|channel>thought ... <channel|>"): not for the user.
std::string withoutChannels(std::string text)
{
    for (size_t open = text.find("<|channel>"); open != std::string::npos; open = text.find("<|channel>", open)) {
        const size_t close = text.find("<channel|>", open);
        text.erase(open, close == std::string::npos ? std::string::npos : close + 10 - open);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.erase(text.begin());
    return text;
}

std::string trimText(std::string s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start])))
        ++start;
    return s.substr(start);
}

// A window written all in capitals or all in small letters (the model does either now and then) as
// sentences: lower case, a capital after . ! ? and at the start, and "I". Mixed case is left alone.
std::string sentenceCase(std::string s)
{
    bool upper = false, lower = false;
    for (char c : s) {
        upper = upper || std::isupper(static_cast<unsigned char>(c));
        lower = lower || std::islower(static_cast<unsigned char>(c));
    }
    if (upper && lower)
        return s;
    bool start = true;
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 0x80) { // not ASCII: left as written
            start = false;
            continue;
        }
        if (std::isalpha(c)) {
            const bool word = (i == 0 || !std::isalnum(static_cast<unsigned char>(s[i - 1]))) &&
                              (i + 1 >= s.size() || !std::isalnum(static_cast<unsigned char>(s[i + 1])) || s[i + 1] == '\'');
            s[i] = char(start || (word && (c == 'i' || c == 'I')) ? std::toupper(c) : std::tolower(c));
            start = false;
        } else if (c == '.' || c == '!' || c == '?') {
            start = true;
        } else if (std::isdigit(c)) {
            start = false;
        }
    }
    return s;
}

// "[01:30]" for 90 seconds.
std::string clockText(int seconds)
{
    char buf[16];
    if (seconds >= 3600)
        std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", seconds / 3600, seconds / 60 % 60, seconds % 60);
    else
        std::snprintf(buf, sizeof(buf), "%02d:%02d", seconds / 60, seconds % 60);
    return buf;
}

// The words of each recording in the last message, written down window by window (the audio encoder
// takes up to 30 seconds at a time): each window a fresh context, greedy, its text streamed with the
// time it starts at ("[01:30] ..."). The whole transcript is the result's text.
void transcribeAudio(Model& m, const interfaces::LocalChatRequest& request, const Report& stage, const Report& text,
                     const Cancelled& cancelled, interfaces::LocalChatResult& r)
{
    if (request.messages.empty())
        throw std::runtime_error("nothing to transcribe");
    const interfaces::LocalChatMessage& last = request.messages.back();
    // A speech model (Qwen3-ASR: "<asr_text>" in its template) takes the sound alone and answers
    // "language English<asr_text>the words"; a chat model is told what to do.
    const bool speechModel = common_chat_templates_source(m.templates.get()).find("<asr_text>") != std::string::npos;
    const std::string instruction =
        speechModel ? std::string()
        : last.text.empty() ? std::string("Transcribe this audio word for word, in the language spoken, with punctuation. "
                                          "If there is no speech, write [no speech].")
                            : last.text;
    const int sampleRate = std::max(1, mtmd_get_audio_sample_rate(m.media.get()));
    const int window = std::clamp(request.audioWindowSeconds, 5, 30);
    common_params_sampling sampling = m.params.sampling;
    sampling.temp = 0.0f; // the same words every time
    // One message with one marker: the window's samples go there.
    interfaces::LocalChatRequest one;
    one.messages.push_back({"user", instruction});
    one.messages.back().media.push_back({interfaces::LocalMedia::Kind::Audio, "window", 0, 0, {}});
    for (const interfaces::LocalMedia& media : last.media) {
        if (media.kind != interfaces::LocalMedia::Kind::Audio)
            continue;
        mtmd::bitmap_ptr sound(mtmd_helper_bitmap_init_from_buf(m.media.get(), reinterpret_cast<const unsigned char*>(media.data.data()),
                                                                media.data.size(), false, mtmd_helper_init_opt_default())
                                   .bitmap);
        if (!sound || !mtmd_bitmap_is_audio(sound.get()))
            throw std::runtime_error("the sound " + media.name + " could not be read (WAV, MP3 and FLAC can)");
        const float* samples = reinterpret_cast<const float*>(mtmd_bitmap_get_data(sound.get()));
        const size_t count = mtmd_bitmap_get_n_bytes(sound.get()) / sizeof(float);
        const size_t step = size_t(window) * size_t(sampleRate);
        const int total = int(count / size_t(sampleRate));
        for (size_t from = 0; from < count && !cancelled(); from += step) {
            const size_t n = std::min(step, count - from);
            if (n < size_t(sampleRate) / 2)
                break; // half a second left: nothing to say
            const int at = int(from / size_t(sampleRate));
            stage("Transcribing " + media.name + ": " + clockText(at) + " of " + clockText(total));
            std::vector<mtmd::bitmap_ptr> bitmaps;
            bitmaps.emplace_back(mtmd_bitmap_init_from_audio(n, samples + from));
            // Each window on its own (told what came before, the model copied it instead of listening).
            common_chat_session session(m.templates.get(), llama_model_get_vocab(m.model()), chatInputs(one));
            int mediaTokens = 0;
            const Prompt prompt = chatPrompt(m, session.prompt(), bitmaps, mediaTokens);
            r.mediaTokens += mediaTokens;
            r.promptTokens += int(prompt.size());
            std::string words;
            const Generated g = generate(m, prompt, 512, sampling, cancelled,
                                         [&](const std::string& piece, llama_token) { words += piece; });
            // The speech model's language tag before its words; "language None": no speech.
            if (const size_t tag = words.find("<asr_text>"); tag != std::string::npos)
                words = trimText(words.substr(tag + 10)).empty() ? std::string("[no speech]") : words.substr(tag + 10);
            const std::string part = (r.text.empty() ? "" : "\n\n") + std::string("[") + clockText(at) + "] " + sentenceCase(trimText(words));
            r.text += part;
            text(part);
            r.generatedTokens += g.produced;
            if (r.firstTokenSeconds == 0)
                r.firstTokenSeconds = g.firstTokenSeconds;
            r.tokensPerSecond = g.decodeSeconds > 0 && g.produced > 1 ? (g.produced - 1) / g.decodeSeconds : r.tokensPerSecond;
        }
    }
    if (cancelled()) {
        r.error = "stopped";
        return;
    }
    r.ok = true;
}

} // namespace

void initBackend()
{
    static const bool once = []() {
        // The lab and the agent show what matters; nothing goes to stdout or stderr.
        llama_log_set([](ggml_log_level, const char*, void*) {}, nullptr);
        mtmd_log_set([](ggml_log_level, const char*, void*) {}, nullptr);
        mtmd_helper_log_set([](ggml_log_level, const char*, void*) {}, nullptr);
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
        // "As large as fits" is measured on a GPU (llama.cpp's fit, in the helper process: a failure there
        // only ends the helper). On the CPU, in the app's own process, a fixed size: fitting 128k tokens
        // into the RAM that happens to be free aborted the app.
        const int contextTokens = request.contextTokens > 0 ? request.contextTokens : request.useGpu ? 0 : kCpuContextTokens;
        const bool compact = request.compactCache && request.useGpu;
        const std::string key = request.modelPath + "|" + request.drafterPath + "|" + request.projectorPath + "|" +
                                std::to_string(request.threads) + "|" + (request.useGpu ? "gpu" : "cpu") + "|" +
                                std::to_string(contextTokens) + (compact ? "|q8" : "");
        if (!loaded_ || key_ != key) {
            unload();
            stage("Loading the local model");
            LoadWatch watch{&cancelled, &stage, "Loading the local model"};
            common_params params = engineParams(request.modelPath, request.drafterPath, request.threads, request.useGpu,
                                                contextTokens, 2048, &watch); // prompts read 2048 tokens a batch
            // Keys and values in 8 bits: the fit gives about twice the context (8-bit values need flash
            // attention, which llama.cpp turns on by itself where the GPU has it).
            if (compact) {
                params.cache_type_k = GGML_TYPE_Q8_0;
                params.cache_type_v = GGML_TYPE_Q8_0;
            }
            // A projector that reads a picture all at once (non-causal: Gemma 4 12B) needs the whole
            // picture in one batch: llama.cpp aborts the process otherwise.
            if (const int whole = wholePictureTokens(request.projectorPath); whole > params.n_ubatch) {
                params.n_ubatch = whole;
                params.n_batch = std::max(params.n_batch, whole);
            }
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
        if (hasMedia(request.messages) && (!m.media || m.mediaPath != request.projectorPath)) {
            if (request.projectorPath.empty()) {
                r.error = "the local model cannot see or hear yet: download its eyes and ears in Local Model";
                return r;
            }
            m.media.reset();
            stage("Loading the model's eyes and ears");
            mtmd_context_params mp = mtmd_context_params_default();
            mp.use_gpu = request.useGpu;
            mp.n_threads = request.threads;
            mp.print_timings = false;
            mp.warmup = false;
            m.media.reset(mtmd_init_from_file(request.projectorPath.c_str(), m.model(), mp));
            if (!m.media) {
                r.error = "the model's eyes and ears could not be loaded (file or memory)";
                return r;
            }
            m.mediaPath = request.projectorPath;
        }
        if (!m.templates)
            m.templates = common_chat_templates_init(m.model(), "");
        r.contextTokens = int(llama_n_ctx(m.ctx()));
        if (request.transcribe) {
            transcribeAudio(m, request, stage, text, cancelled, r);
            lastUse_ = std::chrono::steady_clock::now();
            return r;
        }
        const llama_vocab* vocab = llama_model_get_vocab(m.model());
        common_chat_session session(m.templates.get(), vocab, chatInputs(request));
        common_params_sampling sampling = m.params.sampling;
        if (request.temperature >= 0.0f)
            sampling.temp = request.temperature; // 0: greedy, the same answer to the same conversation
        session.apply_sampling(sampling); // the tool calls' grammar, when there are tools
        const Prompt prompt = chatPrompt(m, session.prompt(), mediaBitmaps(m, request.messages), r.mediaTokens);
        r.promptTokens = int(prompt.size());
        // maxTokens 0: as much as the context has room for.
        const int room = r.contextTokens - int(prompt.size()) - 1;
        const int maxTokens = request.maxTokens > 0 ? request.maxTokens : room;
        if (room < std::min(maxTokens, 256)) {
            r.error = "the conversation is too long for the local model (" + std::to_string(prompt.size()) + " of " +
                      std::to_string(r.contextTokens) + " tokens): start a new chat";
            return r;
        }
        stage(r.mediaTokens > 0 ? "Looking and listening (" + std::to_string(prompt.size()) + " tokens)"
                                : "Reading " + std::to_string(prompt.size()) + " tokens");
        bool started = false;
        size_t sent = 0; // of the answer's text, what went out already
        const Generated g = generate(m, prompt, std::min(maxTokens, room), sampling, cancelled, [&](const std::string& piece, llama_token token) {
            common_chat_input chunk;
            chunk.append(piece, token);
            const common_chat_msg& msg = readingOutput([&]() -> const common_chat_msg& { return session.feed(chunk); });
            // Only the answer's text streams; a tool call's markup is parsed, not shown.
            if (msg.content.size() > sent && msg.content.compare(0, sent, msg.content, 0, sent) == 0) {
                if (!started) {
                    started = true;
                    stage("Writing");
                }
                text(msg.content.substr(sent));
                sent = msg.content.size();
            }
        });
        lastUse_ = std::chrono::steady_clock::now();
        if (cancelled()) {
            r.error = "stopped";
            return r;
        }
        if (g.looped) {
            r.error = "the model went round in a loop, repeating itself (stopped after " + std::to_string(g.produced) + " tokens)";
            return r;
        }
        const common_chat_msg& answer = readingOutput([&]() -> const common_chat_msg& { return session.finish(); });
        r.text = withoutChannels(answer.content);
        for (size_t i = 0; i < answer.tool_calls.size(); ++i) {
            const common_chat_tool_call& call = answer.tool_calls[i];
            r.toolCalls.push_back({call.id.empty() ? "call_" + std::to_string(i + 1) : call.id, call.name, call.arguments});
        }
        r.generatedTokens = g.produced;
        r.firstTokenSeconds = g.firstTokenSeconds;
        r.tokensPerSecond = g.decodeSeconds > 0 && g.produced > 1 ? (g.produced - 1) / g.decodeSeconds : 0;
        r.ok = true;
    } catch (const ModelOutputError& e) {
        r.error = std::string("the model wrote a tool call that cannot be read: ") + e.what();
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
        Prompt code;
        code.tokens = common_tokenize(ctx, kGeneratePrompt, true, true);
        const Generated g = generate(*m, code, settings.generateTokens, m->params.sampling, cancelled,
                                     [&sample](const std::string& piece, llama_token) {
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
