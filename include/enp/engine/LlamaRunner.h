// The local model on llama.cpp: loading, chatting and the Engine Lab's measurements. The same code
// runs in the app (the CPU engine, built in) and in an engine library (enp-engine-cuda, built with
// the CUDA backend), so both answer alike. Nothing here uses the app's OS layer.
#pragma once

#include "enp/interfaces/ILocalEngineService.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace enp::engine {

// Called on the thread that runs the request.
using Report = std::function<void(const std::string&)>;
using Cancelled = std::function<bool()>;

// Starts llama.cpp once per process (its log silenced, its backends registered).
void initBackend();
// "llama.cpp b12030 (a91eab6)".
std::string version();
// The devices llama.cpp's backends can use (initBackend first).
std::vector<interfaces::EngineDevice> devices();

class Runner {
public:
    Runner();
    ~Runner();
    Runner(const Runner&) = delete;
    Runner& operator=(const Runner&) = delete;

    // Answers the conversation; the model stays loaded for the next one with the same settings.
    interfaces::LocalChatResult chat(const interfaces::LocalChatRequest& request, const Report& stage, const Report& text,
                                     const Cancelled& cancelled);
    // Loads the model, measures prompt and generation speed, unloads (the chat model first).
    interfaces::EngineRunResult run(const interfaces::EngineRunSettings& settings, const Report& stage, const Cancelled& cancelled);

    void unload();
    bool loaded() const;
    std::chrono::steady_clock::time_point lastUse() const { return lastUse_; }

private:
    struct Loaded;
    std::unique_ptr<Loaded> loaded_;
    std::string key_; // model, drafter, threads, GPU, context: a change means loading again
    std::chrono::steady_clock::time_point lastUse_;
};

} // namespace enp::engine
