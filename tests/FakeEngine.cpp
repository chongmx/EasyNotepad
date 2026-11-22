// An engine library (include/enp/engine/EnpEngine.h) that behaves as the tests ask, to check that
// the app falls back to its CPU engine whatever an engine library does. No llama.cpp, no GPU.
//
// At start, ENP_FAKE_ENGINE_START: "ok" (default), "nodevice", "abi" (another interface), "crash".
// Per chat, the model path: "fake:answer", "fake:fail", "fake:crash", "fake:crash-after-text",
// "fake:hang", "fake:slow" (writes until cancelled), "fake:bad-tool-call" and "fake:nested" (the
// model's fault), "fake:odd-error" (an error not about loading).
#include "enp/engine/EnpEngine.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace {

std::string start()
{
    const char* s = std::getenv("ENP_FAKE_ENGINE_START");
    return s ? s : "ok";
}

} // namespace

struct enp_engine {
    std::string result;
};

extern "C" {

ENP_ENGINE_EXPORT int enp_engine_abi(void) { return start() == "abi" ? ENP_ENGINE_ABI + 41 : ENP_ENGINE_ABI; }

ENP_ENGINE_EXPORT const char* enp_engine_describe(void)
{
    const std::string mode = start();
    if (mode == "crash")
        std::abort();
    if (mode == "nodevice")
        return "{\"name\":\"Fake\",\"devices\":[],\"error\":\"no fake device\"}";
    return "{\"name\":\"Fake\",\"version\":\"1\",\"devices\":[{\"name\":\"FAKE0\",\"description\":\"Fake GPU\",\"kind\":\"GPU\","
           "\"memoryTotal\":8589934592,\"memoryFree\":8000000000}]}";
}

ENP_ENGINE_EXPORT enp_engine* enp_engine_create(void)
{
    static enp_engine engine;
    return &engine;
}

ENP_ENGINE_EXPORT const char* enp_engine_request(enp_engine* engine, const char* request, enp_engine_event_fn event,
                                                 enp_engine_cancelled_fn cancelled, void* user)
{
    const std::string r = request ? request : "";
    auto has = [&r](const char* what) { return r.find(what) != std::string::npos; };
    auto say = [&](const char* kind, const char* text) { event(user, kind, text); };
    if (has("\"fake:bad-tool-call\"")) {
        engine->result = "{\"ok\":false,\"error\":\"engine error: Failed to parse tool call arguments as JSON\"}";
    } else if (has("\"fake:nested\"")) { // from an older library: the model's brackets in a loop
        engine->result = "{\"ok\":false,\"error\":\"engine error: input is nested more than 64 levels deep\"}";
    } else if (has("\"fake:odd-error\"")) {
        engine->result = "{\"ok\":false,\"error\":\"engine error: something odd\"}";
    } else if (has("\"fake:fail\"")) {
        engine->result = "{\"ok\":false,\"error\":\"out of fake memory\"}";
    } else if (has("\"fake:crash\"")) {
        std::abort();
    } else if (has("\"fake:crash-after-text\"")) {
        say("text", "Half");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::abort();
    } else if (has("\"fake:hang\"")) {
        for (;;)
            std::this_thread::sleep_for(std::chrono::seconds(1));
    } else if (has("\"fake:slow\"")) {
        while (!cancelled(user)) {
            say("text", ".");
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        engine->result = "{\"ok\":false,\"error\":\"stopped\"}";
    } else if (has("\"op\":\"run\"")) {
        say("stage", "Generating");
        engine->result = "{\"ok\":true,\"backend\":\"FAKE0: Fake GPU\",\"generateTokensPerSecond\":123,\"generatedTokens\":16}";
    } else {
        say("stage", "Loading the local model");
        say("text", "Hello");
        say("text", " from the GPU");
        engine->result = "{\"ok\":true,\"generatedTokens\":2,\"backend\":\"FAKE0: Fake GPU\"}";
    }
    return engine->result.c_str();
}

ENP_ENGINE_EXPORT void enp_engine_unload(enp_engine*) {}

ENP_ENGINE_EXPORT void enp_engine_destroy(enp_engine*) {}

} // extern "C"
