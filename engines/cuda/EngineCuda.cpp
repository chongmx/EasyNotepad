// enp-engine-cuda: the local model on llama.cpp built with its CUDA backend, as an engine library
// (include/enp/engine/EnpEngine.h). Put next to the app, it is loaded by the helper process
// (`EasyNotepad --local-engine`); absent or unusable, the app answers on its own CPU engine.
#include "enp/engine/EnpEngine.h"
#include "enp/engine/EngineJson.h"
#include "enp/engine/LlamaRunner.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>

#if defined(_WIN32)
#include <stdlib.h>
#endif

#include "ggml.h"

namespace {

using enp::model::Json;

// A fault inside llama.cpp or CUDA ends the helper process at once, with its message on stderr
// (the app shows it), instead of a fault report that keeps the process waiting. This library has
// its own C runtime (MSVC, static), so it is set here, not only in the app.
void endOnFault()
{
    static const bool once = []() {
#if defined(_MSC_VER) || defined(_UCRT)
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
        std::signal(SIGABRT, [](int) { std::_Exit(3); });
        ggml_set_abort_callback([](const char* message) {
            std::fprintf(stderr, "%s\n", message);
            std::fflush(stderr);
            std::_Exit(3);
        });
        return true;
    }();
    (void)once;
}

std::string& describeText()
{
    static std::string text;
    return text;
}

// The first GPU, as results name it ("CUDA0: NVIDIA GeForce RTX 5060 Laptop GPU").
std::string gpuName()
{
    for (const enp::interfaces::EngineDevice& d : enp::engine::devices())
        if (d.kind != "CPU")
            return d.name + ": " + d.description;
    return {};
}

} // namespace

struct enp_engine {
    enp::engine::Runner runner;
    std::string result; // the last request's answer, valid until the next call
};

extern "C" {

ENP_ENGINE_EXPORT int enp_engine_abi(void) { return ENP_ENGINE_ABI; }

ENP_ENGINE_EXPORT const char* enp_engine_describe(void)
{
    endOnFault();
    try {
        enp::engine::initBackend();
        Json devices = Json::array();
        bool gpu = false;
        for (const enp::interfaces::EngineDevice& d : enp::engine::devices()) {
            gpu = gpu || d.kind != "CPU";
            devices.push(enp::engine::toJson(d));
        }
        Json j = Json::object().set("name", "CUDA").set("version", enp::engine::version()).set("devices", std::move(devices));
        if (!gpu)
            j.set("error", "no CUDA device: no NVIDIA GPU, or its driver is missing or too old");
        describeText() = j.dump();
    } catch (const std::exception& e) {
        describeText() = Json::object().set("name", "CUDA").set("error", std::string("CUDA failed to start: ") + e.what()).dump();
    }
    return describeText().c_str();
}

ENP_ENGINE_EXPORT enp_engine* enp_engine_create(void)
{
    try {
        return std::make_unique<enp_engine>().release(); // owned by the caller until enp_engine_destroy
    } catch (...) {
        return nullptr;
    }
}

ENP_ENGINE_EXPORT const char* enp_engine_request(enp_engine* engine, const char* request_json, enp_engine_event_fn event,
                                                 enp_engine_cancelled_fn cancelled, void* user)
{
    if (!engine)
        return "{\"ok\":false,\"error\":\"no engine\"}";
    auto report = [event, user](const char* kind) {
        return [event, user, kind](const std::string& text) {
            if (event)
                event(user, kind, text.c_str());
        };
    };
    const enp::engine::Cancelled stop = [cancelled, user]() { return cancelled && cancelled(user) != 0; };
    try {
        std::string error;
        const std::optional<Json> request = Json::parse(request_json ? request_json : "", &error);
        if (!request || !request->isObject()) {
            engine->result = Json::object().set("ok", false).set("error", "bad request: " + error).dump();
        } else if (request->getString("op") == "chat") {
            enp::interfaces::LocalChatRequest chat = enp::engine::chatRequestFrom(*request);
            chat.useGpu = true; // every layer on the GPU
            enp::interfaces::LocalChatResult r = engine->runner.chat(chat, report("stage"), report("text"), stop);
            r.backend = gpuName();
            engine->result = enp::engine::toJson(r).dump();
        } else if (request->getString("op") == "run") {
            const enp::interfaces::EngineRunResult r =
                engine->runner.run(enp::engine::runSettingsFrom(*request), report("stage"), stop);
            engine->result = enp::engine::toJson(r).dump();
        } else {
            engine->result = Json::object().set("ok", false).set("error", "unknown request").dump();
        }
    } catch (const std::exception& e) {
        engine->result = Json::object().set("ok", false).set("error", std::string("engine error: ") + e.what()).dump();
    }
    return engine->result.c_str();
}

ENP_ENGINE_EXPORT void enp_engine_unload(enp_engine* engine)
{
    if (engine)
        engine->runner.unload();
}

ENP_ENGINE_EXPORT void enp_engine_destroy(enp_engine* engine)
{
    std::unique_ptr<enp_engine> owned(engine);
}

} // extern "C"
