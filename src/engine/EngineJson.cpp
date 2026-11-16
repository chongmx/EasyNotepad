#include "enp/engine/EngineJson.h"

namespace enp::engine {

using model::Json;

namespace {

int integer(const Json& j, const char* key, int fallback = 0) { return int(j.getNumber(key, fallback)); }
uint64_t bytes(const Json& j, const char* key) { return uint64_t(j.getNumber(key, 0)); }

} // namespace

Json toJson(const interfaces::LocalChatRequest& r)
{
    Json messages = Json::array();
    for (const interfaces::LocalChatMessage& m : r.messages)
        messages.push(Json::object().set("role", m.role).set("text", m.text));
    return Json::object()
        .set("op", "chat")
        .set("messages", std::move(messages))
        .set("modelPath", r.modelPath)
        .set("drafterPath", r.drafterPath)
        .set("threads", r.threads)
        .set("useGpu", r.useGpu)
        .set("maxTokens", r.maxTokens)
        .set("contextTokens", r.contextTokens);
}

interfaces::LocalChatRequest chatRequestFrom(const Json& j)
{
    interfaces::LocalChatRequest r;
    if (const Json* messages = j.find("messages"))
        for (const Json& m : messages->items())
            r.messages.push_back({m.getString("role"), m.getString("text")});
    r.modelPath = j.getString("modelPath");
    r.drafterPath = j.getString("drafterPath");
    r.threads = integer(j, "threads", r.threads);
    r.useGpu = j.getBool("useGpu", r.useGpu);
    r.maxTokens = integer(j, "maxTokens", r.maxTokens);
    r.contextTokens = integer(j, "contextTokens", r.contextTokens);
    return r;
}

Json toJson(const interfaces::LocalChatResult& r)
{
    return Json::object()
        .set("ok", r.ok)
        .set("error", r.error)
        .set("promptTokens", r.promptTokens)
        .set("generatedTokens", r.generatedTokens)
        .set("firstTokenSeconds", r.firstTokenSeconds)
        .set("tokensPerSecond", r.tokensPerSecond)
        .set("loaded", r.loaded)
        .set("backend", r.backend);
}

interfaces::LocalChatResult chatResultFrom(const Json& j)
{
    interfaces::LocalChatResult r;
    r.ok = j.getBool("ok");
    r.error = j.getString("error");
    r.promptTokens = integer(j, "promptTokens");
    r.generatedTokens = integer(j, "generatedTokens");
    r.firstTokenSeconds = j.getNumber("firstTokenSeconds");
    r.tokensPerSecond = j.getNumber("tokensPerSecond");
    r.loaded = j.getBool("loaded");
    r.backend = j.getString("backend");
    return r;
}

Json toJson(const interfaces::EngineRunSettings& s)
{
    return Json::object()
        .set("op", "run")
        .set("modelPath", s.modelPath)
        .set("drafterPath", s.drafterPath)
        .set("useGpu", s.useGpu)
        .set("threads", s.threads)
        .set("promptTokens", s.promptTokens)
        .set("generateTokens", s.generateTokens)
        .set("contextTokens", s.contextTokens);
}

interfaces::EngineRunSettings runSettingsFrom(const Json& j)
{
    interfaces::EngineRunSettings s;
    s.modelPath = j.getString("modelPath");
    s.drafterPath = j.getString("drafterPath");
    s.useGpu = j.getBool("useGpu", s.useGpu);
    s.threads = integer(j, "threads", s.threads);
    s.promptTokens = integer(j, "promptTokens", s.promptTokens);
    s.generateTokens = integer(j, "generateTokens", s.generateTokens);
    s.contextTokens = integer(j, "contextTokens", s.contextTokens);
    return s;
}

Json toJson(const interfaces::EngineRunResult& r)
{
    return Json::object()
        .set("ok", r.ok)
        .set("error", r.error)
        .set("backend", r.backend)
        .set("drafter", r.drafter)
        .set("loadSeconds", r.loadSeconds)
        .set("promptTokensPerSecond", r.promptTokensPerSecond)
        .set("generateTokensPerSecond", r.generateTokensPerSecond)
        .set("firstTokenSeconds", r.firstTokenSeconds)
        .set("promptTokens", r.promptTokens)
        .set("generatedTokens", r.generatedTokens)
        .set("drafted", r.drafted)
        .set("accepted", r.accepted)
        .set("peakMemory", double(r.peakMemory))
        .set("sample", r.sample);
}

interfaces::EngineRunResult runResultFrom(const Json& j)
{
    interfaces::EngineRunResult r;
    r.ok = j.getBool("ok");
    r.error = j.getString("error");
    r.backend = j.getString("backend");
    r.drafter = j.getBool("drafter");
    r.loadSeconds = j.getNumber("loadSeconds");
    r.promptTokensPerSecond = j.getNumber("promptTokensPerSecond");
    r.generateTokensPerSecond = j.getNumber("generateTokensPerSecond");
    r.firstTokenSeconds = j.getNumber("firstTokenSeconds");
    r.promptTokens = integer(j, "promptTokens");
    r.generatedTokens = integer(j, "generatedTokens");
    r.drafted = integer(j, "drafted");
    r.accepted = integer(j, "accepted");
    r.peakMemory = bytes(j, "peakMemory");
    r.sample = j.getString("sample");
    return r;
}

Json toJson(const interfaces::EngineDevice& d)
{
    return Json::object()
        .set("name", d.name)
        .set("description", d.description)
        .set("kind", d.kind)
        .set("memoryTotal", double(d.memoryTotal))
        .set("memoryFree", double(d.memoryFree));
}

interfaces::EngineDevice deviceFrom(const Json& j)
{
    interfaces::EngineDevice d;
    d.name = j.getString("name");
    d.description = j.getString("description");
    d.kind = j.getString("kind");
    d.memoryTotal = bytes(j, "memoryTotal");
    d.memoryFree = bytes(j, "memoryFree");
    return d;
}

} // namespace enp::engine
