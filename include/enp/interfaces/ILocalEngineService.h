// Contract for the on-device inference engine (plan 23): what the device offers, downloading a
// model, and measuring how fast the model runs here. The developer "Engine lab" page uses it.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace enp::interfaces {

// A compute device the engine can use: the CPU, or a GPU found through Vulkan.
struct EngineDevice {
    std::string name;
    std::string description;
    std::string kind; // "CPU", "GPU", "iGPU", "accelerator"
    uint64_t memoryTotal = 0;
    uint64_t memoryFree = 0;
};

struct EngineInfo {
    bool available = false;        // this build has an engine and this CPU can run it
    std::string unavailableReason; // why not, in plain words
    std::string engineVersion;     // e.g. "llama.cpp b12030"
    std::string cpuName;
    std::string cpuFeatures;       // what the CPU supports that matters for speed
    std::string socName;           // phones: the chip, e.g. "Qualcomm SM8650"
    int cpuThreads = 0;
    uint64_t memoryTotal = 0;
    uint64_t memoryAvailable = 0;
    std::vector<EngineDevice> devices;
    // A faster engine library next to the app (enp-engine-cuda), in words: "CUDA: NVIDIA GeForce RTX
    // 5060 (8.0 GB)", "CUDA: not installed", "CUDA: starting", "CUDA: stopped (why); using the CPU".
    // Empty where the platform has none.
    std::string accelerator;
    bool acceleratorReady = false; // chats and GPU runs go to it
};

struct EngineRunSettings {
    std::string modelPath;
    std::string drafterPath; // speculative decoding with this drafter; empty: none
    bool useGpu = false;     // offload every layer to the first GPU
    int threads = 4;
    int promptTokens = 512;   // prompt processing is measured over this many tokens
    int generateTokens = 128; // then this many are generated
    int contextTokens = 4096;
};

struct EngineRunResult {
    bool ok = false;
    std::string error;
    std::string backend; // what actually ran, e.g. "CPU (6 threads)" or "Vulkan0: Adreno 750"
    bool drafter = false;
    double loadSeconds = 0;
    double promptTokensPerSecond = 0;
    double generateTokensPerSecond = 0;
    double firstTokenSeconds = 0; // prompt processed plus the first token generated
    int promptTokens = 0;
    int generatedTokens = 0;
    int drafted = 0;  // speculative decoding: tokens proposed by the drafter
    int accepted = 0; // and accepted by the model
    uint64_t peakMemory = 0; // the process's peak resident memory, when the platform reports it
    std::string sample;      // the start of the generated text, to see it is sensible
};

// A conversation for the local agent: the system prompt first, then the turns, oldest first.
// A picture or a sound that goes with a message. The model sees and hears them when its projector
// (LocalChatRequest::projectorPath) is given; without it they are left out.
struct LocalMedia {
    enum class Kind { Image, Audio };
    Kind kind = Kind::Image;
    std::string name;          // what the conversation calls it: "photo.png", "page 3 of report.pdf"
    int width = 0, height = 0; // Image: its size in pixels
    std::string data;          // Image: RGB, three bytes a pixel, row by row; Audio: the file (WAV, MP3 or FLAC)
};

// A tool the model may call (it answers with calls instead of text when it wants one), and a call.
struct LocalTool {
    std::string name;
    std::string description;
    std::string parameters; // JSON Schema of its arguments, as text
};

struct LocalToolCall {
    std::string id;
    std::string name;
    std::string arguments; // a JSON object, as text
};

struct LocalChatMessage {
    LocalChatMessage() = default;
    LocalChatMessage(std::string role, std::string text) : role(std::move(role)), text(std::move(text)) {}
    std::string role; // "system", "user", "assistant" or "tool"
    std::string text;
    std::vector<LocalMedia> media;        // shown to the model before the text
    std::vector<LocalToolCall> toolCalls; // assistant: the calls it made
    std::string toolCallId;               // tool: the call this answers
    std::string toolName;
};

struct LocalChatRequest {
    std::vector<LocalChatMessage> messages;
    std::string modelPath;
    std::string drafterPath;   // empty: no speculative decoding
    std::string projectorPath; // the model's eyes and ears (mmproj); loaded when a message has media
    std::vector<LocalTool> tools;
    int threads = 4;
    bool useGpu = false;
    int maxTokens = 768;      // 0: as many as the context has room for
    int contextTokens = 8192; // 0: the largest the model takes that fits the memory free
    // How freely it picks its words: 0 the likeliest every time (the same answer to the same
    // conversation), more for variety; below 0 the engine's own (0.3).
    float temperature = -1.0f;
    // Write down the speech of the recordings in the last message (its text, if any, is the
    // instruction), in windows of this many seconds; the result's text is the transcript.
    bool transcribe = false;
    int audioWindowSeconds = 30;
    // On the CPU, in this process, beside whatever the GPU holds: a helper model (a speech model as
    // the ears of one without), so the main model stays loaded on the GPU.
    bool cpuOnly = false;
    // The cache of what it has read (keys and values) in 8 bits on the GPU: about twice the context in
    // the same memory (Qwen 3.5 9B on 8 GB). Part of what is loaded: the same for every request to a model.
    bool compactCache = false;
    // The model reasons before it answers (its template's thinking): what it thinks is not in the
    // answer. Slower; for a plan, where a small model's first idea missed what the question asked.
    bool thinking = false;
};

struct LocalChatResult {
    bool ok = false;
    std::string error;
    std::string text;                     // the answer, without the markup of any tool calls
    std::vector<LocalToolCall> toolCalls; // the tools it wants run before it goes on
    int promptTokens = 0;
    int mediaTokens = 0; // of promptTokens, those the pictures and sounds took
    int contextTokens = 0; // the context the model has (with LocalChatRequest::contextTokens 0, what fitted)
    int generatedTokens = 0;
    double firstTokenSeconds = 0;
    double tokensPerSecond = 0;
    bool loaded = false; // the model was loaded for this answer (not already in memory)
    std::string backend; // what answered: "CPU", or the engine library's device ("CUDA0: ...")
};

// Where a download stands.
struct DownloadStatus {
    enum class State { Idle, Running, Done, Failed };
    State state = State::Idle;
    uint64_t received = 0;
    uint64_t total = 0; // 0 while unknown
    std::string error;
};

class ILocalEngineService {
public:
    virtual ~ILocalEngineService() = default;

    // Cheap after the first call.
    virtual EngineInfo info() = 0;

    // Loads the model, measures, unloads. Runs on a worker thread; `progress` and `done` are
    // called on the main thread. One run at a time.
    virtual void run(const EngineRunSettings& settings, std::function<void(const std::string& stage)> progress,
                     std::function<void(const EngineRunResult& result)> done) = 0;
    virtual void cancel() = 0; // stops the run or chat in progress at the next token
    virtual bool running() const = 0;

    // The local agent: answers the conversation, streaming the text as it is written. The model
    // stays loaded for the next question (see unloadIfIdle). `stage` ("Loading the local model")
    // and `text` come on the main thread, then `done`. One at a time; not while a lab run runs.
    virtual void chat(const LocalChatRequest& request, std::function<void(const std::string& stage)> stage,
                      std::function<void(const std::string& text)> text, std::function<void(const LocalChatResult& result)> done)
    {
        (void)request;
        (void)stage;
        (void)text;
        LocalChatResult r;
        r.error = "this build has no local model";
        done(r);
    }
    virtual bool chatting() const { return false; }
    // Frees the chat model when it has not been used for `idleSeconds` (its memory back to the system).
    virtual void unloadIfIdle(double idleSeconds) { (void)idleSeconds; }

    // Where downloaded models are kept on this device.
    virtual std::string modelsDirectory() = 0;
    // Free space, in bytes, on the drive that holds `path` (0 if unknown).
    virtual uint64_t freeSpace(const std::string& path) = 0;
    // Size of a file in bytes; 0 if it does not exist.
    virtual uint64_t fileSize(const std::string& path) = 0;
    // Fetches `url` into `path`, resuming a partial file. One download at a time. Phones wait for
    // Wi-Fi unless `allowMetered`.
    virtual bool startDownload(const std::string& url, const std::string& path, bool allowMetered, std::string* error) = 0;
    virtual DownloadStatus downloadStatus() = 0;
    virtual void cancelDownload() = 0;
    // On mobile data (or another metered network) right now.
    virtual bool meteredNetwork() = 0;
    // Phones: copy a file the user picks (e.g. one downloaded in the browser) to `path`, followed
    // through downloadStatus() like a download.
    virtual bool canImport() { return false; }
    virtual bool importFile(const std::string& path, std::string* error) { (void)path; (void)error; return false; }
    // SHA-256 of a file, computed on a worker thread; `done` gets it (empty if unreadable) on the
    // main thread, and `progress` the fraction read so far.
    virtual void hashFile(const std::string& path, std::function<void(double fraction)> progress,
                          std::function<void(const std::string& sha256)> done) = 0;
};

} // namespace enp::interfaces
