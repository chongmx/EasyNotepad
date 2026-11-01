// Contract for the on-device inference engine (plan 23): what the device offers, downloading a
// model, and measuring how fast the model runs here. The developer "Engine lab" page uses it.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
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
    virtual void cancel() = 0; // stops the run in progress at the next token
    virtual bool running() const = 0;

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
    // SHA-256 of a file, computed on a worker thread; `done` gets it (empty if unreadable) on the
    // main thread, and `progress` the fraction read so far.
    virtual void hashFile(const std::string& path, std::function<void(double fraction)> progress,
                          std::function<void(const std::string& sha256)> done) = 0;
};

} // namespace enp::interfaces
