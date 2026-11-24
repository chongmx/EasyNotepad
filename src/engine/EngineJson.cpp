#include "enp/engine/EngineJson.h"

#include "enp/model/Base64.h"

namespace enp::engine {

using model::Json;

namespace {

int integer(const Json& j, const char* key, int fallback = 0) { return int(j.getNumber(key, fallback)); }
uint64_t bytes(const Json& j, const char* key) { return uint64_t(j.getNumber(key, 0)); }

Json toJson(const interfaces::LocalToolCall& c)
{
    return Json::object().set("id", c.id).set("name", c.name).set("arguments", c.arguments);
}

interfaces::LocalToolCall toolCallFrom(const Json& j) { return {j.getString("id"), j.getString("name"), j.getString("arguments")}; }

Json toolCallsJson(const std::vector<interfaces::LocalToolCall>& calls)
{
    Json out = Json::array();
    for (const interfaces::LocalToolCall& c : calls)
        out.push(toJson(c));
    return out;
}

std::vector<interfaces::LocalToolCall> toolCallsFrom(const Json& j, const char* key)
{
    std::vector<interfaces::LocalToolCall> out;
    if (const Json* calls = j.find(key))
        for (const Json& c : calls->items())
            out.push_back(toolCallFrom(c));
    return out;
}

} // namespace

Json toJson(const interfaces::LocalChatRequest& r)
{
    Json messages = Json::array();
    for (const interfaces::LocalChatMessage& m : r.messages) {
        Json message = Json::object().set("role", m.role).set("text", m.text);
        if (!m.media.empty()) {
            Json media = Json::array();
            for (const interfaces::LocalMedia& x : m.media)
                media.push(Json::object()
                               .set("kind", x.kind == interfaces::LocalMedia::Kind::Audio ? "audio" : "image")
                               .set("name", x.name)
                               .set("width", x.width)
                               .set("height", x.height)
                               .set("data", model::base64Encode(x.data)));
            message.set("media", std::move(media));
        }
        if (!m.toolCalls.empty())
            message.set("toolCalls", toolCallsJson(m.toolCalls));
        if (!m.toolCallId.empty() || !m.toolName.empty())
            message.set("toolCallId", m.toolCallId).set("toolName", m.toolName);
        messages.push(std::move(message));
    }
    Json tools = Json::array();
    for (const interfaces::LocalTool& t : r.tools)
        tools.push(Json::object().set("name", t.name).set("description", t.description).set("parameters", t.parameters));
    return Json::object()
        .set("op", "chat")
        .set("messages", std::move(messages))
        .set("tools", std::move(tools))
        .set("modelPath", r.modelPath)
        .set("drafterPath", r.drafterPath)
        .set("projectorPath", r.projectorPath)
        .set("threads", r.threads)
        .set("useGpu", r.useGpu)
        .set("compactCache", r.compactCache)
        .set("thinking", r.thinking)
        .set("maxTokens", r.maxTokens)
        .set("contextTokens", r.contextTokens)
        .set("temperature", double(r.temperature))
        .set("transcribe", r.transcribe)
        .set("audioWindowSeconds", r.audioWindowSeconds);
}

interfaces::LocalChatRequest chatRequestFrom(const Json& j)
{
    interfaces::LocalChatRequest r;
    if (const Json* messages = j.find("messages"))
        for (const Json& m : messages->items()) {
            interfaces::LocalChatMessage message;
            message.role = m.getString("role");
            message.text = m.getString("text");
            if (const Json* media = m.find("media"))
                for (const Json& x : media->items()) {
                    interfaces::LocalMedia item;
                    item.kind = x.getString("kind") == "audio" ? interfaces::LocalMedia::Kind::Audio
                                                               : interfaces::LocalMedia::Kind::Image;
                    item.name = x.getString("name");
                    item.width = integer(x, "width");
                    item.height = integer(x, "height");
                    item.data = model::base64Decode(x.getString("data")).value_or(std::string());
                    message.media.push_back(std::move(item));
                }
            message.toolCalls = toolCallsFrom(m, "toolCalls");
            message.toolCallId = m.getString("toolCallId");
            message.toolName = m.getString("toolName");
            r.messages.push_back(std::move(message));
        }
    if (const Json* tools = j.find("tools"))
        for (const Json& t : tools->items())
            r.tools.push_back({t.getString("name"), t.getString("description"), t.getString("parameters")});
    r.modelPath = j.getString("modelPath");
    r.drafterPath = j.getString("drafterPath");
    r.projectorPath = j.getString("projectorPath");
    r.threads = integer(j, "threads", r.threads);
    r.useGpu = j.getBool("useGpu", r.useGpu);
    r.compactCache = j.getBool("compactCache");
    r.thinking = j.getBool("thinking");
    r.maxTokens = integer(j, "maxTokens", r.maxTokens);
    r.contextTokens = integer(j, "contextTokens", r.contextTokens);
    if (const Json* t = j.find("temperature"); t && t->isNumber())
        r.temperature = float(t->asNumber());
    r.transcribe = j.getBool("transcribe");
    r.audioWindowSeconds = integer(j, "audioWindowSeconds", r.audioWindowSeconds);
    return r;
}

Json toJson(const interfaces::LocalChatResult& r)
{
    return Json::object()
        .set("ok", r.ok)
        .set("error", r.error)
        .set("text", r.text)
        .set("toolCalls", toolCallsJson(r.toolCalls))
        .set("promptTokens", r.promptTokens)
        .set("mediaTokens", r.mediaTokens)
        .set("contextTokens", r.contextTokens)
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
    r.text = j.getString("text");
    r.toolCalls = toolCallsFrom(j, "toolCalls");
    r.promptTokens = integer(j, "promptTokens");
    r.mediaTokens = integer(j, "mediaTokens");
    r.contextTokens = integer(j, "contextTokens");
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
