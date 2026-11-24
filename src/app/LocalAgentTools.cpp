#include "LocalAgentTools.h"

#include "AutomationController.h"

#include "enp/model/BinaryInfo.h"
#include "enp/model/Csv.h"
#include "enp/model/viewer/Viewer.h"
#include "enp/os/Dispatch.h"
#include "enp/os/Fs.h"
#include "enp/os/Memory.h"
#include "enp/services/ImageDecoder.h"
#include "enp/services/ImageEncoder.h"
#include "enp/view/MainView.h"
#include "enp/view/UiAutomation.h"
#include "enp/viewmodel/FileDialogViewModel.h"
#include "enp/viewmodel/NotepadViewModel.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstring>
#include <ctime>
#include <map>
#include <cstdio>

namespace enp::app {

using interfaces::LocalMedia;
using interfaces::LocalTool;
using model::Json;
using Output = viewmodel::AgentViewModel::ToolOutput;

namespace {

constexpr int kReadLines = 200;          // of a text file, when no range is asked for
constexpr int kListLimit = 300;          // entries of a folder
constexpr int kDocumentPages = 3;        // pages of a document, when no range is asked for
constexpr size_t kPartChars = 10000;     // of a document without pages (Word reflowed, Excel)
constexpr size_t kDocumentChars = 11000; // of pages in one reply (the agent cuts a tool result at 12000)
constexpr int kMinPictureTokens = 70, kMaxPictureTokens = 1120; // ... and 70 to 1120 tokens on one
constexpr size_t kSoundBytes = 25u << 20; // a recording read whole (about 2 minutes of WAV)
constexpr int kLookMinFrames = 4;        // a page shown is drawn at least this often before it is captured
constexpr int kLookMaxFrames = 240;      // ... and captured after this many even while pictures still load

// A JSON Schema for a tool's arguments: each is a string or integer, with what it is for.
std::string schema(std::vector<std::tuple<const char*, const char*, const char*>> args, std::vector<const char*> required = {})
{
    Json properties = Json::object();
    for (const auto& [name, type, what] : args)
        properties.set(name, Json::object().set("type", type).set("description", what));
    Json req = Json::array();
    for (const char* r : required)
        req.push(Json(std::string(r)));
    return Json::object().set("type", "object").set("properties", std::move(properties)).set("required", std::move(req)).dump();
}

// An argument the model gave as a number or as text ("3", 3, 3.0).
int argInt(const Json& args, const char* key, int fallback)
{
    const Json* v = args.find(key);
    if (!v)
        return fallback;
    if (v->isNumber())
        return int(std::lround(v->asNumber()));
    if (v->isString()) {
        try {
            return std::stoi(v->asString());
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::string trimmed(std::string s); // (below, with the tables)
bool isDocument(const std::string& path); // (below, with the many-documents tools)
std::vector<std::string> splitList(const std::string& text); // (below, with query_table)
std::vector<std::string> documentsUnder(const std::string& root, const std::string& dir, size_t limit);

std::string extension(const std::string& path)
{
    const size_t dot = path.find_last_of('.');
    const size_t slash = path.find_last_of("/\\");
    return dot == std::string::npos || (slash != std::string::npos && dot < slash) ? std::string() : lower(path.substr(dot + 1));
}

std::string fileName(const std::string& path)
{
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Text from the model as the file has it: "12| " prefixes copied from read_file dropped (when every
// line has one), and the file's line ends (`crlf`).
std::string asFileText(const std::string& text, bool crlf)
{
    std::vector<std::string> lines;
    for (size_t from = 0;;) {
        const size_t nl = text.find('\n', from);
        std::string line = text.substr(from, nl == std::string::npos ? std::string::npos : nl - from);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(std::move(line));
        if (nl == std::string::npos)
            break;
        from = nl + 1;
    }
    auto prefix = [](const std::string& line) -> size_t { // length of "12| ", or 0
        size_t i = 0;
        while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i])))
            ++i;
        if (i == 0 || i >= line.size() || line[i] != '|')
            return 0;
        return i + 1 < line.size() && line[i + 1] == ' ' ? i + 2 : i + 1;
    };
    bool numbered = true, any = false;
    for (const std::string& line : lines)
        if (!line.empty()) {
            any = true;
            numbered = numbered && prefix(line) > 0;
        }
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        out += numbered && any && !lines[i].empty() ? lines[i].substr(prefix(lines[i])) : lines[i];
        if (i + 1 < lines.size())
            out += crlf ? "\r\n" : "\n";
    }
    return out;
}

std::string trimEnd(std::string s)
{
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.pop_back();
    return s;
}

// Where `oldText`'s lines are in `text`, comparing lines without their trailing spaces (a small model
// drops them): the byte range of the whole lines, when found exactly once.
bool findLines(const std::string& text, const std::string& oldText, size_t& begin, size_t& end)
{
    std::vector<std::string> want;
    for (size_t from = 0;;) {
        const size_t nl = oldText.find('\n', from);
        want.push_back(trimEnd(oldText.substr(from, nl == std::string::npos ? std::string::npos : nl - from)));
        if (nl == std::string::npos)
            break;
        from = nl + 1;
    }
    while (!want.empty() && want.back().empty())
        want.pop_back();
    if (want.empty())
        return false;
    std::vector<size_t> starts; // of each line of `text`
    for (size_t from = 0;;) {
        starts.push_back(from);
        const size_t nl = text.find('\n', from);
        if (nl == std::string::npos)
            break;
        from = nl + 1;
    }
    auto line = [&](size_t i) {
        const size_t to = i + 1 < starts.size() ? starts[i + 1] - 1 : text.size();
        return trimEnd(text.substr(starts[i], to - starts[i]));
    };
    int found = 0;
    for (size_t i = 0; i + want.size() <= starts.size(); ++i) {
        bool same = true;
        for (size_t k = 0; k < want.size() && same; ++k)
            same = line(i + k) == want[k];
        if (same) {
            if (++found > 1)
                return false;
            begin = starts[i];
            const size_t last = i + want.size() - 1;
            end = last + 1 < starts.size() ? starts[last + 1] - 1 : text.size(); // up to its line end
            if (end > begin && end <= text.size() && end > 0 && text[end - 1] == '\r')
                --end;
        }
    }
    return found == 1;
}

// What a file is by its name, for choosing the tool that reads it.
enum class FileKind { Text, Document, Picture, Sound };

FileKind kindOf(const std::string& path)
{
    static const char* const documents[] = {"pdf", "docx", "xlsx", "pptx", "docm", "xlsm", "pptm"};
    static const char* const pictures[] = {"png", "jpg", "jpeg", "gif", "bmp", "webp", "tif", "tiff", "tga", "qoi",
                                           "jp2", "j2k", "ico", "ppm", "pgm", "pnm", "hdr", "psd"};
    static const char* const sounds[] = {"wav", "mp3", "flac"};
    const std::string ext = extension(path);
    for (const char* e : documents)
        if (ext == e)
            return FileKind::Document;
    for (const char* e : pictures)
        if (ext == e)
            return FileKind::Picture;
    for (const char* e : sounds)
        if (ext == e)
            return FileKind::Sound;
    return FileKind::Text;
}

// The call to make for a file, spelled out: a small model follows an example better than a rule.
std::string whichTool(const std::string& path, bool hears)
{
    const std::string p = Json(path).dump(); // quoted and escaped
    switch (kindOf(path)) {
    case FileKind::Document:
        if (const std::string ext = extension(path); ext == "xlsx" || ext == "xlsm")
            return "It is a table: call query_table {\"path\": " + p +
                   "} to count, filter, group and total its rows (its reply names the columns), or read_document {\"path\": " + p +
                   "} for its text.";
        return "It is a document: call read_document {\"path\": " + p + "} for its text, or look_at_page {\"path\": " + p +
               ", \"page\": 1} to see a page.";
    case FileKind::Picture: return "It is a picture: call look_at_image {\"path\": " + p + "}.";
    case FileKind::Sound:
        return hears ? "It is a recording: call listen_to {\"path\": " + p + "}."
                     : "It is a recording, and this local model cannot hear recordings: tell the user so (Gemma can).";
    case FileKind::Text: break;
    }
    return "Call read_file {\"path\": " + p + "} for its text.";
}

// The skills (resources/skills/index.json): name and what each is for, in the index's order.
std::vector<std::pair<std::string, std::string>> skillIndex()
{
    std::vector<std::pair<std::string, std::string>> out;
    if (const std::optional<std::string> index = os::fs::readAsset("skills/index.json"))
        if (const std::optional<Json> j = Json::parse(*index); j && j->isArray())
            for (const Json& s : j->items())
                out.emplace_back(s.getString("name"), s.getString("description"));
    return out;
}

std::string skillDescription(const std::string& name)
{
    for (const auto& [n, description] : skillIndex())
        if (n == name)
            return description;
    return {};
}

bool looksBinary(const std::string& bytes)
{
    const size_t n = std::min<size_t>(bytes.size(), 8192);
    for (size_t i = 0; i < n; ++i)
        if (bytes[i] == '\0')
            return true;
    return false;
}

} // namespace

// The tokens a look spends on its picture: by how carefully it looks (low, normal, high, or a
// number), never fewer than the user's minimum (the agent set-up), within what Gemma 4 takes.
bool LocalAgentTools::nativeEars() const { return vm_.engineLab().chosenModel().hears; }

bool LocalAgentTools::hears() const { return nativeEars() || vm_.engineLab().earsUsable(); }

int LocalAgentTools::tokenPixels() const { return vm_.engineLab().chosenModel().tokenPixels; }

int LocalAgentTools::pictureTokens(const std::string& detail) const
{
    const std::string d = lower(trimmed(detail));
    int tokens = 280;
    if (d == "low")
        tokens = kMinPictureTokens;
    else if (d == "high" || d == "ocr" || d == "max" || d == "careful")
        tokens = kMaxPictureTokens;
    else if (!d.empty() && std::isdigit(static_cast<unsigned char>(d[0])))
        tokens = std::atoi(d.c_str());
    return std::clamp(std::max(tokens, vm_.agent().localPictureTokens.get()), kMinPictureTokens, kMaxPictureTokens);
}

namespace {

// RGBA (as decoded or captured) to the RGB picture the model is given, resized so it costs about
// `tokens` (one per `tokenPixels`: 48 x 48 for Gemma 4, 32 x 32 for Qwen 3.5): shrunk by averaging,
// or enlarged smoothly (a small part zoomed into keeps its detail for the model instead of being a
// few tokens).
LocalMedia picture(const std::string& name, const unsigned char* rgba, int width, int height, int stride, int tokens,
                   int tokenPixels)
{
    const double k = std::sqrt(double(tokens) * tokenPixels / (double(width) * double(height)));
    LocalMedia m;
    m.kind = LocalMedia::Kind::Image;
    m.name = name;
    m.width = std::clamp(int(std::lround(width * k)), 1, 8192);
    m.height = std::clamp(int(std::lround(height * k)), 1, 8192);
    m.data.resize(size_t(m.width) * size_t(m.height) * 3);
    const double sx = double(width) / m.width, sy = double(height) / m.height;
    auto at = [&](int x, int y) { return rgba + size_t(y) * size_t(stride) + size_t(x) * 4; };
    for (int y = 0; y < m.height; ++y)
        for (int x = 0; x < m.width; ++x) {
            double sum[3] = {0, 0, 0};
            if (sx > 1.0 || sy > 1.0) { // shrinking: the average of the source pixels it covers
                const int x0 = int(x * sx), x1 = std::max(x0 + 1, std::min(width, int(std::ceil((x + 1) * sx))));
                const int y0 = int(y * sy), y1 = std::max(y0 + 1, std::min(height, int(std::ceil((y + 1) * sy))));
                for (int yy = y0; yy < y1; ++yy)
                    for (int xx = x0; xx < x1; ++xx)
                        for (int c = 0; c < 3; ++c)
                            sum[c] += at(xx, yy)[c];
                for (double& s : sum)
                    s /= double((x1 - x0) * (y1 - y0));
            } else { // enlarging: between the four nearest
                const double fx = std::max(0.0, (x + 0.5) * sx - 0.5), fy = std::max(0.0, (y + 0.5) * sy - 0.5);
                const int x0 = std::min(width - 1, int(fx)), y0 = std::min(height - 1, int(fy));
                const int x1 = std::min(width - 1, x0 + 1), y1 = std::min(height - 1, y0 + 1);
                const double ax = fx - x0, ay = fy - y0;
                for (int c = 0; c < 3; ++c)
                    sum[c] = (at(x0, y0)[c] * (1 - ax) + at(x1, y0)[c] * ax) * (1 - ay) + (at(x0, y1)[c] * (1 - ax) + at(x1, y1)[c] * ax) * ay;
            }
            unsigned char* q = reinterpret_cast<unsigned char*>(&m.data[(size_t(y) * size_t(m.width) + size_t(x)) * 3]);
            for (int c = 0; c < 3; ++c)
                q[c] = static_cast<unsigned char>(std::clamp(sum[c] + 0.5, 0.0, 255.0));
        }
    return m;
}

} // namespace

// The part of a picture or page a look is about, as fractions x0, y0, x1, y1:
//  - region: a name - all, top, bottom, left, right, top-left, ..., center, with half (the default),
//    third or quarter ("top third", "bottom-right quarter", "middle-left third") - or a box in
//    percent "left,top,right,bottom" ("10,40,60,70");
//  - zoom: percent, magnifies that part: 200 its middle half, 400 a quarter of it across...
//  - at: "x,y" in percent of the whole, the point to zoom in around (default the part's middle).
// False with the reason when the region cannot be read.
bool LocalAgentTools::lookBox(const Json& args, float box[4], std::string& why)
{
    box[0] = 0.0f, box[1] = 0.0f, box[2] = 1.0f, box[3] = 1.0f;
    // The region as text, or as a list of four numbers ([10, 40, 60, 70]), under the names a model
    // may give it.
    std::string region;
    for (const char* key : {"region", "box", "area", "bbox", "part"})
        if (const Json* v = args.find(key)) {
            if (v->isString())
                region = v->asString();
            else if (v->isArray())
                for (const Json& n : v->items())
                    region += (region.empty() ? "" : ",") + (n.isNumber() ? std::to_string(n.asNumber()) : n.asString());
            break;
        }
    // No region, but four numbers under another name ("at", "coordinates", "crop"): that is the box.
    if (trimmed(region).empty())
        for (const auto& [key, v] : args.members()) {
            if (key == "path" || key == "page" || key == "zoom" || key == "detail")
                continue;
            std::string text = v.isString() ? v.asString() : std::string();
            if (v.isArray())
                for (const Json& n : v.items())
                    text += (text.empty() ? "" : ",") + (n.isNumber() ? std::to_string(n.asNumber()) : n.asString());
            int numbers = 0;
            bool inNumber = false;
            for (char c : text) {
                const bool digit = std::isdigit(static_cast<unsigned char>(c)) || c == '.';
                numbers += digit && !inNumber;
                inNumber = digit;
            }
            if (numbers == 4) {
                region = text;
                break;
            }
        }
    region = lower(trimmed(region));
    std::vector<float> numbers;
    {
        std::string n;
        for (size_t i = 0; i <= region.size(); ++i) {
            const char c = i < region.size() ? region[i] : ' ';
            if (std::isdigit(static_cast<unsigned char>(c)) || c == '.')
                n += c;
            else if (!n.empty()) {
                numbers.push_back(float(std::atof(n.c_str())));
                n.clear();
            }
        }
    }
    auto has = [&](const char* word) { return region.find(word) != std::string::npos; };
    if (region.empty() || region == "all" || region == "whole" || region == "page" || region == "everything") {
        // all of it
    } else if (numbers.size() == 4) {
        const float scale = std::max({numbers[0], numbers[1], numbers[2], numbers[3]}) <= 1.0f ? 1.0f : 100.0f;
        for (int k = 0; k < 4; ++k)
            box[k] = std::clamp(numbers[size_t(k)] / scale, 0.0f, 1.0f);
        if (box[2] <= box[0] || box[3] <= box[1]) // given as left, top, width, height
            box[2] = std::min(1.0f, box[0] + box[2]), box[3] = std::min(1.0f, box[1] + box[3]);
        if (box[2] - box[0] < 0.01f || box[3] - box[1] < 0.01f) {
            why = "the region '" + region + "' is empty: give left,top,right,bottom in percent, e.g. \"10,40,60,70\"";
            return false;
        }
    } else {
        const float s = has("quarter") ? 0.25f : has("third") ? 1.0f / 3.0f : 0.5f;
        const bool top = has("top") || has("upper"), bottom = has("bottom") || has("lower");
        const bool left = has("left"), right = has("right"), middle = has("center") || has("centre") || has("middle");
        if (!top && !bottom && !left && !right && !middle) {
            why = "cannot read the region '" + region + "': use all, top, bottom, left, right, top-left, top-right, "
                  "bottom-left, bottom-right, center (with third or quarter for smaller parts, e.g. \"top third\"), or "
                  "left,top,right,bottom in percent, e.g. \"10,40,60,70\"";
            return false;
        }
        const int v = top ? 0 : bottom ? 2 : middle ? 1 : -1, h = left ? 0 : right ? 2 : middle ? 1 : -1;
        auto span = [s](int where, float& a, float& b) {
            if (where == 0)
                a = 0.0f, b = s;
            else if (where == 2)
                a = 1.0f - s, b = 1.0f;
            else if (where == 1)
                a = 0.5f - s * 0.5f, b = 0.5f + s * 0.5f;
        };
        span(v, box[1], box[3]);
        span(h, box[0], box[2]);
    }
    // Zoom in on the part: around its middle, or the point asked for.
    const int zoom = std::clamp(argInt(args, "zoom", 100), 0, 1600);
    if (zoom > 100) {
        float cx = (box[0] + box[2]) * 0.5f, cy = (box[1] + box[3]) * 0.5f;
        std::string at = args.getString("at");
        if (const Json* v = args.find("at"); v && v->isArray())
            for (const Json& n : v->items())
                at += (at.empty() ? "" : ",") + (n.isNumber() ? std::to_string(n.asNumber()) : n.asString());
        if (float x = 0, y = 0; !at.empty() && std::sscanf(at.c_str(), "%f%*[ ,;xX]%f", &x, &y) == 2) {
            const float scale = x <= 1.0f && y <= 1.0f && at.find('.') != std::string::npos ? 1.0f : 100.0f;
            cx = std::clamp(x / scale, 0.0f, 1.0f), cy = std::clamp(y / scale, 0.0f, 1.0f);
        }
        const float w = (box[2] - box[0]) * 100.0f / float(zoom), h = (box[3] - box[1]) * 100.0f / float(zoom);
        box[0] = std::clamp(cx - w * 0.5f, 0.0f, 1.0f - w), box[1] = std::clamp(cy - h * 0.5f, 0.0f, 1.0f - h);
        box[2] = box[0] + w, box[3] = box[1] + h;
    }
    return true;
}

namespace {

// A look that saw one flat colour (a margin, an empty corner): said in the reply, so the model looks
// at the whole first instead of reading something into nothing.
std::string blankNote(const LocalMedia& m)
{
    if (m.data.empty())
        return std::string();
    double sum = 0, square = 0;
    const size_t n = m.data.size();
    for (char c : m.data) {
        const double v = static_cast<unsigned char>(c);
        sum += v;
        square += v * v;
    }
    const double mean = sum / double(n), spread = std::sqrt(std::max(0.0, square / double(n) - mean * mean));
    if (spread >= 4.0)
        return std::string();
    return " This part is empty: all one colour (" + std::string(mean > 200 ? "white" : mean < 55 ? "black" : "flat") +
           "), with nothing to read. Look at the whole first (no region) to find where things are, then zoom there.";
}

// "the top-left quarter", "10-60% across, 40-70% down": what a box is, for the model's reply.
std::string boxText(const float box[4])
{
    if (box[0] <= 0.0f && box[1] <= 0.0f && box[2] >= 1.0f && box[3] >= 1.0f)
        return std::string();
    char text[96];
    std::snprintf(text, sizeof(text), "%d-%d%% across, %d-%d%% down", int(box[0] * 100 + 0.5f), int(box[2] * 100 + 0.5f),
                  int(box[1] * 100 + 0.5f), int(box[3] * 100 + 0.5f));
    return text;
}

void reply(const LocalAgentTools::Done& done, std::string text)
{
    Output out;
    out.text = std::move(text);
    done(std::move(out));
}

} // namespace

LocalAgentTools::LocalAgentTools(viewmodel::NotepadViewModel& vm, std::function<view::MainView*()> view,
                                 interfaces::ILocalEngineService* engine)
    : vm_(vm), view_(std::move(view)), engine_(engine)
{
    if (const std::string dir = os::fs::appDataDirectory(); !dir.empty())
        memoryPath_ = dir + "/agent-memory/memories.json";
    // The tools' pictures are kept a day: older ones go when the app starts.
    if (const std::string images = imagesDirectory(); !images.empty() && os::fs::isDirectory(images)) {
        const std::int64_t dayAgo = std::int64_t(std::time(nullptr)) - 24 * 3600;
        for (const os::fs::DirEntry& e : os::fs::listDirectory(images))
            if (!e.isDirectory && os::fs::stat(images + "/" + e.name).modifiedTime < dayAgo)
                std::remove((images + "/" + e.name).c_str());
    }
    session_ = std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + "-" +
               std::to_string(reinterpret_cast<std::uintptr_t>(this)); // this run (and this object)
}

LocalAgentTools::~LocalAgentTools()
{
    *alive_ = false;
    os::mem::release(looks_); // their answers are not given: the agent goes with the app
}

void LocalAgentTools::attach()
{
    vm_.agent().setLocalTools([this](bool canEdit) { return tools(canEdit); },
                              [this](const interfaces::LocalToolCall& call, Done done) { run(call, std::move(done)); },
                              [this]() { return guide(); });
    // The user's preferences with each question: in the guide alone, a small model let them slip.
    vm_.agent().setLocalReminder([this]() {
        std::string prefs;
        const Json memories = loadMemories();
        for (const Json& m : memories.items())
            if (m.getString("kind") == "preference")
                prefs += (prefs.empty() ? "" : "; ") + m.getString("text");
        return prefs.empty() ? std::string() : "Follow my preferences in your answer: " + prefs;
    });
    // Its working notes, shown to it at each step of a question taken in steps.
    vm_.agent().setLocalNotes([this]() {
        std::string out;
        const std::vector<std::string>& n = notes();
        for (size_t i = n.size() > 40 ? n.size() - 40 : 0; i < n.size(); ++i) // the latest, within reason
            out += "- " + n[i] + "\n";
        return out;
    });
    vm_.agent().setLocalNamedFiles([this](const std::string& question) { return namedFiles(question); });
    // The task's memory of a question taken in steps, as a file next to the long-term memories:
    // what each step is given, for the user to see (rewritten as the task goes on).
    vm_.agent().setLocalTaskMemory([this](const std::string& memory) {
        if (memoryPath_.empty())
            return;
        const std::string dir = viewmodel::FileDialogViewModel::parent(memoryPath_);
        os::fs::createDirectories(dir);
        os::fs::writeFile(dir + "/task.md", "# The task's memory\n\n" + memory);
    });
    // A skill for a step of a plan: its instructions without the front matter; one the index does
    // not list (the plan made it up), or the planning skill itself, is none.
    vm_.agent().setLocalSkill([](const std::string& name) {
        if (name == "planning" || skillDescription(name).empty())
            return std::string();
        std::string text = os::fs::readAsset("skills/" + name + "/SKILL.md").value_or(std::string());
        if (text.rfind("---", 0) == 0)
            if (const size_t end = text.find("\n---", 3); end != std::string::npos) {
                const size_t body = text.find('\n', end + 4);
                text.erase(0, body == std::string::npos ? text.size() : body + 1);
            }
        return text;
    });
    // Questions split into steps first, with the planning skill (resources/skills/planning).
    // The plan is made with what the open folder holds (each folder with how many files of which
    // kinds): a plan built on guessed file names fails at its first step.
    vm_.agent().setLocalPlanner([this](const std::string& question) {
        std::string text = os::fs::readAsset("skills/planning/SKILL.md").value_or(std::string());
        std::string error;
        const std::string root = resolve("", error);
        if (text.empty() || root.empty())
            return text;
        std::vector<os::fs::DirEntry> entries = os::fs::listDirectory(root);
        std::sort(entries.begin(), entries.end(), [](const os::fs::DirEntry& a, const os::fs::DirEntry& b) { return a.name < b.name; });
        std::string summary;
        int lines = 0;
        for (const os::fs::DirEntry& e : entries) {
            if (e.hidden || ++lines > 40)
                continue;
            if (!e.isDirectory) {
                summary += "- " + e.name + "\n";
                continue;
            }
            std::vector<std::string> kinds;
            size_t files = 0, tables = 0, documents = 0;
            for (const os::fs::DirEntry& f : os::fs::listDirectory(root + "/" + e.name))
                if (!f.isDirectory && !f.hidden) {
                    ++files;
                    const std::string ext = extension(f.name);
                    tables += ext == "csv" || ext == "tsv" || ext == "xlsx" || ext == "xlsm";
                    documents += isDocument(f.name) && !(ext == "csv" || ext == "tsv" || ext == "xlsx" || ext == "xlsm");
                    if (!ext.empty() && std::find(kinds.begin(), kinds.end(), "." + ext) == kinds.end() && kinds.size() < 6)
                        kinds.push_back("." + ext);
                }
            std::string k;
            for (const std::string& x : kinds)
                k += (k.empty() ? "" : " ") + x;
            const std::string use = documents > 1 && documents >= tables ? " - documents: extract_from_documents with files " + e.name
                                    : tables > 0 && tables >= documents ? " - tables: query_table" : std::string();
            summary += "- " + e.name + "/: " + std::to_string(files) + " files" + (k.empty() ? "" : " (" + k + ")") + use + "\n";
        }
        if (lines > 40)
            summary += "- ... and " + std::to_string(lines - 40) + " more\n";
        // The tables the question names: their real columns (a plan with guessed columns fails).
        std::string columns;
        const std::string q = lower(question);
        for (const std::string& f : documentsUnder(root, root, 300)) {
            const std::string ext = extension(f);
            if (ext != "csv" && ext != "tsv" && ext != "xlsx" && ext != "xlsm")
                continue;
            std::string stem = lower(fileName(f));
            stem = stem.substr(0, stem.rfind('.'));
            if (stem.size() < 3 || q.find(stem) == std::string::npos || columns.size() > 1500)
                continue;
            std::string listed;
            queryTable(Json::object().set("path", f).set("limit", 1), [&listed](Output out) { listed = out.text; });
            if (const size_t at = listed.find("Columns: "); at != std::string::npos)
                columns += "- " + f + " (a table: query_table) columns: " + listed.substr(at + 9, listed.find('\n', at) - at - 9) + "\n";
        }
        const std::string named = namedFiles(question);
        std::string skills;
        for (const auto& [name, description] : skillIndex())
            if (name != "planning" && (hears() || name != "meetings"))
                skills += "- " + name + ": " + description + "\n";
        return text + "\n\nThe skills a step can name:\n" + skills +
               "\nThe open folder holds (plan with these names; never invent a file):\n" + summary +
               (named.empty() ? std::string() : "\n" + named + "\n") +
               (columns.empty() ? std::string() : "\nTables in the question, with their exact column names:\n" + columns);
    });
}

std::vector<LocalTool> LocalAgentTools::tools(bool canEdit) const
{
    const char* detail = "How carefully to look: low (fast, ~70 tokens), normal (~280), high (small print, OCR: ~1100), "
                         "or a number of tokens from 70 to 1120";
    const char* region = "The part to look at: all (default); top, bottom, left, right, top-left, top-right, bottom-left, "
                         "bottom-right, center - a half by default, add third or quarter for less (\"bottom-right quarter\", "
                         "\"top third\"); or a box in percent \"left,top,right,bottom\" (\"55,60,100,80\")";
    const char* zoom = "Percent magnification of that part (default 100): 200 its middle half, 400 a quarter of it across";
    const char* at = "With zoom: the point to zoom in around, \"x,y\" in percent of the whole (\"70,85\")";
    std::vector<LocalTool> t = {
        // The folder.
        {"list_files", "Lists a folder of the open folder: sub-folders (ending in /) and files with their sizes.",
         schema({{"path", "string", "Folder, relative to the open folder; empty for the open folder itself"}})},
        {"read_file", "Reads a text file (code, notes, data), its lines numbered. Long files: ask for a range of lines.",
         schema({{"path", "string", "File, relative to the open folder"},
                 {"start_line", "integer", "First line, from 1 (default 1)"},
                 {"end_line", "integer", "Last line (default: 200 lines on)"}},
                {"path"})},
        {"search_files", "Finds text in every text file of the open folder: how many lines have it (exactly), and each "
                         "line's file, number and text. To count lines, this is exact.",
         schema({{"query", "string", "The text to find"},
                 {"also", "string", "Other words the line must have too, separated by commas, e.g. billing (optional)"},
                 {"include", "string", "Only these files, e.g. *.cpp;*.h or *.log (optional)"}},
                {"query"})},
        {"open_file", "Opens a file in the editor for the user to see, optionally at a line.",
         schema({{"path", "string", "File, relative to the open folder"}, {"line", "integer", "Line to show, from 1 (optional)"}},
                {"path"})},
        // Documents, pictures, sound, binaries.
        {"read_document", "Reads the text of a PDF, Word, Excel or PowerPoint file, a few pages at a time.",
         schema({{"path", "string", "Document, relative to the open folder"},
                 {"from_page", "integer", "First page or slide, from 1 (default 1); for Excel and reflowed Word, the part"},
                 {"to_page", "integer", "Last page (default: 3 pages on)"}},
                {"path"})},
        {"search_documents", "Searches inside every document in the open folder (PDF, Word, Excel, PowerPoint, text): which "
                             "files and pages mention something, with a snippet of each. Without a query, lists the "
                             "documents with their kind, pages and title.",
         schema({{"query", "string", "Words to find, e.g. termination notice period (optional: none lists the documents)"},
                 {"folder", "string", "Only this folder, relative to the open folder (default: all of it)"}})},
        {"extract_from_documents",
         "Asks ONE question of MANY documents, one document at a time (each read fresh; a long one part by part), and "
         "returns a table of what each says, with pages; it is also saved in your notes. For collecting, comparing or "
         "totalling things across documents: invoice totals, contract dates, what each report concludes.",
         schema({{"files", "string",
                  "Which documents: a folder (\"contracts\"), a pattern (\"invoices/*.pdf\"), or names separated by "
                  "commas (default: every document in the open folder, up to 30)"},
                 {"question", "string", "What to find in each, e.g. the invoice number, date and total amount"}},
                {"question"})},
        {"scan_document",
         "Looks for something through long documents in overlapping windows, one window at a time, and says where "
         "it is (pages) with the passage quoted. For finding a clause, a figure or a mention in a long report, then "
         "reading or looking at those pages.",
         schema({{"path", "string", "The document, or several: a folder, a pattern (\"reports/*.pdf\") or names with commas"},
                 {"find", "string", "What to look for, e.g. the notice period for ending the contract"},
                 {"window", "string", "small (3,000 characters), normal (6,000, default), large (10,000), or a number"},
                 {"overlap", "integer", "Percent of a window repeated in the next (default 20)"}},
                {"path", "find"})},
        {"read_closely",
         "Reads one file closely, a small part at a time (overlapping, with line numbers), for an objective, and "
         "keeps notes of what each part holds for it; then answers from the notes. For a long file, one the other "
         "tools cannot make sense of (a log, an export, a jumbled PDF text, an odd format), or when every instance "
         "or an exact figure matters. Slower: a few seconds a part.",
         schema({{"path", "string", "The file, relative to the open folder"},
                 {"objective", "string", "What to find or collect, e.g. every payment over 500 EUR, with its date and payee"},
                 {"mode", "string", "answer (default: the notes, then an answer from them), collect (the notes only), or find "
                                    "(stop at the first part that has it)"},
                 {"window", "string", "small (2,500 characters), normal (4,500, default) or large (8,000)"}},
                {"path", "objective"})},
        {"scan_image",
         "Looks for something in a picture in overlapping tiles, one at a time, then looks again finer where it was "
         "seen (coarse to fine), and says in which region it is. For finding small things: a stamp, a signature, a "
         "number in a photo, a label on a drawing; then look_at_image on that region.",
         schema({{"path", "string", "Picture, relative to the open folder"},
                 {"find", "string", "What to look for, e.g. the date stamp, or the serial number"},
                 {"grid", "integer", "Tiles across and down at first: 1 to 4 (default 2)"},
                 {"levels", "integer", "How many times a tile where it was seen is cut finer: 1 to 3 (default 2)"},
                 {"region", "string", "Only this part of the picture (as look_at_image)"},
                 {"detail", "string", "How carefully each tile is looked at (default normal)"}},
                {"path", "find"})},
        {"note", "Writes a line in your working notes: something you found that later steps will need. Your notes are "
                 "shown to you at each step, however long the conversation gets.",
         schema({{"text", "string", "e.g. Invoice 2041 (invoices/march.pdf p.1): total 1,250.00 EUR"}}, {"text"})},
        {"read_notes", "Reads back your working notes for this conversation.", schema({})},
        {"look_at_page", "Shows a page of a PDF, Word or PowerPoint document in the viewer and looks at it as a picture: "
                         "for tables, charts, drawings, layout and scanned pages.",
         schema({{"path", "string", "Document, relative to the open folder (default: the one shown)"},
                 {"page", "integer", "Page or slide, from 1 (default 1)"},
                 {"region", "string", region},
                 {"zoom", "integer", zoom},
                 {"at", "string", at},
                 {"detail", "string", detail}})},
        {"look_at_image", "Looks at a picture file (PNG, JPEG, GIF, BMP, TIFF, WebP and more), whole or zoomed into a part.",
         schema({{"path", "string", "Picture, relative to the open folder"},
                 {"region", "string", region},
                 {"zoom", "integer", zoom},
                 {"at", "string", at},
                 {"detail", "string", detail}},
                {"path"})},
        {"listen_to", "Listens to a short recording (WAV, MP3 or FLAC): speech, music, sounds. For meetings use transcribe.",
         schema({{"path", "string", "Recording, relative to the open folder"}}, {"path"})},
        {"transcribe", "Writes down word for word what is said in a recording (WAV, MP3, FLAC), however long, with "
                       "[mm:ss] times, into <recording>.transcript.md next to it, and gives you the text to work with.",
         schema({{"path", "string", "Recording, relative to the open folder"}}, {"path"})},
        {"inspect_binary", "Shows what EasyNotepad's binary view shows of a program or library (.exe .dll .so .o .a .lib): "
                           "its kind, sections, libraries, and exported and imported functions with readable C++ names.",
         schema({{"path", "string", "File, relative to the open folder"},
                 {"list", "string", "overview, exports, imports, sections or libraries (default overview)"},
                 {"filter", "string", "Only names containing this (optional)"}},
                {"path"})},
        {"read_bytes", "Shows the bytes of any file as hex and text, from an offset.",
         schema({{"path", "string", "File, relative to the open folder"},
                 {"offset", "integer", "First byte (default 0)"},
                 {"length", "integer", "How many bytes (default 256, at most 4096)"}},
                {"path"})},
        // Work.
        {"calculate", "Calculates exactly: + - * / % ^ ( ), and sqrt abs round floor ceil min max sum avg pow log ln exp "
                      "sin cos tan, pi, e. Use it for EVERY number you work out - sums, differences, totals, "
                      "averages, percentages, shares, conversions, rounding - never in your head. Several at once, "
                      "separated by ';', each may be named and used by the next. Compares too: "
                      "'9.50 + 4 = 13.50' gives true or false (also != < > <= >=). Dates too: "
                      "'3 October 2025 - 18 August 2025' gives the days between, '2025-05-02 + 21' a date.",
         schema({{"expression", "string",
                  "e.g. 15% of 2340 + 7.5% of 980; or several: total = 42.50 * 1.08; each = total / 4; round(each, 2)"}},
                {"expression"})},
        {"query_table", "Filters, sorts and totals the rows of a CSV file or an Excel sheet. Totals per group: group_by and "
                        "aggregate. Which group has the most: group_by Region, aggregate sum(Sales), sort 'sum(Sales) desc', "
                        "limit 1. Call it again for another grouping.",
         schema({{"path", "string", "CSV, TSV or Excel file, relative to the open folder"},
                 {"sheet", "string", "Excel: the sheet's name (default the first)"},
                 {"where", "string", "Conditions joined by 'and', e.g. Country = Canada and Units Sold > 1000; also != >= <= contains; a month of dates: Date = June 2025"},
                 {"columns", "string", "Columns to show, comma-separated (default all)"},
                 {"group_by", "string", "A column to group by, or several separated by commas (\"day, time\": a row for each pair) (optional)"},
                 {"aggregate", "string", "count, or sum/avg/min/max of a column or of an expression of each row's columns, comma-separated, e.g. sum(Sales), count, sum(Units * Price); min and max also show the row they are on"},
                 {"sort", "string", "A column, or with group_by an aggregate such as sum(Sales); add ' desc' for largest first"},
                 {"limit", "integer", "Rows to show (default 30)"}},
                {"path"})},
        // Memory and skills.
        {"remember", "Keeps something for later conversations: a preference of the user, a lesson from a mistake, a fact "
                     "about the work, a note for the coming days, or a temporary note for this session.",
         schema({{"text", "string", "What to keep, in one sentence"},
                 {"kind", "string", "preference, lesson, fact, note (kept 7 days) or temporary (this session) (default note)"}},
                {"text"})},
        {"recall", "Lists what you remember (all, or those containing a word), with their ids.",
         schema({{"query", "string", "Only memories containing this (optional)"}, {"kind", "string", "Only this kind (optional)"}})},
        {"amend_memory", "Corrects a memory that turned out wrong or out of date.",
         schema({{"id", "integer", "The memory's id (from recall)"}, {"text", "string", "What it should say"}}, {"id", "text"})},
        {"forget_memory", "Forgets a memory.", schema({{"id", "integer", "The memory's id (from recall)"}}, {"id"})},
        {"rewrite_memories", "Compacts memories: replaces every memory of a kind with the lines you give (merge duplicates, "
                             "drop what is outdated). Recall them first.",
         schema({{"kind", "string", "preference, lesson, fact or note"}, {"lines", "string", "One memory per line"}},
                {"kind", "lines"})},
        {"show_image", "Shows a picture to the user in the chat: a picture file of the open folder, or one a tool made.",
         schema({{"path", "string", "Picture, relative to the open folder, or a path a tool gave"}}, {"path"})},
        {"read_skill", "Reads one of your skills: how to do a kind of task well with these tools. Read it before such a task.",
         schema({{"name", "string", "The skill's name, from the list in your guide"}}, {"name"})},
    };
    if (!hears()) // pictures only (Qwen): no recordings offered
        t.erase(std::remove_if(t.begin(), t.end(), [](const LocalTool& tool) { return tool.name == "listen_to" || tool.name == "transcribe"; }),
                t.end());
    if (canEdit) {
        t.push_back({"edit_file",
                     "Proposes a change to a text file: `old_text` (as in the file, found once) becomes `new_text`. The user "
                     "reviews it in the agent panel and accepts or rejects it.",
                     schema({{"path", "string", "File, relative to the open folder"},
                             {"old_text", "string", "The exact text to replace; enough lines to be found only once"},
                             {"new_text", "string", "What replaces it"}},
                            {"path", "old_text", "new_text"})});
        t.push_back({"write_file",
                     "Proposes a new file, or new contents for a whole file. The user reviews it in the agent panel.",
                     schema({{"path", "string", "File, relative to the open folder"}, {"content", "string", "The whole text"}},
                            {"path", "content"})});
    }
    return t;
}

std::string LocalAgentTools::usage(const std::string& name) const
{
    for (const LocalTool& tool : tools(true)) {
        if (tool.name != name)
            continue;
        const std::optional<Json> s = Json::parse(tool.parameters);
        Json example = Json::object();
        std::string args;
        std::vector<std::string> required;
        if (s) {
            if (const Json* req = s->find("required"))
                for (const Json& r : req->items())
                    required.push_back(r.asString());
            if (const Json* props = s->find("properties"))
                for (const auto& [key, p] : props->members()) {
                    const bool needed = std::find(required.begin(), required.end(), key) != required.end();
                    if (needed)
                        example.set(key, p.getString("type") == "integer" ? Json(1.0) : Json("..."));
                    args += "\n- " + key + " (" + p.getString("type") + (needed ? ", required" : "") + "): " + p.getString("description");
                }
        }
        return name + " " + example.dump() + "\nArguments:" + args;
    }
    std::string names;
    for (const LocalTool& tool : tools(true))
        names += (names.empty() ? "" : ", ") + tool.name;
    return "Tools: " + names;
}

std::string LocalAgentTools::guide() const
{
    std::string g;
    const std::string& root = vm_.workspaceRoot();
    g += root.empty() ? "No folder is open: you can read the open documents only.\n"
                      : "The open folder is " + root + "; paths are relative to it.\n";
    g += "Open documents:";
    const model::Document* active = vm_.activeDocument();
    for (const auto& doc : vm_.documents())
        g += " " + (doc->path.empty() ? doc->title : doc->path) + (doc.get() == active ? " (active)" : "") + ";";
    g += "\n";
    g += vm_.agent().autonomyIndex.get() == 0 ? "Mode: Edits files - edit_file and write_file only propose changes, which the "
                                                "user reviews and accepts: say the change waits for their review, never that "
                                                "it is done.\n"
                                              : "Mode: Plan only - you cannot change files; describe the changes instead.\n";
    const bool ears = hears();
    g += vm_.localSetup().projectorPath.empty()
             ? std::string(ears ? "You cannot see pictures or hear recordings yet: the user can download your eyes and ears in "
                                  "the agent set-up (the gear). Use read_document for documents meanwhile.\n"
                                : "You cannot see pictures yet: the user can download your eyes in the agent set-up (the "
                                  "gear). Use read_document for documents meanwhile.\n")
             : std::string(ears ? "You can see pictures and pages (look_at_image, look_at_page) and hear recordings (listen_to).\n"
                                : "You can see pictures and pages (look_at_image, look_at_page).\n");
    if (!ears)
        g += "You cannot hear recordings (.wav .mp3 .flac): if a question needs one, say so; a Gemma model can.\n";
    g += "Which tool for which file:\n"
         "- code, notes (.cpp .py .md .txt .json ...): read_file; find text in them with search_files\n"
         "- tables (.csv .tsv .xlsx): query_table to filter, sort, group and total rows; read_document for an overview\n"
         "- documents (.pdf .docx .xlsx .pptx): read_document for the text; look_at_page to see a page (tables, charts)\n"
         "- pictures (.png .jpg .gif .webp ...): look_at_image\n"
         + std::string(ears ? "- recordings (.wav .mp3 .flac): listen_to for a short one; transcribe for a meeting or anything long\n"
                            : "") +
         "- programs and libraries (.exe .dll .so .o .a .lib): inspect_binary; read_bytes for raw bytes of any file\n"
         "- a folder: list_files\n"
         "- several documents at once: search_documents to find which mention something (or, with no query, to see "
         "them all); extract_from_documents to ask one question of each of them and get a table (read the "
         "multiple-documents skill first)\n"
         "- one long document, or a few: scan_document to find where something is (pages), then read those pages\n"
         "- a long file, or one the other tools cannot make sense of (a log, an export, an odd format), or when every "
         "instance or an exact figure matters: read_closely with the objective\n"
         "- a small thing in a picture: scan_image to find where it is, then look_at_image on that region\n"
         "- what you found so far: note it (note), read it back (read_notes)\n"
         "Any arithmetic: calculate (never do sums in your head).\n"
         "Call a tool as name {\"argument\": value}. If a tool says the call was wrong, read its example and call it again.\n";
    g += memorySummary();
    // The skills: their names and what each is for (resources/skills/<name>/SKILL.md).
    std::string skills;
    if (const std::optional<std::string> index = os::fs::readAsset("skills/index.json"))
        if (const std::optional<Json> j = Json::parse(*index); j && j->isArray())
            for (const Json& s : j->items())
                if (ears || s.getString("name") != "meetings") // transcribing: not without ears
                    skills += "- " + s.getString("name") + ": " + s.getString("description") + "\n";
    if (!skills.empty())
        g += "Your skills (a step of a plan comes with those it needs; otherwise read_skill <name> before a task of "
             "that kind):\n" + skills;
    return g;
}

std::string LocalAgentTools::resolve(const std::string& path, std::string& error) const
{
    std::string p = path;
    std::replace(p.begin(), p.end(), '\\', '/');
    while (!p.empty() && p.back() == '/')
        p.pop_back();
    for (const auto& doc : vm_.documents())
        if (!p.empty() && (doc->path == p || doc->title == p))
            return vm_.absolutePath(doc->path.empty() ? doc->title : doc->path);
    const std::string& root = vm_.workspaceRoot();
    if (root.empty()) {
        error = "no folder is open: only the open documents can be read";
        return {};
    }
    std::string base = root;
    std::replace(base.begin(), base.end(), '\\', '/');
    while (!base.empty() && base.back() == '/')
        base.pop_back();
    // A small model often names the open folder itself ("office-samples", "cpp/office-samples", the
    // whole path) or starts a path with it: those mean the folder, and paths inside it.
    if (p.empty() || p == "." || p == "/")
        return base;
    // A path that is there as given is that path: a folder named like the open one inside it
    // ("data/loans.csv" in a folder called data) is not the open folder's name repeated.
    if (!viewmodel::FileDialogViewModel::isAbsolute(p) && p.find("..") == std::string::npos &&
        os::fs::stat(base + "/" + p).exists)
        return base + "/" + p;
    if (!viewmodel::FileDialogViewModel::isAbsolute(p))
        for (size_t slash = base.find('/'); slash != std::string::npos; slash = base.find('/', slash + 1)) {
            const std::string tail = base.substr(slash + 1);
            if (p == tail)
                return base;
            if (p.rfind(tail + "/", 0) == 0) {
                p = p.substr(tail.size() + 1);
                break;
            }
        }
    const std::string absolute = viewmodel::FileDialogViewModel::isAbsolute(p) ? p : vm_.absolutePath(p);
    if (p.find("..") != std::string::npos || (absolute != base && absolute.rfind(base + "/", 0) != 0)) {
        error = "'" + path + "' is outside the open folder";
        return {};
    }
    return absolute;
}

std::string LocalAgentTools::fileText(const std::string& absolute, bool& open) const
{
    for (const auto& doc : vm_.documents())
        if (!doc->path.empty() && vm_.absolutePath(doc->path) == absolute && doc->kind == model::FileKind::Text) {
            open = true;
            return doc->buffer.text();
        }
    open = false;
    return os::fs::readFile(absolute).value_or(std::string());
}

std::string LocalAgentTools::fileNamed(const std::string& path) const
{
    std::string error;
    const std::string root = resolve("", error);
    std::string want = path;
    std::replace(want.begin(), want.end(), '\\', '/');
    want = lower(fileName(want));
    if (root.empty() || want.size() < 3 || want.find('.') == std::string::npos)
        return {};
    std::string found;
    std::vector<std::string> todo{root};
    size_t seen = 0;
    while (!todo.empty() && seen < 5000) {
        const std::string dir = todo.back();
        todo.pop_back();
        for (const os::fs::DirEntry& e : os::fs::listDirectory(dir)) {
            if (e.hidden || e.name == "node_modules" || e.name == ".git" || ++seen > 5000)
                continue;
            const std::string full = dir + "/" + e.name;
            if (e.isDirectory) {
                if (todo.size() < 2000)
                    todo.push_back(full);
            } else if (lower(e.name) == want) {
                if (!found.empty())
                    return {}; // two files of that name: the model must say which
                found = full.substr(root.size() + 1);
            }
        }
    }
    return found;
}

void LocalAgentTools::run(const interfaces::LocalToolCall& call, Done done)
{
    // The arguments, forgiving the slips a small model makes: a code fence around them, single
    // quotes, a comma before a closing brace.
    std::string text = call.arguments;
    auto trim = [](std::string s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
            s.erase(s.begin());
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
            s.pop_back();
        return s;
    };
    text = trim(text);
    if (text.rfind("```", 0) == 0) {
        const size_t nl = text.find('\n');
        const size_t end = text.rfind("```");
        text = nl != std::string::npos && end > nl ? trim(text.substr(nl + 1, end - nl - 1)) : text;
    }
    if (text.empty())
        text = "{}";
    std::optional<Json> parsed = Json::parse(text);
    if (!parsed || !parsed->isObject()) {
        std::string fixed = text;
        if (fixed.find('"') == std::string::npos)
            std::replace(fixed.begin(), fixed.end(), '\'', '"');
        for (size_t at = fixed.find(','); at != std::string::npos; at = fixed.find(',', at + 1)) {
            size_t next = at + 1;
            while (next < fixed.size() && std::isspace(static_cast<unsigned char>(fixed[next])))
                ++next;
            if (next < fixed.size() && (fixed[next] == '}' || fixed[next] == ']'))
                fixed.erase(at, 1);
        }
        parsed = Json::parse(fixed);
    }
    if (!parsed || !parsed->isObject())
        return reply(done, "The arguments of " + call.name + " are not a JSON object (" + call.arguments.substr(0, 120) +
                               "). Call it like this:\n" + usage(call.name));
    // calculate with its one argument under another name ({"x": "2 + 2"}, {"2": ...}): that is the
    // expression. A small model named it so, and was told "needs `expression`" each time.
    if (call.name == "calculate" && !parsed->find("expression") && !parsed->find("expressions") && parsed->members().size() == 1) {
        Json value = parsed->members().front().second; // a bare number stays one: calculate says to quote it
        parsed = Json::object().set("expression", std::move(value));
    }
    const Json args = std::move(*parsed);

    using Handler = void (LocalAgentTools::*)(const Json&, Done);
    static const std::vector<std::pair<const char*, Handler>> handlers = {
        {"list_files", &LocalAgentTools::listFiles},       {"read_file", &LocalAgentTools::readFile},
        {"search_files", &LocalAgentTools::searchFiles},   {"read_document", &LocalAgentTools::readDocument},
        {"search_documents", &LocalAgentTools::searchDocuments}, {"extract_from_documents", &LocalAgentTools::extractFromDocuments},
        {"note", &LocalAgentTools::note},                   {"read_notes", &LocalAgentTools::readNotes},
        {"scan_document", &LocalAgentTools::scanDocument}, {"scan_image", &LocalAgentTools::scanImage},
        {"look_at_page", &LocalAgentTools::lookAtPage},    {"look_at_image", &LocalAgentTools::lookAtImage},
        {"listen_to", &LocalAgentTools::listenTo},         {"transcribe", &LocalAgentTools::transcribe},
        {"inspect_binary", &LocalAgentTools::inspectBinary}, {"read_bytes", &LocalAgentTools::readBytes},
        {"read_closely", &LocalAgentTools::readClosely},
        {"open_file", &LocalAgentTools::openFile},         {"calculate", &LocalAgentTools::calculate},
        {"query_table", &LocalAgentTools::queryTable},     {"remember", &LocalAgentTools::remember},
        {"recall", &LocalAgentTools::recall},              {"amend_memory", &LocalAgentTools::amendMemory},
        {"forget_memory", &LocalAgentTools::forgetMemory}, {"rewrite_memories", &LocalAgentTools::rewriteMemories},
        {"edit_file", &LocalAgentTools::editFile},         {"write_file", &LocalAgentTools::writeFile},
        {"read_skill", &LocalAgentTools::readSkill},
        {"show_image", &LocalAgentTools::showImage},
    };
    if (!hears() && (call.name == "listen_to" || call.name == "transcribe"))
        return reply(done, "This local model cannot hear recordings yet: tell the user so (its ears can be downloaded in the "
                           "agent set-up, or a Gemma model hears).");
    for (const auto& [name, handler] : handlers) {
        if (call.name != name)
            continue;
        // The required arguments are there: else say how to call it, with an example.
        for (const LocalTool& tool : tools(true))
            if (tool.name == call.name)
                if (const std::optional<Json> s = Json::parse(tool.parameters))
                    if (const Json* req = s->find("required"))
                        for (const Json& r : req->items())
                            if (!args.find(r.asString()))
                                return reply(done, call.name + " needs `" + r.asString() + "`. Call it like this:\n" + usage(call.name));
        // A file that is not there (a name the model guessed): say so, with what its folder holds.
        if (const std::string path = args.getString("path"); !path.empty() && call.name != "write_file") {
            std::string error;
            const std::string absolute = resolve(path, error);
            const std::string root = resolve("", error);
            if (!absolute.empty() && !root.empty() && !os::fs::stat(absolute).exists) {
                // A wrong folder or a bare name ("bin/x.dll" for binaries/x.dll): the one file of that
                // name is read, and the model told which.
                if (const std::string found = fileNamed(path); !found.empty()) {
                    Json fixed = args;
                    fixed.set("path", found);
                    const std::string note = "(There is no " + path + ": this is " + found + ".)\n";
                    (this->*handler)(fixed, [done = std::move(done), note, found](viewmodel::AgentViewModel::ToolOutput out) {
                        out.text = note + out.text;
                        out.read = found;
                        done(std::move(out));
                    });
                    return;
                }
                std::string dir = absolute;
                while (dir.size() > root.size() && !os::fs::stat(dir).isDirectory)
                    dir = dir.substr(0, dir.find_last_of('/') == std::string::npos ? 0 : dir.find_last_of('/'));
                std::vector<os::fs::DirEntry> entries = os::fs::listDirectory(dir);
                std::sort(entries.begin(), entries.end(), [](const os::fs::DirEntry& a, const os::fs::DirEntry& b) { return a.name < b.name; });
                std::string names;
                int n = 0;
                for (const os::fs::DirEntry& e : entries)
                    if (!e.hidden && ++n <= 40)
                        names += (names.empty() ? "" : ", ") + e.name + (e.isDirectory ? "/" : "");
                const std::string where = dir.size() > root.size() + 1 ? dir.substr(root.size() + 1) + "/" : "the open folder";
                // Documents where a table was wanted: the tool that reads the same facts from each.
                int documents = 0;
                for (const os::fs::DirEntry& e : entries) {
                    const std::string ext = extension(e.name);
                    documents += !e.isDirectory && isDocument(e.name) && ext != "csv" && ext != "tsv" && ext != "xlsx" && ext != "xlsm";
                }
                const std::string hint =
                    documents > 1 && call.name != "extract_from_documents"
                        ? " They are documents, not a table: to get the same facts from each of them, call extract_from_documents "
                          "{\"files\": \"" + (dir.size() > root.size() + 1 ? dir.substr(root.size() + 1) : std::string(".")) +
                              "\", \"question\": \"...\"}."
                        : std::string();
                return reply(done, path + " does not exist. " + where + " holds: " + (names.empty() ? "nothing" : names) +
                                       (n > 40 ? ", ..." : "") + ". Call " + call.name + " again with one of these." + hint);
            }
        }
        (this->*handler)(args, std::move(done));
        return;
    }
    reply(done, "There is no tool called '" + call.name + "'. " + usage(""));
}

void LocalAgentTools::listFiles(const Json& args, Done done)
{
    std::string error;
    const std::string path = args.getString("path");
    const std::string dir = path.empty() || path == "." ? vm_.workspaceRoot() : resolve(path, error);
    if (dir.empty())
        return reply(done, error.empty() ? "no folder is open" : error);
    bool more = false;
    std::vector<os::fs::DirEntry> entries = os::fs::listDirectory(dir, size_t(kListLimit), &more, &error);
    if (entries.empty() && !error.empty())
        return reply(done, "cannot list " + (path.empty() ? std::string("the open folder") : path) + ": " + error);
    std::sort(entries.begin(), entries.end(), [](const os::fs::DirEntry& a, const os::fs::DirEntry& b) {
        return a.isDirectory != b.isDirectory ? a.isDirectory : lower(a.name) < lower(b.name);
    });
    std::string out = (path.empty() ? std::string("The open folder") : path) + ":\n";
    std::vector<std::pair<std::string, int>> kinds; // files of each extension, counted for it
    int folders = 0;
    for (const os::fs::DirEntry& e : entries) {
        if (e.name == ".git")
            continue;
        out += e.isDirectory ? e.name + "/\n" : e.name + "  (" + std::to_string(e.size) + " bytes)\n";
        if (e.isDirectory) {
            ++folders;
            continue;
        }
        const std::string ext = extension(e.name);
        const std::string kind = ext.empty() ? std::string("without an extension") : "." + ext;
        auto k = std::find_if(kinds.begin(), kinds.end(), [&](const auto& p) { return p.first == kind; });
        if (k == kinds.end())
            kinds.emplace_back(kind, 1);
        else
            ++k->second;
    }
    if (more)
        out += "[more entries not listed]\n";
    // Counted here: a small model miscounts a list ("2 .wav files" of three).
    std::string counts;
    for (const auto& [kind, n] : kinds)
        counts += (counts.empty() ? "" : ", ") + std::to_string(n) + " " + kind;
    if (folders)
        counts += (counts.empty() ? "" : ", ") + std::to_string(folders) + (folders == 1 ? " folder" : " folders");
    if (!counts.empty())
        out += std::string(more ? "Listed: " : "In all: ") + counts + "\n";
    reply(done, out);
}

namespace {
// Whether a word of `text` (lower case) starts with `value` (lower case, 3 letters or more): "sun" in
// "on sundays", "dinner" in "at dinner".
bool wordStarts(const std::string& text, const std::string& value)
{
    if (value.size() < 3)
        return false;
    for (size_t at = text.find(value); at != std::string::npos; at = text.find(value, at + 1))
        if (at == 0 || !std::isalnum(static_cast<unsigned char>(text[at - 1])))
            return true;
    return false;
}

// A table's columns, for a plan: "total_bill, day [Sun, Sat, Thur, Fri], time [Dinner, Lunch]", with
// the values of a text column that has few of them when `question` (lower case) names one ("at
// dinner"). From every row (a file sorted by a column has its last values far down); empty when the
// file is not a table it can read.
std::string tableColumns(const std::string& absolute, const std::string& question)
{
    const os::fs::FileInfo st = os::fs::stat(absolute);
    if (!st.exists || st.isDirectory || st.size > 16u * 1024 * 1024)
        return {};
    std::optional<std::string> bytes = os::fs::readFile(absolute);
    if (!bytes)
        return {};
    std::vector<std::vector<std::string>> rows; // the header, then the rows
    const std::string ext = extension(absolute);
    if (ext == "xlsx" || ext == "xlsm") {
        const model::viewer::ViewerDocument doc = model::viewer::readViewerDocument(absolute, std::move(*bytes));
        if (doc.book.sheets.empty())
            return {};
        const auto& sheetRows = doc.book.sheets.front().rows;
        const size_t stride = std::max<size_t>(1, sheetRows.size() / 4000); // spread over the sheet
        for (size_t i = 0; i < sheetRows.size(); i = i == 0 ? 1 : i + stride) {
            const model::viewer::Row& row = sheetRows[i];
            if (row.cells.empty())
                continue;
            std::vector<std::string> values(row.cells.back().column + 1);
            for (const model::viewer::Cell& c : row.cells)
                values[c.column] = c.text;
            rows.push_back(std::move(values));
        }
    } else {
        const model::csv::Table t = model::csv::parse(*bytes, model::csv::detectDelimiter(*bytes, absolute));
        const size_t stride = std::max<size_t>(1, t.rowCount() / 4000); // spread over the file, not all of a big one
        for (size_t r = 0; r < t.rowCount(); r = r == 0 ? 1 : r + stride) {
            std::vector<std::string> values;
            for (int c = 0; c < t.columns; ++c)
                values.emplace_back(t.cell(r, c));
            rows.push_back(std::move(values));
        }
    }
    if (rows.empty())
        return {};
    // Kept apart from the list it goes in ("a (use), its columns: ...; b (use)").
    auto clean = [](std::string s) {
        for (char& ch : s)
            if (ch == ';' || ch == '(' || ch == ')' || ch == '[' || ch == ']' || ch == '\n' || ch == '\r')
                ch = ' ';
        return trimmed(s);
    };
    std::string out;
    for (size_t c = 0; c < rows[0].size() && c < 40; ++c) {
        const std::string name = clean(rows[0][c]);
        if (name.empty())
            continue;
        std::vector<std::string> values;
        bool text = rows.size() > 2;
        for (size_t r = 1; r < rows.size() && text; ++r) {
            const std::string v = c < rows[r].size() ? clean(rows[r][c]) : std::string();
            if (v.empty())
                continue;
            char* end = nullptr;
            std::strtod(v.c_str(), &end);
            if (end && *end == '\0')
                text = false; // a number: its values are not words a question names
            else if (std::find(values.begin(), values.end(), v) == values.end())
                values.push_back(v);
            if (values.size() > 6)
                text = false;
        }
        out += (out.empty() ? "" : ", ") + name;
        // Only for a column whose values the question names ("at dinner"; "Sun" in "on Sundays"): the
        // values of the others draw a small model into filters nobody asked for.
        const bool named = std::any_of(values.begin(), values.end(), [&](const std::string& v) { return wordStarts(question, lower(v)); });
        if (text && named && values.size() >= 2 && rows.size() > values.size() * 2) {
            std::string list;
            for (const std::string& v : values)
                list += (list.empty() ? "" : ", ") + v.substr(0, 30);
            out += " [" + list + "]";
        }
    }
    // The columns the question names by their words ("party size": size; "flipper length":
    // flipper_length_mm), every word of 3 letters or more: a small model grouped by another.
    std::string named;
    for (size_t c = 0; c < rows[0].size() && c < 40; ++c) {
        const std::string name = clean(rows[0][c]);
        std::vector<std::string> words;
        std::string word;
        for (const char ch : lower(name) + " ") {
            if (std::isalnum(static_cast<unsigned char>(ch))) {
                word += ch;
            } else {
                if (word.size() >= 3)
                    words.push_back(word);
                word.clear();
            }
        }
        if (!words.empty() && std::all_of(words.begin(), words.end(), [&](const std::string& w) { return wordStarts(question, w); }))
            named += (named.empty() ? "" : ", ") + name;
    }
    if (!named.empty())
        out += " - named in my question: " + named;
    return out;
}
} // namespace

std::string LocalAgentTools::namedFiles(const std::string& question)
{
    std::string error;
    os::mem::release(question_);
    const std::string root = resolve("", error);
    if (root.empty() || question.empty())
        return {};
    std::string q = lower(question);
    std::replace(q.begin(), q.end(), '\\', '/');
    question_ = q; // for query_table: the values the question names
    auto word = [&](size_t i) { return i < q.size() && (std::isalnum(static_cast<unsigned char>(q[i])) || q[i] == '_'); };
    // At `at` in the question as a whole name: not inside a longer word or path ("a.wav" in "data.wav",
    // "meeting.wav" in "meeting.wav.transcript").
    auto whole = [&](size_t at, size_t length) {
        const size_t end = at + length;
        return !word(at - 1) && (at == 0 || q[at - 1] != '/') && !word(end) && !(end < q.size() && q[end] == '.' && word(end + 1));
    };
    auto mentions = [&](const std::string& name) {
        for (size_t at = q.find(name); at != std::string::npos; at = q.find(name, at + 1))
            if (whole(at, name.size()))
                return true;
        return false;
    };
    std::vector<std::string> byPath, byName;
    std::vector<std::string> todo{root};
    size_t seen = 0;
    while (!todo.empty() && seen < 5000) {
        const std::string dir = todo.back();
        todo.pop_back();
        for (const os::fs::DirEntry& e : os::fs::listDirectory(dir)) {
            if (e.hidden || e.name == "node_modules" || e.name == "build" || e.name == "bin" || e.name == "obj" || ++seen > 5000)
                continue;
            const std::string full = dir + "/" + e.name;
            if (e.isDirectory) {
                if (todo.size() < 2000)
                    todo.push_back(full);
                continue;
            }
            const std::string rel = full.substr(root.size() + 1);
            const std::string name = lower(e.name);
            if (rel.find('/') != std::string::npos && mentions(lower(rel)))
                byPath.push_back(rel);
            else if (name.size() >= 5 && name.find('.') != std::string::npos && mentions(name))
                byName.push_back(rel);
        }
    }
    // By name only when no path was given for it and the name is not ambiguous.
    std::vector<std::string> files = byPath;
    for (const std::string& f : byName) {
        const std::string name = lower(fileName(f));
        const bool pathed = std::any_of(byPath.begin(), byPath.end(), [&](const std::string& p) { return lower(fileName(p)) == name; });
        const long same = std::count_if(byName.begin(), byName.end(), [&](const std::string& o) { return lower(fileName(o)) == name; });
        if (!pathed && same == 1)
            files.push_back(f);
    }
    if (files.empty())
        return {};
    std::sort(files.begin(), files.end());
    std::string out = "In the open folder, the files my question names:";
    for (size_t i = 0; i < files.size() && i < 8; ++i) {
        const std::string& f = files[i];
        const std::string ext = extension(f);
        const char* use = ext == "csv" || ext == "tsv" || ext == "xlsx" || ext == "xlsm" ? "a table: query_table"
                          : ext == "exe" || ext == "dll" || ext == "so" || ext == "o" || ext == "a" || ext == "lib"
                              ? "a program: inspect_binary"
                          : kindOf(f) == FileKind::Document ? "a document: read_document"
                          : kindOf(f) == FileKind::Picture  ? "a picture: look_at_image"
                          : kindOf(f) == FileKind::Sound    ? (hears() ? "a recording: transcribe, or listen_to if short"
                                                                           : "a recording, which this model cannot hear")
                                                            : "text: read_file";
        std::string columns;
        if (ext == "csv" || ext == "tsv" || ext == "xlsx" || ext == "xlsm")
            if (const std::string c = tableColumns(root + "/" + f, q); !c.empty())
                columns = ", its columns: " + c;
        out += std::string(i ? ";" : "") + " " + f + " (" + use + ")" + columns;
    }
    return out + ".";
}

namespace {
std::string calendarText(const std::string& ics);
}

void LocalAgentTools::readFile(const Json& args, Done done)
{
    std::string error;
    const std::string path = args.getString("path");
    const std::string absolute = resolve(path, error);
    if (absolute.empty())
        return reply(done, error);
    bool open = false;
    const std::string text = fileText(absolute, open);
    if (text.empty() && !open && !os::fs::exists(absolute))
        return reply(done, path + " does not exist");
    if (kindOf(path) != FileKind::Text)
        return reply(done, path + " is not a text file. " + whichTool(path, hears()));
    if (looksBinary(text))
        return reply(done, path + " is a binary file (a program, library or data): the tools cannot read it as text.");
    std::vector<std::string> lines;
    for (size_t from = 0;;) {
        const size_t nl = text.find('\n', from);
        lines.push_back(text.substr(from, nl == std::string::npos ? std::string::npos : nl - from));
        if (nl == std::string::npos)
            break;
        from = nl + 1;
    }
    const int total = int(lines.size());
    const int first = std::clamp(argInt(args, "start_line", 1), 1, std::max(1, total));
    const int last = std::clamp(argInt(args, "end_line", first + kReadLines - 1), first, total);
    std::string out = path + (open ? " (open in the editor)" : "") + ", lines " + std::to_string(first) + "-" +
                      std::to_string(last) + " of " + std::to_string(total) + ":\n";
    // A calendar: its events as dates and times first, then its lines.
    if (first == 1 && extension(path) == "ics")
        if (const std::string events = calendarText(text); !events.empty())
            out = path + ": a calendar. " + events + "\nIts lines " + std::to_string(first) + "-" + std::to_string(last) + " of " +
                  std::to_string(total) + ":\n";
    for (int i = first; i <= last; ++i) {
        std::string line = lines[size_t(i - 1)];
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        out += std::to_string(i) + "| " + line + "\n";
    }
    // Only part of a long file: how to go through all of it for something.
    if (last < total)
        out += "[" + std::to_string(total - last) + " more lines: read on with start_line, or read_closely {\"path\": \"" + path +
               "\", \"objective\": \"...\"} to go through all of it for what you need]\n";
    // A transcript written earlier ("voicemail.wav.transcript.md"): which recording it is of, and the
    // other recordings beside it (a model answered about a voicemail from a team call's transcript).
    out += transcriptNote(path, absolute);
    reply(done, out);
}

// For a transcript written earlier, named "<recording>.transcript.md" ("voicemail 0417.transcript
// (2).md", with or without the recording's extension): which recording it is of, and the other
// recordings beside it when the question names none. Empty for other files.
std::string LocalAgentTools::transcriptNote(const std::string& path, const std::string& absolute) const
{
    const std::string name = fileName(path);
    if (lower(name).find(".transcript") == std::string::npos)
        return {};
    std::string recording = name.substr(0, lower(name).find(".transcript"));
    if (kindOf(recording) != FileKind::Sound) {
        const size_t slash = absolute.find_last_of("/\\");
        std::string found;
        if (slash != std::string::npos)
            for (const os::fs::DirEntry& e : os::fs::listDirectory(absolute.substr(0, slash)))
                if (!e.isDirectory && kindOf(e.name) == FileKind::Sound && e.name.find_last_of('.') != std::string::npos &&
                    lower(e.name.substr(0, e.name.find_last_of('.'))) == lower(recording))
                    found = e.name;
        recording = found;
    }
    if (recording.empty())
        return {};
    return "(This is the transcript of the recording " + recording + "." + othersBeside(path, absolute, recording, false) + ")\n";
}

namespace {
std::string unsearchedMedia(const std::string& root, const std::string& dir);
std::string oneLine(std::string s, size_t most);

// The files under `root` whose path holds one of `words` (lower case), as paths from it; up to 8.
std::string filesNamedBy(const std::string& root, const std::vector<std::string>& words)
{
    std::string names;
    size_t count = 0, seen = 0;
    std::vector<std::string> todo{root};
    while (!todo.empty() && seen < 3000) {
        const std::string at = todo.back();
        todo.pop_back();
        for (const os::fs::DirEntry& e : os::fs::listDirectory(at)) {
            if (e.hidden || e.name == "node_modules" || ++seen > 3000)
                continue;
            const std::string full = at + "/" + e.name;
            if (e.isDirectory) {
                if (todo.size() < 500)
                    todo.push_back(full);
                continue;
            }
            const std::string rel = full.substr(std::min(full.size(), root.size() + 1));
            const std::string low = lower(rel);
            if (std::any_of(words.begin(), words.end(), [&](const std::string& w) { return low.find(w) != std::string::npos; }) &&
                ++count <= 8)
                names += (names.empty() ? "" : ", ") + rel;
        }
    }
    return names + (count > 8 ? ", ..." : "");
}
} // namespace

void LocalAgentTools::searchFiles(const Json& args, Done done)
{
    Json params = Json::object().set("query", args.getString("query"));
    // The filter matches file names: a path given ("exports/orders.html") is its name.
    if (std::string include = args.getString("include"); !include.empty()) {
        std::string names;
        std::replace(include.begin(), include.end(), ';', ',');
        for (std::string entry : splitList(include)) {
            std::replace(entry.begin(), entry.end(), '\\', '/');
            entry = trimmed(entry.substr(entry.find_last_of('/') == std::string::npos ? 0 : entry.find_last_of('/') + 1));
            if (!entry.empty())
                names += (names.empty() ? "" : ";") + entry;
        }
        params.set("include", names.empty() ? include : names);
    }
    const Json result = handleAutomation(vm_, "search_files", params);
    if (!result.getString("error").empty())
        return reply(done, "search failed: " + result.getString("error"));
    // Lines that also hold every one of these words (any case): "ERROR" also "billing". Counted
    // exactly, all of them (a count read off a list, or by reading, was wrong).
    std::vector<std::string> also;
    for (const std::string& w : splitList(args.getString("also")))
        if (!trimmed(w).empty())
            also.push_back(lower(trimmed(w)));
    std::string listed;
    int count = 0, shown = 0;
    std::vector<std::string> inFiles;
    for (const auto& file : vm_.findInFiles().results())
        for (const auto& hit : file.hits) {
            const std::string low = lower(hit.text);
            if (std::any_of(also.begin(), also.end(), [&](const std::string& w) { return low.find(w) == std::string::npos; }))
                continue;
            ++count;
            if (std::find(inFiles.begin(), inFiles.end(), file.path) == inFiles.end())
                inFiles.push_back(file.path);
            if (++shown <= 60)
                listed += file.path + ":" + std::to_string(hit.line + 1) + ": " + hit.text + "\n";
        }
    std::string what = "'" + args.getString("query") + "'";
    for (const std::string& w : also)
        what += " and '" + w + "'";
    if (count == 0) {
        std::string error;
        const std::string root = resolve("", error);
        // A phrase no line holds ("lasagne recipe", "boiler pressure"): the files its words name
        // (lasagne.md, "boiler - user guide.txt"), and the lines with each word alone. A model took
        // "no lines" for "not in the folder" and answered so.
        std::string more;
        std::vector<std::string> words;
        {
            static const char* const common[] = {"the", "and", "for", "with", "from", "that", "this", "what", "which", "how",
                                                 "where", "when", "does", "are", "was", "all", "any", "about", "into"};
            std::string w;
            for (char c : lower(args.getString("query")) + " ") {
                if (std::isalnum(static_cast<unsigned char>(c)) || (c & 0x80)) {
                    w += c;
                    continue;
                }
                if (w.size() >= 3 && std::none_of(std::begin(common), std::end(common), [&](const char* x) { return w == x; }) &&
                    std::find(words.begin(), words.end(), w) == words.end())
                    words.push_back(w);
                w.clear();
            }
        }
        if (!root.empty() && !words.empty() && also.empty()) {
            if (const std::string named = filesNamedBy(root, words); !named.empty())
                more += "\nFiles whose names hold its words: " + named + ". Read or look at the likely one.";
            if (words.size() > 1)
                for (const std::string& word : words) {
                    Json one = params;
                    one.set("query", word);
                    if (!handleAutomation(vm_, "search_files", one).getString("error").empty())
                        continue;
                    int lines = 0;
                    std::string some;
                    for (const auto& file : vm_.findInFiles().results())
                        for (const auto& hit : file.hits)
                            if (++lines <= 4)
                                some += "\n  " + file.path + ":" + std::to_string(hit.line + 1) + ": " + oneLine(hit.text, 120);
                    if (lines)
                        more += "\nLines with '" + word + "' alone (" + std::to_string(lines) + "):" + some + (lines > 4 ? "\n  ..." : "");
                }
            // Lines with several words, not as one phrase, are counted by the tool, all of them (a
            // model read a long log by hand and counted 3 of 18).
            if (words.size() > 1) {
                std::string others;
                for (size_t k = 1; k < words.size(); ++k)
                    others += (others.empty() ? "" : ", ") + words[k];
                more += "\nTo count the lines with all of these words, in any order: search_files {\"query\": \"" + words[0] +
                        "\", \"also\": \"" + others + "\"} (leave out a word the lines may not hold).";
            }
        }
        return reply(done, "No lines with " + what + "." + more + (root.empty() ? std::string() : unsearchedMedia(root, root)));
    }
    std::string out = std::to_string(count) + (count == 1 ? " line" : " lines") + " with " + what + ", in " +
                      std::to_string(inFiles.size()) + (inFiles.size() == 1 ? " file" : " files") + ":\n" + listed;
    if (shown > 60)
        out += "[" + std::to_string(shown - 60) + " more lines not listed; the count above is all of them]\n";
    reply(done, out);
}

void LocalAgentTools::readDocument(const Json& args, Done done)
{
    std::string error;
    const std::string path = args.getString("path");
    const std::string absolute = resolve(path, error);
    if (absolute.empty())
        return reply(done, error);
    const int from = std::max(1, argInt(args, "from_page", 1));
    const int to = std::max(from, argInt(args, "to_page", from + kDocumentPages - 1));
    // Read in the background (a long document takes a while); answered on the main thread.
    std::weak_ptr<bool> alive = alive_;
    os::dispatch::runAsync([alive, absolute, ears = hears(), path, from, to, done = std::move(done)]() mutable {
        std::string out;
        std::optional<std::string> bytes = os::fs::readFile(absolute);
        if (!bytes) {
            out = "cannot read " + path;
        } else if (model::viewer::viewerFormat(absolute, *bytes).empty()) {
            out = path + " is not a PDF, Word, Excel or PowerPoint document. " + whichTool(path, ears);
        } else {
            model::viewer::ViewerDocument doc = model::viewer::readViewerDocument(absolute, std::move(*bytes));
            if (doc.needsPassword) {
                out = path + " is protected by a password. Ask the user to open it in the viewer and type it; then use "
                             "look_at_page to see its pages.";
            } else if (doc.shape == model::viewer::Shape::None) {
                out = path + " could not be read: " + doc.error;
            } else if (!doc.pages.pages.empty()) {
                const int count = int(doc.pages.pages.size());
                const int last = std::min(to, count);
                out = path + ": " + doc.format + ", " + std::to_string(count) + " pages.\n";
                // Where its tables and figures are (their captions, from the first 200 pages), so
                // the model goes to the right page instead of giving up.
                std::string captions;
                for (int i = 1; i <= std::min(count, 200); ++i) {
                    doc.pages.load(size_t(i - 1));
                    const std::string text = model::viewer::pageText(doc.pages.pages[size_t(i - 1)]);
                    doc.pages.unload(size_t(i - 1));
                    for (size_t at = 0; at < text.size();) {
                        size_t end = text.find('\n', at);
                        if (end == std::string::npos)
                            end = text.size();
                        std::string line = trimmed(text.substr(at, end - at));
                        at = end + 1;
                        const std::string l = lower(line);
                        for (const char* word : {"table ", "figure ", "fig. ", "chart ", "exhibit "})
                            if (l.rfind(word, 0) == 0 && l.size() > std::strlen(word) &&
                                std::isdigit(static_cast<unsigned char>(l[std::strlen(word)])) && captions.size() < 1500) {
                                captions += "  page " + std::to_string(i) + ": " + line.substr(0, 90) + "\n";
                                break;
                            }
                    }
                }
                if (!captions.empty())
                    out += "Tables and figures:\n" + captions;
                // The pages asked for, within what one reply holds: what does not fit is named, to
                // be read with another call.
                int shown = from - 1;
                for (int i = from; i <= last; ++i) {
                    doc.pages.load(size_t(i - 1));
                    std::string text = model::viewer::pageText(doc.pages.pages[size_t(i - 1)]);
                    doc.pages.unload(size_t(i - 1));
                    const size_t room = out.size() < kDocumentChars ? kDocumentChars - out.size() : 0;
                    if (room < 400 && i > from)
                        break;
                    const bool cut = text.size() > room;
                    if (cut)
                        text.resize(room);
                    out += "\n--- Page " + std::to_string(i) + " ---\n" + text;
                    if (cut) {
                        out += "\n[page " + std::to_string(i) + " is longer: the rest was not read]\n";
                        shown = i;
                        break;
                    }
                    shown = i;
                }
                if (from > count)
                    out += "There is no page " + std::to_string(from) + ".\n";
                else if (shown < count)
                    out += "\n[read pages " + std::to_string(from) + "-" + std::to_string(shown) + " of " + std::to_string(count) +
                           "; for more call read_document {\"path\": \"" + path + "\", \"from_page\": " + std::to_string(shown + 1) + "}]\n";
            } else {
                // No pages (Word reflowed, Excel): the text in parts.
                const std::string text = model::viewer::plainText(doc);
                const int parts = std::max(1, int((text.size() + kPartChars - 1) / kPartChars));
                const int part = std::min(from, parts);
                out = path + ": " + doc.format + ", " + std::to_string(text.size()) + " characters in " + std::to_string(parts) +
                      " parts. Part " + std::to_string(part) + ":\n" + text.substr(size_t(part - 1) * kPartChars, kPartChars);
            }
            if (!doc.notShown.empty()) {
                out += "\nNot read as text:";
                for (const std::string& s : doc.notShown)
                    out += " " + s + ";";
            }
        }
        os::dispatch::postToMain([alive, out = std::move(out), done = std::move(done)]() {
            if (alive.lock())
                reply(done, out);
        });
    });
}

// ---- Many documents -------------------------------------------------------------------------------
// A small model cannot hold many documents at once: it finds them (search_documents), asks each the
// same question in a pass of its own (extract_from_documents: the document alone in front of it, a
// long one part by part with what it found carried along), and keeps what it found in working
// notes outside the conversation (note, read_notes; repeated at each step of a planned question).

namespace {

constexpr size_t kDocumentFiles = 300;     // documents a search or a catalogue reads
constexpr size_t kExtractFiles = 30;       // documents one extract_from_documents call reads
constexpr size_t kPassChars = 9000;        // of a document in one pass (about 2,500 tokens)
constexpr size_t kShortTextChars = 6000;   // a text file extract_from_documents hands back whole
constexpr size_t kPassesPerDocument = 10;  // parts of a long document read for one question
constexpr size_t kSearchChars = 4u << 20;  // of one document searched

constexpr const char* kExtractPrompt =
    "You read one document to answer one question for a colleague. Use only this document. Give what it says: "
    "names, dates and figures copied exactly as written, with the page ([page N]) they are on. If the document "
    "does not say, answer only: not in this document. Do not calculate and do not guess. Be brief: a few lines.";

bool readableText(const std::string& path)
{
    const std::string e = extension(path);
    for (const char* t : {"txt", "md", "markdown", "csv", "tsv", "rtf", "html", "htm", "json", "xml", "log", "eml"})
        if (e == t)
            return true;
    return false;
}

bool isDocument(const std::string& path) { return kindOf(path) == FileKind::Document || readableText(path); }

struct DocumentText {
    std::string kind;               // "PDF document", "text", ...
    std::vector<std::string> pages; // page by page; one part when it has no pages
    size_t pageCount = 0;           // all its pages (more than `pages` when it was cut)
    bool paged = false;
    std::string error;
};

// A document's text, up to about `maxChars`, read here (call it off the main thread).
long long civilDays(int y, int m, int d);

// A calendar (.ics) as its events, one a line, in date order: "Thursday 12 March 2026, 14:00-15:00:
// Dentist (at Bright Smile)". A small model read DTSTART:20260312T140000 and said the afternoon was
// free. Empty when the text holds no event.
std::string calendarText(const std::string& ics)
{
    static const char* const days[] = {"Thursday", "Friday", "Saturday", "Sunday", "Monday", "Tuesday", "Wednesday"};
    static const char* const months[] = {"January", "February", "March", "April", "May", "June", "July", "August",
                                         "September", "October", "November", "December"};
    // Lines unfolded (a line starting with a space goes on the one before).
    std::vector<std::string> lines;
    std::string line;
    for (size_t i = 0; i <= ics.size(); ++i) {
        const char c = i < ics.size() ? ics[i] : '\n';
        if (c == '\r')
            continue;
        if (c != '\n') {
            line += c;
            continue;
        }
        if (!line.empty() && (line[0] == ' ' || line[0] == '\t') && !lines.empty())
            lines.back() += line.substr(1);
        else
            lines.push_back(line);
        line.clear();
    }
    struct Event {
        std::string start, end, summary, location, repeats;
    };
    std::vector<Event> events;
    Event e;
    bool in = false;
    auto unescape = [](std::string v) {
        std::string out;
        for (size_t k = 0; k < v.size(); ++k) {
            if (v[k] == '\\' && k + 1 < v.size()) {
                ++k;
                out += v[k] == 'n' || v[k] == 'N' ? ' ' : v[k];
            } else {
                out += v[k];
            }
        }
        return out;
    };
    for (const std::string& l : lines) {
        if (l == "BEGIN:VEVENT") {
            e = Event();
            in = true;
            continue;
        }
        if (l == "END:VEVENT") {
            if (in)
                events.push_back(e);
            in = false;
            continue;
        }
        const size_t colon = l.find(':');
        if (!in || colon == std::string::npos)
            continue;
        std::string name = l.substr(0, l.find_first_of(";:"));
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::toupper(c)); });
        const std::string value = l.substr(colon + 1);
        if (name == "DTSTART")
            e.start = value;
        else if (name == "DTEND")
            e.end = value;
        else if (name == "SUMMARY")
            e.summary = unescape(value);
        else if (name == "LOCATION")
            e.location = unescape(value);
        else if (name == "RRULE")
            e.repeats = value;
    }
    if (events.empty())
        return {};
    std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) { return a.start < b.start; });
    auto when = [&](const std::string& v, bool withDay) {
        if (v.size() < 8 || !std::all_of(v.begin(), v.begin() + 8, [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }))
            return v;
        const int y = std::atoi(v.substr(0, 4).c_str()), m = std::atoi(v.substr(4, 2).c_str()), d = std::atoi(v.substr(6, 2).c_str());
        std::string out;
        if (withDay && m >= 1 && m <= 12) {
            const long long n = civilDays(y, m, d);
            out = std::string(days[((n % 7) + 7) % 7]) + " " + std::to_string(d) + " " + months[m - 1] + " " + std::to_string(y);
        }
        if (v.size() >= 13 && v[8] == 'T')
            out += (out.empty() ? "" : ", ") + v.substr(9, 2) + ":" + v.substr(11, 2) + (v.back() == 'Z' ? " UTC" : "");
        return out;
    };
    std::string out = "Its events, in date order:\n";
    for (const Event& ev : events) {
        std::string t = when(ev.start, true);
        if (!ev.end.empty()) {
            const bool sameDay = ev.end.compare(0, 8, ev.start, 0, std::min<size_t>(8, ev.start.size())) == 0 && ev.end.size() >= 13;
            t += (sameDay ? "-" + when(ev.end, false) : " to " + when(ev.end, true));
        }
        out += "- " + t + ": " + (ev.summary.empty() ? std::string("(no title)") : ev.summary);
        if (!ev.location.empty())
            out += " (at " + ev.location + ")";
        if (!ev.repeats.empty())
            out += " [repeats: " + ev.repeats + "]";
        out += "\n";
    }
    return out;
}

DocumentText documentText(const std::string& absolute, size_t maxChars)
{
    DocumentText d;
    std::optional<std::string> bytes = os::fs::readFile(absolute);
    if (!bytes) {
        d.error = "cannot be read";
        return d;
    }
    if (kindOf(absolute) == FileKind::Document) {
        if (model::viewer::viewerFormat(absolute, *bytes).empty()) {
            d.error = "not a document this reads";
            return d;
        }
        model::viewer::ViewerDocument doc = model::viewer::readViewerDocument(absolute, std::move(*bytes));
        if (doc.needsPassword) {
            d.error = "protected by a password";
            return d;
        }
        if (doc.shape == model::viewer::Shape::None) {
            d.error = doc.error.empty() ? "could not be read" : doc.error;
            return d;
        }
        d.kind = doc.format;
        if (!doc.pages.pages.empty()) {
            d.paged = true;
            d.pageCount = doc.pages.pages.size();
            size_t total = 0;
            for (size_t i = 0; i < doc.pages.pages.size() && total < maxChars; ++i) {
                doc.pages.load(i);
                std::string text = model::viewer::pageText(doc.pages.pages[i]);
                doc.pages.unload(i);
                total += text.size();
                d.pages.push_back(std::move(text));
            }
        } else {
            std::string text = model::viewer::plainText(doc);
            if (text.size() > maxChars)
                text.resize(maxChars);
            d.pageCount = 1;
            d.pages.push_back(std::move(text));
        }
        return d;
    }
    if (looksBinary(*bytes)) {
        d.error = "not text";
        return d;
    }
    d.kind = "text";
    if (extension(absolute) == "ics")
        if (std::string events = calendarText(*bytes); !events.empty())
            *bytes = std::move(events);
    if (bytes->size() > maxChars)
        bytes->resize(maxChars);
    d.pageCount = 1;
    d.pages.push_back(std::move(*bytes));
    return d;
}

// The documents under `dir`, as paths relative to `root`, sorted, at most `limit`. Hidden folders
// and build output are passed over.
std::vector<std::string> documentsUnder(const std::string& root, const std::string& dir, size_t limit)
{
    std::vector<std::string> out;
    std::vector<std::string> todo{dir};
    while (!todo.empty() && out.size() < limit) {
        const std::string d = todo.front();
        todo.erase(todo.begin());
        std::vector<os::fs::DirEntry> entries = os::fs::listDirectory(d);
        std::sort(entries.begin(), entries.end(), [](const os::fs::DirEntry& a, const os::fs::DirEntry& b) { return a.name < b.name; });
        for (const os::fs::DirEntry& e : entries) {
            if (e.hidden || e.name == "node_modules" || e.name == "build" || e.name == "bin" || e.name == "obj")
                continue;
            const std::string path = d + "/" + e.name;
            if (e.isDirectory) {
                if (todo.size() < 4000)
                    todo.push_back(path);
            } else if (isDocument(e.name) && out.size() < limit) {
                out.push_back(path.size() > root.size() + 1 ? path.substr(root.size() + 1) : e.name);
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// "*.pdf", "invoice-??.docx": * any run, ? one character, ignoring case.
bool wildcard(const std::string& pattern, const std::string& text)
{
    size_t p = 0, t = 0, star = std::string::npos, mark = 0;
    auto same = [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); };
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || same(pattern[p], text[t]))) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = t;
        } else if (star != std::string::npos) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*')
        ++p;
    return p == pattern.size();
}

std::string oneLine(std::string s, size_t most)
{
    for (char& c : s)
        if (c == '\n' || c == '\r' || c == '\t')
            c = ' ';
    std::string out;
    for (char c : s)
        if (!(c == ' ' && !out.empty() && out.back() == ' '))
            out += c;
    out = trimmed(out);
    if (out.size() > most) {
        out.resize(most);
        out += "...";
    }
    return out;
}

// For a search that found nothing: the pictures and recordings under `dir`, whose words no search
// reads ("the parking fine" was only a photo, and a model searched 16 rounds for it). Empty if none.
std::string unsearchedMedia(const std::string& root, const std::string& dir)
{
    std::string names;
    size_t count = 0, seen = 0;
    std::vector<std::string> todo{dir};
    while (!todo.empty() && seen < 3000) {
        const std::string at = todo.back();
        todo.pop_back();
        for (const os::fs::DirEntry& e : os::fs::listDirectory(at)) {
            if (e.hidden || ++seen > 3000)
                continue;
            const std::string full = at + "/" + e.name;
            if (e.isDirectory) {
                if (todo.size() < 500)
                    todo.push_back(full);
                continue;
            }
            const FileKind kind = kindOf(e.name);
            if (kind != FileKind::Picture && kind != FileKind::Sound)
                continue;
            if (++count <= 6)
                names += (names.empty() ? "" : ", ") + full.substr(std::min(full.size(), root.size() + 1));
        }
    }
    if (!count)
        return {};
    return "\nNot searched: " + std::to_string(count) + (count == 1 ? " picture or recording" : " pictures and recordings") +
           ", whose words only looking or listening finds: " + names + (count > 6 ? ", ..." : "") +
           ". If what you look for may be in a photo or a recording, look at or listen to the likely one.";
}

// search_documents' answer: the catalogue (no words), or the pages that mention the words, best first.
std::string searchDocumentsText(const std::string& root, const std::string& dir, const std::string& query)
{
    const std::vector<std::string> files = documentsUnder(root, dir, kDocumentFiles);
    const std::string where = dir == root ? std::string("the open folder") : dir.substr(std::min(dir.size(), root.size() + 1));
    if (files.empty())
        return "No documents (PDF, Word, Excel, PowerPoint or text files) in " + where + ".";
    static const char* const common[] = {"the", "and", "for", "with", "from", "that", "this", "are", "was", "what", "which",
                                         "how", "many", "much", "all", "any", "who", "when", "where", "does", "about"};
    std::vector<std::string> words;
    {
        std::string w;
        for (char c : lower(query) + " ") {
            if (std::isalnum(static_cast<unsigned char>(c)) || (c & 0x80)) {
                w += c;
                continue;
            }
            if (w.size() >= 3 && std::none_of(std::begin(common), std::end(common), [&](const char* x) { return w == x; }) &&
                std::find(words.begin(), words.end(), w) == words.end())
                words.push_back(w);
            w.clear();
        }
    }
    if (words.empty()) { // the catalogue
        std::string out = std::to_string(files.size()) + " documents in " + where + (files.size() >= kDocumentFiles ? " (the first ones)" : "") + ":\n";
        for (const std::string& f : files) {
            const DocumentText t = documentText(root + "/" + f, 1500);
            std::string title;
            for (const std::string& p : t.pages) {
                size_t at = 0;
                while (title.empty() && at < p.size()) {
                    size_t end = p.find('\n', at);
                    if (end == std::string::npos)
                        end = p.size();
                    title = oneLine(p.substr(at, end - at), 80);
                    at = end + 1;
                }
                if (!title.empty())
                    break;
            }
            out += "- " + f + " - " + (t.error.empty() ? t.kind + (t.paged ? ", " + std::to_string(t.pageCount) + " pages" : std::string()) : t.error) +
                   (title.empty() ? std::string() : " - \"" + title + "\"") + "\n";
            if (out.size() > 9000) {
                out += "[more documents not listed: give a folder]\n";
                break;
            }
        }
        return out;
    }
    struct Hit {
        int score;
        std::string file;
        int page; // from 1; 0 when the document has no pages
        std::string snippet;
    };
    std::vector<Hit> hits;
    const std::string phrase = lower(trimmed(query));
    size_t mentioning = 0;
    for (const std::string& f : files) {
        const DocumentText t = documentText(root + "/" + f, kSearchChars);
        bool any = false;
        for (size_t i = 0; i < t.pages.size(); ++i) {
            const std::string l = lower(t.pages[i]);
            int distinct = 0, count = 0;
            size_t first = std::string::npos;
            for (const std::string& w : words) {
                size_t at = l.find(w);
                if (at == std::string::npos)
                    continue;
                ++distinct;
                first = std::min(first, at);
                for (; at != std::string::npos && count < 999; at = l.find(w, at + w.size()))
                    ++count;
            }
            if (distinct == 0)
                continue;
            any = true;
            const size_t exact = phrase.size() > 3 ? l.find(phrase) : std::string::npos;
            const size_t at = exact != std::string::npos ? exact : first;
            const size_t from = at > 90 ? at - 90 : 0;
            hits.push_back({distinct * 1000 + count + (exact != std::string::npos ? 5000 : 0), f, t.paged ? int(i) + 1 : 0,
                            oneLine(t.pages[i].substr(from, 220), 240)});
        }
        mentioning += any;
    }
    if (hits.empty())
        return "No document in " + where + " mentions " + query + " (" + std::to_string(files.size()) +
               " searched). Try other words, or search_documents without a query to see the documents." + unsearchedMedia(root, dir);
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.score > b.score; });
    std::string out = std::to_string(mentioning) + " of " + std::to_string(files.size()) + " documents mention it; the best pages:\n";
    for (size_t i = 0; i < hits.size() && i < 25; ++i)
        out += "- " + hits[i].file + (hits[i].page ? ", page " + std::to_string(hits[i].page) : std::string()) + ": ..." + hits[i].snippet + "...\n";
    if (hits.size() > 25)
        out += "[" + std::to_string(hits.size() - 25) + " more pages: add words to narrow it]\n";
    return out;
}

} // namespace

std::vector<std::string>& LocalAgentTools::notes()
{
    if (notesConversation_ != vm_.agent().conversation()) {
        os::mem::release(notes_);
        notesConversation_ = vm_.agent().conversation();
    }
    return notes_;
}

void LocalAgentTools::note(const Json& args, Done done)
{
    const std::string text = oneLine(args.getString("text"), 600);
    if (text.empty())
        return reply(done, "note needs the text to keep. " + usage("note"));
    std::vector<std::string>& n = notes();
    if (n.size() >= 200)
        n.erase(n.begin());
    n.push_back(text);
    reply(done, "Noted (" + std::to_string(n.size()) + " notes). They are shown to you at each step; read_notes reads them.");
}

void LocalAgentTools::readNotes(const Json&, Done done)
{
    const std::vector<std::string>& n = notes();
    std::string out;
    for (size_t i = 0; i < n.size(); ++i)
        out += std::to_string(i + 1) + ". " + n[i] + "\n";
    reply(done, out.empty() ? std::string("No notes yet in this conversation.") : "Your notes:\n" + out);
}

void LocalAgentTools::searchDocuments(const Json& args, Done done)
{
    std::string error;
    const std::string root = resolve("", error);
    if (root.empty())
        return reply(done, error);
    const std::string dir = resolve(args.getString("folder"), error);
    if (dir.empty())
        return reply(done, error);
    if (!os::fs::stat(dir).isDirectory)
        return reply(done, args.getString("folder") + " is not a folder: give a folder, or read the file with read_document.");
    const std::string query = args.getString("query");
    std::weak_ptr<bool> alive = alive_;
    os::dispatch::runAsync([alive, root, dir, query, done = std::move(done)]() mutable {
        std::string out = searchDocumentsText(root, dir, query);
        os::dispatch::postToMain([alive, out = std::move(out), done = std::move(done)]() {
            if (alive.lock())
                reply(done, out);
        });
    });
}

struct LocalAgentTools::ExtractJob {
    std::string question;
    std::vector<std::string> files;              // relative to the open folder
    std::vector<std::vector<std::string>> parts; // per document: what one pass reads
    std::vector<std::string> problems;           // per document: why it was not read, or what was left out
    std::vector<std::string> answers;
    size_t file = 0, part = 0;
    std::string found; // the current document's answer so far
    bool busy = false; // the agent was answering when it began: when it stops, so does this
};

// The documents a call names in "files" (or "path"): folders, patterns ("invoices/*.pdf") and names,
// separated by commas, or a list; nothing means the whole open folder. Relative to `root`, sorted.
bool LocalAgentTools::documentFiles(const Json& args, const std::string& root, std::vector<std::string>& files, std::string& why) const
{
    std::vector<std::string> wanted;
    const char* key = args.find("files") ? "files" : "path";
    if (const Json* v = args.find(key); v && v->isArray()) {
        for (const Json& f : v->items())
            wanted.push_back(f.isString() ? f.asString() : f.dump());
    } else {
        std::string item;
        for (char c : args.getString(key) + ",") {
            if (c == ',' || c == ';' || c == '\n') {
                if (!trimmed(item).empty())
                    wanted.push_back(trimmed(item));
                item.clear();
            } else {
                item += c;
            }
        }
    }
    if (wanted.empty())
        wanted.push_back(".");
    std::string error;
    for (const std::string& w : wanted) {
        std::string p = w;
        std::replace(p.begin(), p.end(), '\\', '/');
        if (p.find_first_of("*?") != std::string::npos) {
            // A pattern: the folder before the first wildcard, then each document under it.
            const size_t slash = p.rfind('/', p.find_first_of("*?"));
            const std::string folder = slash == std::string::npos ? std::string() : p.substr(0, slash);
            const std::string dir = resolve(folder, error);
            if (dir.empty()) {
                why = error;
                return false;
            }
            for (const std::string& f : documentsUnder(root, dir, kDocumentFiles)) {
                const std::string rest = folder.empty() ? f : f.substr(std::min(f.size(), folder.size() + 1));
                if (wildcard(slash == std::string::npos ? p : p.substr(slash + 1), rest) ||
                    wildcard(slash == std::string::npos ? p : p.substr(slash + 1), fileName(f)))
                    files.push_back(f);
            }
            continue;
        }
        const std::string absolute = resolve(p, error);
        if (absolute.empty()) {
            why = error;
            return false;
        }
        const os::fs::FileInfo st = os::fs::stat(absolute);
        if (!st.exists) {
            if (const std::string found = fileNamed(p); !found.empty() && isDocument(found)) {
                files.push_back(found); // a bare name ("a.txt" for invoices/a.txt): the one file of that name
                continue;
            }
            why = w + " does not exist. search_documents without a query lists the documents.";
            return false;
        }
        if (st.isDirectory) {
            for (const std::string& f : documentsUnder(root, absolute, kDocumentFiles))
                files.push_back(f);
        } else {
            files.push_back(absolute == root ? fileName(absolute) : absolute.substr(std::min(absolute.size(), root.size() + 1)));
        }
    }
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    if (files.empty()) {
        why = "No documents match '" + args.getString(key) + "'. search_documents without a query lists them.";
        return false;
    }
    return true;
}

void LocalAgentTools::extractFromDocuments(const Json& args, Done done)
{
    if (!engine_)
        return reply(done, "extract_from_documents cannot run here: the local model is not available to it.");
    const std::string question = trimmed(args.getString("question"));
    if (question.empty())
        return reply(done, "extract_from_documents needs the question to ask of each document. " + usage("extract_from_documents"));
    std::string error;
    const std::string root = resolve("", error);
    if (root.empty())
        return reply(done, error);
    std::vector<std::string> files;
    if (!documentFiles(args, root, files, error))
        return reply(done, error);
    // Tables are not read a document at a time: query_table counts, totals and averages their rows.
    if (std::all_of(files.begin(), files.end(), [](const std::string& f) {
            const std::string e = extension(f);
            return e == "csv" || e == "tsv" || e == "xlsx" || e == "xlsm";
        }))
        return reply(done, files.front() + " is a table, not a document: call query_table {\"path\": \"" + files.front() +
                               "\"} to filter, count, total, average or group its rows (it lists the columns).");
    // One short text file: the whole of it, not answers to a question about it (a narrower question
    // than the user's lost what it needed: "the total budget", without what was spent).
    if (files.size() == 1 && kindOf(files.front()) == FileKind::Text) {
        bool open = false;
        std::string text = fileText(root + "/" + files.front(), open);
        if (extension(files.front()) == "ics")
            if (std::string events = calendarText(text); !events.empty())
                text = std::move(events);
        if (!text.empty() && text.size() <= kShortTextChars && !looksBinary(text))
            return reply(done, files.front() + " is short, so here it is whole; answer from it:\n\n" + text);
    }
    std::string left;
    if (files.size() > kExtractFiles) {
        left = std::to_string(files.size() - kExtractFiles) + " more documents were not read (ask again with a narrower folder or pattern).";
        files.resize(kExtractFiles);
    }
    auto job = std::make_shared<ExtractJob>();
    job->question = question;
    job->files = files;
    job->busy = vm_.agent().busy.get();
    if (!left.empty())
        job->problems.push_back(left);
    // Read the texts in the background, cut into the parts one pass reads; then the passes.
    std::weak_ptr<bool> alive = alive_;
    vm_.agent().setLocalActivity("reading " + std::to_string(files.size()) + " documents");
    os::dispatch::runAsync([this, alive, root, job, done = std::move(done)]() mutable {
        std::vector<std::vector<std::string>> parts(job->files.size());
        std::vector<std::string> problems(job->files.size());
        for (size_t i = 0; i < job->files.size(); ++i) {
            const DocumentText t = documentText(root + "/" + job->files[i], kPassChars * kPassesPerDocument);
            if (!t.error.empty()) {
                problems[i] = t.error;
                continue;
            }
            std::string part;
            for (size_t p = 0; p < t.pages.size(); ++p) {
                std::string page = t.paged ? "[page " + std::to_string(p + 1) + "]\n" + t.pages[p] + "\n" : t.pages[p];
                // A page longer than a pass is split; pages are joined while they fit.
                while (!page.empty()) {
                    const size_t room = kPassChars - std::min(kPassChars, part.size());
                    if (room < 500 && !part.empty()) {
                        parts[i].push_back(std::move(part));
                        part.clear();
                        continue;
                    }
                    const size_t take = std::min(room, page.size());
                    part += page.substr(0, take);
                    page.erase(0, take);
                }
            }
            if (!trimmed(part).empty())
                parts[i].push_back(std::move(part));
            if (parts[i].size() > kPassesPerDocument)
                parts[i].resize(kPassesPerDocument);
            if (t.paged && t.pages.size() < t.pageCount)
                problems[i] = "read its first " + std::to_string(t.pages.size()) + " of " + std::to_string(t.pageCount) + " pages";
        }
        os::dispatch::postToMain([this, alive, job, parts = std::move(parts), problems = std::move(problems), done = std::move(done)]() mutable {
            if (!alive.lock())
                return;
            job->parts = std::move(parts);
            for (size_t i = 0; i < problems.size(); ++i)
                if (!problems[i].empty())
                    job->problems.push_back(job->files[i] + ": " + problems[i]);
            extractPass(job, std::move(done));
        });
    });
}

void LocalAgentTools::extractPass(std::shared_ptr<ExtractJob> job, Done done)
{
    if (job->busy && !vm_.agent().busy.get())
        return; // the user stopped the agent, or closed the folder: no more passes
    // Skip what has nothing to read.
    while (job->file < job->files.size() && job->parts[job->file].empty()) {
        job->answers.push_back("(not read)");
        ++job->file;
    }
    if (job->file >= job->files.size()) { // all read: the table, also kept in the notes
        vm_.agent().setLocalActivity(std::string());
        std::string table = "| Document | What it says |\n|---|---|\n";
        std::vector<std::string>& n = notes();
        for (size_t i = 0; i < job->files.size(); ++i) {
            std::string a = oneLine(i < job->answers.size() ? job->answers[i] : std::string(), 500);
            std::string cell = a;
            std::replace(cell.begin(), cell.end(), '|', '/');
            table += "| " + job->files[i] + " | " + cell + " |\n";
            if (n.size() >= 200)
                n.erase(n.begin());
            n.push_back(job->files[i] + " (" + oneLine(job->question, 80) + "): " + a);
        }
        std::string out = "Asked of " + std::to_string(job->files.size()) + " documents: " + job->question + "\n\n" + table;
        for (const std::string& p : job->problems)
            out += "Note: " + p + "\n";
        out += "\nThis is also in your notes. Figures are as each document gives them: combine them with calculate.";
        return reply(done, out);
    }
    const std::vector<std::string>& parts = job->parts[job->file];
    const std::string& file = job->files[job->file];
    // The model and device of the exchange in progress; a short answer.
    const interfaces::LocalChatRequest& now = vm_.agent().localRequestNow();
    interfaces::LocalChatRequest r;
    r.modelPath = now.modelPath;
    r.drafterPath = now.drafterPath;
    r.projectorPath = now.projectorPath;
    r.threads = now.threads;
    r.useGpu = now.useGpu;
    r.compactCache = now.compactCache;
    r.contextTokens = now.contextTokens;
    r.maxTokens = 600;
    r.temperature = 0.0f; // the same document and question: the same answer
    r.messages.emplace_back("system", kExtractPrompt);
    std::string ask = "Question: " + job->question + "\nDocument: " + file;
    if (parts.size() > 1)
        ask += " (part " + std::to_string(job->part + 1) + " of " + std::to_string(parts.size()) + ")";
    if (!job->found.empty())
        ask += "\nWhat you found in its earlier parts:\n" + job->found;
    ask += "\n\n----- the document -----\n" + parts[job->part] + "\n----- end -----\n";
    ask += parts.size() > 1 ? "Answer with everything found so far, from the earlier parts and this one."
                            : "Answer the question from this document.";
    r.messages.emplace_back("user", ask);
    vm_.agent().setLocalActivity("reading " + file + " (" + std::to_string(job->file + 1) + " of " + std::to_string(job->files.size()) +
                                 (parts.size() > 1 ? ", part " + std::to_string(job->part + 1) + " of " + std::to_string(parts.size()) : "") + ")");
    std::weak_ptr<bool> alive = alive_;
    engine_->chat(
        r, [](const std::string&) {}, [](const std::string&) {},
        [this, alive, job, done = std::move(done)](const interfaces::LocalChatResult& result) mutable {
            if (!alive.lock())
                return;
            if (!result.ok) {
                job->found = "(could not be read: " + result.error + ")";
                job->part = job->parts[job->file].size(); // no more parts of it
            } else {
                job->found = trimmed(result.text);
                ++job->part;
            }
            if (job->part >= job->parts[job->file].size()) {
                job->answers.push_back(job->found);
                job->found.clear();
                job->part = 0;
                ++job->file;
            }
            extractPass(job, std::move(done));
        });
}

// ---- Scanning: windows over a long document, tiles over a picture ---------------------------------
// The model in front of one small piece at a time, with what is being looked for: a small model
// finds a fact in 6,000 characters far better than in 60,000. Windows overlap, so a sentence cut at
// a window's edge is whole in the next; a picture's tiles where something was seen are cut again,
// finer (coarse to fine), so the model gets closer without being told where to look.

namespace {

constexpr const char* kScanPrompt =
    "You look at one part of a longer document or a larger picture, for a colleague who is searching for "
    "something. Say only what is in this part. If something here is about what they search for, quote it "
    "exactly (with its [page N] when shown) or say where in this part it is and what it shows, in one or two "
    "lines. If nothing here is, answer only: nothing.";
constexpr size_t kScanPasses = 48; // passes one scan makes at most

bool nothingFound(const std::string& answer)
{
    const std::string a = lower(trimmed(answer));
    return a.empty() || a.rfind("nothing", 0) == 0 || a.rfind("not in", 0) == 0 || a.rfind("none", 0) == 0 || a == "no" ||
           a.rfind("no.", 0) == 0 || a.rfind("no,", 0) == 0;
}

} // namespace

struct LocalAgentTools::ScanJob {
    struct Pass {
        std::string file, label; // what the hit is reported as: "report.pdf, pages 3-4", "region 0-50% across ..."
        std::string text;        // a window of a document
        float box[4] = {0, 0, 1, 1}; // a tile of a picture (fractions)
        int depth = 1;
    };
    std::string find;
    std::vector<Pass> passes;
    size_t next = 0;
    std::vector<std::string> hits, notes;
    bool busy = false;
    // A picture: its pixels, for tiles cut finer where something was seen.
    std::string picture;
    std::vector<unsigned char> rgba;
    int width = 0, height = 0, levels = 1, tokens = 280;
    std::string header;
};

interfaces::LocalChatRequest LocalAgentTools::passRequest(int maxTokens) const
{
    const interfaces::LocalChatRequest& now = vm_.agent().localRequestNow();
    interfaces::LocalChatRequest r;
    r.modelPath = now.modelPath;
    r.drafterPath = now.drafterPath;
    r.projectorPath = now.projectorPath;
    r.threads = now.threads;
    r.useGpu = now.useGpu;
    r.compactCache = now.compactCache;
    r.contextTokens = now.contextTokens;
    r.maxTokens = maxTokens;
    r.temperature = 0.0f; // the same part and question: the same answer
    return r;
}

void LocalAgentTools::scanDocument(const Json& args, Done done)
{
    if (!engine_)
        return reply(done, "scan_document cannot run here: the local model is not available to it.");
    const std::string find = trimmed(args.getString("find"));
    if (find.empty())
        return reply(done, "scan_document needs what to look for (find). " + usage("scan_document"));
    std::string error;
    const std::string root = resolve("", error);
    if (root.empty())
        return reply(done, error);
    std::vector<std::string> files;
    if (!documentFiles(args, root, files, error))
        return reply(done, error);
    // The window: small, normal, large or characters; the overlap in percent of it.
    const std::string w = lower(trimmed(args.getString("window")));
    size_t window = w == "small" ? 3000 : w == "large" ? 10000 : 6000;
    if (!w.empty() && std::isdigit(static_cast<unsigned char>(w[0])))
        window = size_t(std::clamp(std::atoi(w.c_str()), 1500, 12000));
    const int overlapPercent = std::clamp(argInt(args, "overlap", 20), 0, 50);
    const size_t overlap = window * size_t(overlapPercent) / 100;
    auto job = std::make_shared<ScanJob>();
    job->find = find;
    job->busy = vm_.agent().busy.get();
    vm_.agent().setLocalActivity("reading " + std::to_string(files.size()) + " documents");
    std::weak_ptr<bool> alive = alive_;
    os::dispatch::runAsync([this, alive, root, files, window, overlap, overlapPercent, job, done = std::move(done)]() mutable {
        std::vector<ScanJob::Pass> passes;
        std::vector<std::string> notes;
        size_t windows = 0;
        for (const std::string& f : files) {
            const DocumentText t = documentText(root + "/" + f, 2u << 20);
            if (!t.error.empty()) {
                notes.push_back(f + ": " + t.error);
                continue;
            }
            // The whole text with page marks, and where each page starts in it.
            std::string all;
            std::vector<std::pair<size_t, int>> starts;
            for (size_t p = 0; p < t.pages.size(); ++p) {
                starts.emplace_back(all.size(), int(p) + 1);
                all += (t.paged ? "[page " + std::to_string(p + 1) + "]\n" : std::string()) + t.pages[p] + "\n";
            }
            if (trimmed(all).empty()) {
                notes.push_back(f + ": no text (a scan? look at its pages)");
                continue;
            }
            auto pageAt = [&](size_t at) {
                int page = 1;
                for (const auto& [offset, number] : starts)
                    if (offset <= at)
                        page = number;
                return page;
            };
            const size_t step = window - std::min(window - 500, overlap);
            for (size_t from = 0; from < all.size(); from += step) {
                if (passes.size() >= kScanPasses) {
                    notes.push_back(f + ": read up to " + (t.paged ? "page " + std::to_string(pageAt(from)) : std::to_string(from) + " characters") +
                                    " (the scan's limit: narrow it with search_documents, or a larger window)");
                    break;
                }
                ScanJob::Pass p;
                p.file = f;
                p.text = all.substr(from, window);
                const int first = pageAt(from), last = pageAt(std::min(all.size(), from + window) - 1);
                p.label = f + (t.paged ? first == last ? ", page " + std::to_string(first) : ", pages " + std::to_string(first) + "-" + std::to_string(last)
                                       : ", characters " + std::to_string(from) + "-" + std::to_string(std::min(all.size(), from + window)));
                passes.push_back(std::move(p));
                ++windows;
                if (from + window >= all.size())
                    break;
            }
        }
        os::dispatch::postToMain([this, alive, job, passes = std::move(passes), notes = std::move(notes), windows, window, overlapPercent,
                                  nfiles = files.size(), done = std::move(done)]() mutable {
            if (!alive.lock())
                return;
            job->passes = std::move(passes);
            job->notes = std::move(notes);
            job->header = "Looked for: " + job->find + "\nin " + std::to_string(nfiles) + " document(s), " + std::to_string(windows) +
                          " windows of " + std::to_string(window) + " characters overlapping by " + std::to_string(overlapPercent) + "%.";
            scanPass(job, std::move(done));
        });
    });
}

// ---- Reading closely: one file, window by window, with notes -------------------------------------
// For a long file, or one the other tools cannot make sense of: the model reads it a small part at a
// time (a few thousand characters, numbered lines, overlapping), each time with the objective, what
// the task keeps in mind and the notes so far, and writes down what is new. The notes only grow (a
// small model rewriting them all lost some); a note whose figure that part does not hold is dropped;
// long notes are tidied in a pass that must keep every figure. Then, by default, an answer from the
// notes alone.

namespace {

constexpr const char* kCloseReadPrompt =
    "You read one part of a longer file closely, for a task. From this part, write down only what is new and "
    "matters for the objective: one note per line, starting with '- ', with names, figures and dates exactly as "
    "written in the file, and where it is at the end: (line N). Do not repeat a note you already have: the lines "
    "before the mark were in the last part. If nothing in this part is new for the objective, answer only: nothing.";
constexpr const char* kCloseMergePrompt =
    "You tidy the notes of a close reading. Merge notes that say the same thing and drop repeats; keep every name, "
    "figure, date and (line N) exactly as written. Write the notes only, one per line, each starting with '- '.";
constexpr const char* kCloseAnswerPrompt =
    "You answer an objective from the notes of a close reading of a file. Use only the notes, with figures exactly "
    "as written and their (line N). If the notes do not hold all of it, say what they hold and that the rest is not "
    "in the file.";
constexpr size_t kCloseWindows = 60;       // parts one reading reads at most
constexpr size_t kCloseNotesChars = 3000;  // notes longer than this are tidied

// The figures a note gives ("1,250.50" as "1250.50"), not its line references ("(line 12)", "lines 3-9").
std::vector<std::string> noteFigures(const std::string& text)
{
    std::vector<std::string> out;
    const std::string low = lower(text);
    bool lineRef = false;
    for (size_t i = 0; i < text.size();) {
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) {
            ++i;
            continue;
        }
        size_t end = i;
        std::string figure;
        while (end < text.size() && (std::isdigit(static_cast<unsigned char>(text[end])) ||
                                     ((text[end] == ',' || text[end] == '.') && end + 1 < text.size() &&
                                      std::isdigit(static_cast<unsigned char>(text[end + 1]))))) {
            if (text[end] != ',')
                figure += text[end];
            ++end;
        }
        size_t back = i;
        while (back > 0 && low[back - 1] == ' ')
            --back;
        const bool afterLine = (back >= 4 && low.compare(back - 4, 4, "line") == 0) || (back >= 5 && low.compare(back - 5, 5, "lines") == 0);
        const bool rangeEnd = back > 0 && low[back - 1] == '-' && lineRef;
        lineRef = afterLine || rangeEnd;
        if (!lineRef)
            out.push_back(figure);
        i = end;
    }
    return out;
}

// A text's digits without thousands commas, for finding a note's figures in it.
std::string withoutDigitCommas(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == ',' && i > 0 && i + 1 < text.size() && std::isdigit(static_cast<unsigned char>(text[i - 1])) &&
            std::isdigit(static_cast<unsigned char>(text[i + 1])))
            continue;
        out += text[i];
    }
    return out;
}

// The notes in a pass's answer: its "- " lines (or "* ", "• "), each as given.
std::vector<std::string> notesIn(const std::string& answer)
{
    std::vector<std::string> out;
    size_t from = 0;
    while (from < answer.size()) {
        size_t end = answer.find('\n', from);
        if (end == std::string::npos)
            end = answer.size();
        std::string line = trimmed(answer.substr(from, end - from));
        from = end + 1;
        size_t skip = line.rfind("- ", 0) == 0 || line.rfind("* ", 0) == 0 ? 2 : line.rfind("\xe2\x80\xa2", 0) == 0 ? 3 : 0;
        if (skip == 0)
            continue;
        line = trimmed(line.substr(skip));
        if (!line.empty() && lower(line) != "nothing")
            out.push_back(line.substr(0, 400));
    }
    return out;
}

// The printable runs of a binary file (6 characters or more), one a line: text inside a program or
// an odd file.
std::string printableRuns(const std::string& bytes)
{
    std::string out, run;
    for (const char c : bytes) {
        if (c >= 32 && c < 127) {
            run += c;
            continue;
        }
        if (run.size() >= 6)
            out += run + "\n";
        run.clear();
    }
    if (run.size() >= 6)
        out += run + "\n";
    return out;
}

} // namespace

struct LocalAgentTools::CloseReadJob {
    std::string file, objective, mode, keep, kind;
    std::vector<std::string> windows; // numbered lines, the repeated ones before a mark
    std::vector<std::string> plain;   // each window's text with digits' thousands commas taken out
    std::vector<std::string> labels;  // "lines 120-185"
    size_t next = 0;
    std::vector<std::string> notes;
    size_t dropped = 0;
    size_t tidiedAt = 0; // the notes' length after the last tidy
    bool busy = false, found = false, answered = false;
    std::string answer, problem;
};

void LocalAgentTools::readClosely(const Json& args, Done done)
{
    if (!engine_)
        return reply(done, "read_closely cannot run here: the local model is not available to it.");
    const std::string objective = trimmed(args.getString("objective"));
    if (objective.empty())
        return reply(done, "read_closely needs the objective: what to find or collect. " + usage("read_closely"));
    std::string error;
    const std::string path = args.getString("path");
    const std::string absolute = resolve(path, error);
    if (absolute.empty())
        return reply(done, error);
    if (os::fs::stat(absolute).isDirectory)
        return reply(done, path + " is a folder: read_closely reads one file (list_files shows what it holds).");
    std::string mode = lower(trimmed(args.getString("mode")));
    if (mode != "collect" && mode != "find")
        mode = "answer";
    const std::string w = lower(trimmed(args.getString("window")));
    const size_t window = w == "small" ? 2500 : w == "large" ? 8000 : 4500;
    auto job = std::make_shared<CloseReadJob>();
    job->file = path;
    job->objective = objective;
    job->mode = mode;
    job->busy = vm_.agent().busy.get();
    for (const std::string& k : vm_.agent().localKeepInMind())
        job->keep += "- " + k + "\n";
    vm_.agent().setLocalActivity("reading " + path + " closely");
    std::weak_ptr<bool> alive = alive_;
    os::dispatch::runAsync([this, alive, absolute, window, job, done = std::move(done)]() mutable {
        // The text: a document's (pages marked), a text file's, or the printable runs of anything else.
        std::string all;
        DocumentText t = documentText(absolute, 4u << 20);
        if (t.error == "not text") {
            const std::optional<std::string> bytes = os::fs::readFile(absolute);
            all = bytes ? printableRuns(bytes->substr(0, 8u << 20)) : std::string();
            job->kind = "the text found in a binary file";
        } else if (!t.error.empty()) {
            job->problem = t.error;
        } else {
            job->kind = t.kind;
            for (size_t p = 0; p < t.pages.size(); ++p)
                all += (t.paged ? "[page " + std::to_string(p + 1) + "]\n" : std::string()) + t.pages[p] + "\n";
        }
        // Lines, numbered; a very long line (minified, one-line exports) in pieces of its number.
        std::vector<std::pair<size_t, std::string>> lines;
        size_t number = 0, from = 0;
        while (from < all.size()) {
            size_t end = all.find('\n', from);
            if (end == std::string::npos)
                end = all.size();
            ++number;
            std::string line = all.substr(from, end - from);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            from = end + 1;
            for (size_t at = 0; at < line.size() || at == 0; at += window / 2) {
                lines.emplace_back(number, line.substr(at, window / 2));
                if (line.size() <= window / 2)
                    break;
            }
        }
        // Windows of about `window` characters, ending at a line, the next starting about 15% back.
        size_t start = 0, repeated = 0;
        while (start < lines.size() && job->windows.size() < kCloseWindows) {
            std::string text, plain;
            size_t k = start, chars = 0;
            while (k < lines.size() && (k == start || chars + lines[k].second.size() <= window)) {
                if (k == start + repeated && repeated > 0)
                    text += "--- (the lines above were in the last part; new from here) ---\n";
                text += std::to_string(lines[k].first) + "| " + lines[k].second + "\n";
                plain += lines[k].second + "\n";
                chars += lines[k].second.size() + 1;
                ++k;
            }
            job->windows.push_back(std::move(text));
            job->plain.push_back(withoutDigitCommas(plain));
            const size_t first = lines[start].first, last = lines[k - 1].first;
            job->labels.push_back(first == last ? "line " + std::to_string(first)
                                                : "lines " + std::to_string(first) + "-" + std::to_string(last));
            if (k >= lines.size())
                break;
            // The next starts with about 15% of this one again (whole lines), and always moves on.
            size_t back = k, overlap = 0;
            while (back > start + 1 && overlap + lines[back - 1].second.size() <= window * 15 / 100) {
                overlap += lines[back - 1].second.size() + 1;
                --back;
            }
            repeated = k - back;
            start = back;
        }
        if (job->windows.size() >= kCloseWindows && start < lines.size())
            job->problem = "read up to line " + std::to_string(lines[std::min(start, lines.size() - 1)].first) + " of " +
                           std::to_string(number) + " (the limit of one reading: ask again about the rest)";
        os::dispatch::postToMain([this, alive, job, done = std::move(done)]() mutable {
            if (!alive.lock())
                return;
            if (job->windows.empty())
                return reply(done, job->file + (job->problem.empty() ? std::string(" has no text to read.") : ": " + job->problem));
            closePass(job, std::move(done));
        });
    });
}

void LocalAgentTools::closePass(std::shared_ptr<CloseReadJob> job, Done done)
{
    if (job->busy && !vm_.agent().busy.get())
        return; // the user stopped the agent, or closed the folder: no more passes
    std::string notes;
    for (const std::string& n : job->notes)
        notes += "- " + n + "\n";
    const bool read = job->next >= job->windows.size() || (job->mode == "find" && job->found);
    // All read: the notes, and an answer from them (not from the file) unless only notes were asked for.
    if (read && (job->answered || job->mode != "answer" || job->notes.empty())) {
        vm_.agent().setLocalActivity(std::string());
        std::string out = "Read " + job->file + " closely (" + job->kind + ", " + std::to_string(job->next) + " of " +
                          std::to_string(job->windows.size()) + " parts) for: " + job->objective + "\n\nNotes (" +
                          std::to_string(job->notes.size()) + "):\n" + (notes.empty() ? std::string("(nothing found)\n") : notes);
        if (!job->answer.empty())
            out += "\nFrom the notes:\n" + job->answer + "\n";
        if (job->dropped)
            out += "\n(" + std::to_string(job->dropped) + " note(s) left out: their figures are not in the part they came from.)\n";
        if (!job->problem.empty())
            out += "Note: " + job->problem + "\n";
        std::string error;
        if (const std::string absolute = resolve(job->file, error); !absolute.empty())
            out += transcriptNote(job->file, absolute);
        return reply(done, out);
    }
    interfaces::LocalChatRequest r = passRequest(700);
    const std::string keep = job->keep.empty() ? std::string() : "What the task keeps in mind:\n" + job->keep;
    enum class Pass { Read, Tidy, Answer } pass =
        read ? Pass::Answer : notes.size() > kCloseNotesChars && notes.size() > job->tidiedAt + 500 ? Pass::Tidy : Pass::Read;
    if (pass == Pass::Answer) {
        r.messages.emplace_back("system", kCloseAnswerPrompt);
        r.messages.emplace_back("user", "Objective: " + job->objective + "\n" + keep + "\nNotes from reading " + job->file + ":\n" + notes +
                                            "\nAnswer the objective from these notes.");
        vm_.agent().setLocalActivity("answering from the notes on " + job->file);
    } else if (pass == Pass::Tidy) {
        r.messages.emplace_back("system", kCloseMergePrompt);
        r.messages.emplace_back("user", "Objective: " + job->objective + "\nThe notes:\n" + notes);
        vm_.agent().setLocalActivity("tidying the notes on " + job->file);
    } else {
        r.messages.emplace_back("system", kCloseReadPrompt);
        r.messages.emplace_back("user", "Objective: " + job->objective + "\n" + keep + "Your notes so far:\n" +
                                            (notes.empty() ? std::string("(none yet)\n") : notes) + "\nFile: " + job->file + ", part " +
                                            std::to_string(job->next + 1) + " of " + std::to_string(job->windows.size()) + " (" +
                                            job->labels[job->next] + "):\n" + job->windows[job->next] +
                                            "\nWrite the new notes for the objective from this part, or: nothing." +
                                            (job->mode == "find"
                                                 ? std::string("\nThen, only when your notes (with this part's) hold everything the "
                                                               "objective asks - each thing it names - add a last line: ANSWERED. "
                                                               "Something only near it (the same name, another event) is not it.")
                                                 : std::string()));
        vm_.agent().setLocalActivity("reading " + job->file + " closely (part " + std::to_string(job->next + 1) + " of " +
                                     std::to_string(job->windows.size()) + ", " + std::to_string(job->notes.size()) + " notes)");
    }
    std::weak_ptr<bool> alive = alive_;
    engine_->chat(
        r, [](const std::string&) {}, [](const std::string&) {},
        [this, alive, job, pass, done = std::move(done)](const interfaces::LocalChatResult& result) mutable {
            if (!alive.lock())
                return;
            if (pass == Pass::Answer) {
                job->answer = result.ok ? trimmed(result.text) : std::string();
                job->answered = true;
            } else if (pass == Pass::Tidy) {
                // Kept only when every figure of the old notes is still there; else the old ones stay
                // (and are not tidied again).
                std::vector<std::string> tidy = result.ok ? notesIn(result.text) : std::vector<std::string>();
                std::string joined;
                for (const std::string& n : tidy)
                    joined += n + "\n";
                joined = withoutDigitCommas(joined);
                bool kept = !tidy.empty();
                for (const std::string& n : job->notes)
                    for (const std::string& f : noteFigures(n))
                        kept = kept && joined.find(f) != std::string::npos;
                if (kept && tidy.size() < job->notes.size())
                    job->notes = std::move(tidy);
                // Not tidied again until the notes grow (a tidy that changed nothing would repeat).
                job->tidiedAt = 0;
                for (const std::string& n : job->notes)
                    job->tidiedAt += n.size() + 3;
            } else {
                for (const std::string& n : result.ok ? notesIn(result.text) : std::vector<std::string>()) {
                    // Every figure of a note is in the part it came from (or the objective); else made up.
                    bool held = true;
                    for (const std::string& f : noteFigures(n))
                        held = held && (job->plain[job->next].find(f) != std::string::npos ||
                                        withoutDigitCommas(job->objective).find(f) != std::string::npos);
                    if (!held) {
                        ++job->dropped;
                        continue;
                    }
                    if (std::find(job->notes.begin(), job->notes.end(), n) == job->notes.end())
                        job->notes.push_back(n);
                }
                // Find: done when it says all of it is in the notes (and there are notes).
                if (result.ok && job->mode == "find" && !job->notes.empty()) {
                    std::string low = lower(result.text);
                    low.erase(std::remove(low.begin(), low.end(), '*'), low.end());
                    for (size_t at = low.find("answered"); at != std::string::npos && !job->found; at = low.find("answered", at + 1))
                        job->found = (at == 0 || low[at - 1] == '\n' || low[at - 1] == ' ') &&
                                     low.compare(at > 3 ? at - 4 : 0, 4, "not ") != 0 && low.compare(at > 2 ? at - 3 : 0, 3, "un-") != 0;
                }
                ++job->next;
            }
            closePass(job, std::move(done));
        });
}

void LocalAgentTools::scanImage(const Json& args, Done done)
{
    if (!engine_)
        return reply(done, "scan_image cannot run here: the local model is not available to it.");
    const std::string find = trimmed(args.getString("find"));
    if (find.empty())
        return reply(done, "scan_image needs what to look for (find). " + usage("scan_image"));
    std::string error;
    const std::string path = args.getString("path");
    const std::string absolute = resolve(path, error);
    if (absolute.empty())
        return reply(done, error);
    const std::optional<std::string> bytes = os::fs::readFile(absolute);
    auto job = std::make_shared<ScanJob>();
    services::DecodedImage image;
    std::string note;
    if (!bytes || !services::decodeImage(*bytes, image, note, 8192))
        return reply(done, kindOf(path) != FileKind::Picture ? path + " is not a picture. " + whichTool(path, hears())
                                                             : path + " is a picture this cannot read: " + note);
    float region[4];
    if (!lookBox(args, region, error))
        return reply(done, error + ".\n" + usage("scan_image"));
    const int grid = std::clamp(argInt(args, "grid", 2), 1, 4);
    job->find = find;
    job->busy = vm_.agent().busy.get();
    job->picture = path;
    job->rgba = std::move(image.rgba);
    job->width = image.width;
    job->height = image.height;
    job->levels = std::clamp(argInt(args, "levels", 2), 1, 3);
    job->tokens = pictureTokens(args.getString("detail"));
    // The first tiles: a grid over the region, each a little larger than its cell (they overlap).
    const float cw = (region[2] - region[0]) / float(grid), ch = (region[3] - region[1]) / float(grid);
    for (int y = 0; y < grid; ++y)
        for (int x = 0; x < grid; ++x) {
            ScanJob::Pass p;
            p.file = path;
            p.box[0] = std::max(region[0], region[0] + cw * (float(x) - 0.15f));
            p.box[1] = std::max(region[1], region[1] + ch * (float(y) - 0.15f));
            p.box[2] = std::min(region[2], region[0] + cw * (float(x) + 1.15f));
            p.box[3] = std::min(region[3], region[1] + ch * (float(y) + 1.15f));
            job->passes.push_back(p);
        }
    job->header = "Looked for: " + find + "\nin " + path + " (" + std::to_string(image.sourceWidth) + " x " + std::to_string(image.sourceHeight) +
                  "), a " + std::to_string(grid) + " x " + std::to_string(grid) + " grid of overlapping tiles, up to " +
                  std::to_string(job->levels) + " levels finer where something was seen.";
    scanPass(job, std::move(done));
}

void LocalAgentTools::scanPass(std::shared_ptr<ScanJob> job, Done done)
{
    if (job->busy && !vm_.agent().busy.get())
        return; // stopped
    if (job->next >= job->passes.size() || job->next >= kScanPasses) { // done: the findings, also in the notes
        vm_.agent().setLocalActivity(std::string());
        std::string out = job->header + "\n";
        std::vector<std::string>& n = notes();
        if (job->hits.empty()) {
            out += "Found nothing about it.";
        } else {
            out += "Found in " + std::to_string(job->hits.size()) + " place(s):\n";
            for (const std::string& h : job->hits) {
                out += "- " + h + "\n";
                if (n.size() >= 200)
                    n.erase(n.begin());
                n.push_back("Looking for " + oneLine(job->find, 60) + ": " + oneLine(h, 400));
            }
            out += job->rgba.empty() ? "Read around a hit with read_document (from_page), or look_at_page on its page."
                                     : "Look closer at a hit with look_at_image and its region, detail high.";
        }
        if (job->next < job->passes.size())
            out += "\n(" + std::to_string(job->passes.size() - job->next) + " more parts not looked at: the scan's limit.)";
        for (const std::string& note : job->notes)
            out += "\nNote: " + note;
        os::mem::release(job->rgba); // the picture's pixels can be large
        return reply(done, out);
    }
    const ScanJob::Pass pass = job->passes[job->next];
    interfaces::LocalChatRequest r = passRequest(300);
    r.messages.emplace_back("system", kScanPrompt);
    interfaces::LocalChatMessage ask("user", "Look for: " + job->find + "\n");
    std::string where;
    if (job->rgba.empty()) {
        where = pass.label;
        ask.text += "In: " + pass.label + "\n\n----- this part -----\n" + pass.text + "\n----- end -----\nQuote what is about it, or answer only: nothing.";
    } else {
        // The tile, cut from the whole picture at its own detail.
        const int left = std::clamp(int(pass.box[0] * float(job->width)), 0, job->width - 1);
        const int top = std::clamp(int(pass.box[1] * float(job->height)), 0, job->height - 1);
        const int right = std::clamp(int(std::ceil(pass.box[2] * float(job->width))), left + 1, job->width);
        const int bottom = std::clamp(int(std::ceil(pass.box[3] * float(job->height))), top + 1, job->height);
        where = "region \"" + std::to_string(int(pass.box[0] * 100 + 0.5f)) + "," + std::to_string(int(pass.box[1] * 100 + 0.5f)) + "," +
                std::to_string(int(pass.box[2] * 100 + 0.5f)) + "," + std::to_string(int(pass.box[3] * 100 + 0.5f)) + "\" (" +
                boxText(pass.box) + (pass.depth > 1 ? ", closer" : "") + ")";
        ask.media.push_back(picture(fileName(job->picture), job->rgba.data() + (size_t(top) * size_t(job->width) + size_t(left)) * 4,
                                    right - left, bottom - top, job->width * 4, job->tokens, tokenPixels()));
        ask.text += "This is one part of the picture " + job->picture + ". Say what in it is about that, and where in this part; or answer only: nothing.";
    }
    r.messages.push_back(std::move(ask));
    ++job->next;
    vm_.agent().setLocalActivity("looking for it: part " + std::to_string(job->next) + " of " + std::to_string(job->passes.size()));
    std::weak_ptr<bool> alive = alive_;
    engine_->chat(r, [](const std::string&) {}, [](const std::string&) {},
                  [this, alive, job, pass, where, done = std::move(done)](const interfaces::LocalChatResult& result) mutable {
                      if (!alive.lock())
                          return;
                      if (!result.ok) {
                          job->notes.push_back(where + ": could not be read (" + result.error + ")");
                      } else if (!nothingFound(result.text)) {
                          job->hits.push_back(where + ": " + oneLine(result.text, 400));
                          // A picture: the tile again, in four overlapping parts, one level finer.
                          if (!job->rgba.empty() && pass.depth < job->levels) {
                              const float w = pass.box[2] - pass.box[0], h = pass.box[3] - pass.box[1];
                              for (int y = 0; y < 2; ++y)
                                  for (int x = 0; x < 2; ++x) {
                                      ScanJob::Pass p = pass;
                                      p.depth = pass.depth + 1;
                                      p.box[0] = pass.box[0] + w * std::max(0.0f, 0.5f * float(x) - 0.1f);
                                      p.box[1] = pass.box[1] + h * std::max(0.0f, 0.5f * float(y) - 0.1f);
                                      p.box[2] = pass.box[0] + w * std::min(1.0f, 0.5f * float(x) + 0.6f);
                                      p.box[3] = pass.box[1] + h * std::min(1.0f, 0.5f * float(y) + 0.6f);
                                      job->passes.push_back(p);
                                  }
                          }
                      }
                      scanPass(job, std::move(done));
                  });
}

void LocalAgentTools::lookAtPage(const Json& args, Done done)
{
    view::MainView* view = view_ ? view_() : nullptr;
    if (!view)
        return reply(done, "the window is not shown, so there is nothing to look at");
    std::string path = args.getString("path");
    const model::Document* doc = vm_.activeDocument();
    if (!path.empty()) {
        std::string error;
        const std::string absolute = resolve(path, error);
        if (absolute.empty())
            return reply(done, error);
        vm_.openFile(absolute);
        doc = vm_.activeDocument();
    }
    if (!doc)
        return reply(done, "no document is open");
    path = doc->path.empty() ? doc->title : doc->path;
    Look look;
    std::string why;
    if (!lookBox(args, look.box, why))
        return reply(done, why + ".\n" + usage("look_at_page"));
    const int page = std::max(1, argInt(args, "page", 1));
    // The viewer zooms so the part fills the view (the whole page fits it), then it is captured:
    // a small part is seen with the page's own detail, not enlarged pixels.
    const bool shown = view->editor().viewerView().showPage(doc->id, page - 1, -1.0f, look.box);
    look.done = std::move(done);
    look.page = page - 1;
    look.documentId = doc->id;
    look.tokens = pictureTokens(args.getString("detail"));
    const std::string part = boxText(look.box);
    look.what = (shown ? "page " + std::to_string(page) + " of " : std::string()) + fileName(path) + (part.empty() ? "" : ", " + part);
    looks_.push_back(std::move(look));
}

void LocalAgentTools::tick()
{
    for (Look& l : looks_)
        ++l.frames;
}

bool LocalAgentTools::captureWanted() const
{
    for (const Look& l : looks_)
        if (l.frames >= kLookMaxFrames || (l.frames >= kLookMinFrames && vm_.viewer().loadingCount() == 0))
            return true;
    return false;
}

void LocalAgentTools::deliverCapture(bool ok, int width, int height, const std::vector<unsigned char>& rgba)
{
    std::vector<Look> ready;
    for (size_t i = 0; i < looks_.size();)
        if (looks_[i].frames >= kLookMinFrames) {
            ready.push_back(std::move(looks_[i]));
            looks_.erase(looks_.begin() + long(i));
        } else {
            ++i;
        }
    for (Look& l : ready) {
        if (!ok || rgba.empty()) {
            reply(l.done, "the window could not be captured");
            continue;
        }
        // The document's area, in the capture's pixels: ImGui's positions are on the screen (with
        // viewports, the window's own position is in them), the capture is the window's, and may
        // have more pixels than ImGui has units.
        const view::UiElement* area = view::UiAutomation::instance().find("document-area");
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        const ImVec2 origin = vp ? vp->Pos : ImVec2(0, 0);
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        const float sx = display.x > 0 ? float(width) / display.x : 1.0f;
        const float sy = display.y > 0 ? float(height) / display.y : 1.0f;
        auto toX = [&](float x) { return (x - origin.x) * sx; };
        auto toY = [&](float y) { return (y - origin.y) * sy; };
        float x0 = 0, y0 = 0, x1 = float(width), y1 = float(height);
        if (area) {
            x0 = toX(area->min.x), y0 = toY(area->min.y), x1 = toX(area->max.x), y1 = toY(area->max.y);
        }
        // The part of the page asked for, where the page was drawn (a paged document), within the
        // document's area; else that part of the area (Word, Excel).
        float p[4] = {x0, y0, x1, y1};
        if (view::MainView* view = view_ ? view_() : nullptr; view && l.documentId >= 0)
            if (float r[4]; view->editor().viewerView().pageOnScreen(l.documentId, l.page, r))
                p[0] = toX(r[0]), p[1] = toY(r[1]), p[2] = toX(r[2]), p[3] = toY(r[3]);
        const float* f = l.box;
        const int left = std::clamp(int(std::max(x0, p[0] + (p[2] - p[0]) * f[0])), 0, width - 1);
        const int top = std::clamp(int(std::max(y0, p[1] + (p[3] - p[1]) * f[1])), 0, height - 1);
        const int right = std::clamp(int(std::min(x1, p[0] + (p[2] - p[0]) * f[2])), left + 1, width);
        const int bottom = std::clamp(int(std::min(y1, p[1] + (p[3] - p[1]) * f[3])), top + 1, height);
        Output out;
        out.media.push_back(picture(l.what, rgba.data() + (size_t(top) * size_t(width) + size_t(left)) * 4, right - left,
                                    bottom - top, width * 4, l.tokens, tokenPixels()));
        out.text = (area ? "Looked at " + l.what + " (as shown in the viewer)."
                         : "The document is not shown in the viewer, so this is the whole window.") +
                   blankNote(out.media.back());
        if (const std::string saved = savePicture(out.media.back(), "page"); !saved.empty())
            out.shown.push_back(saved); // the user sees the part it looked at
        l.done(std::move(out));
    }
}

// A picture or recording the question does not name, picked from a folder ("a photo of a parking
// fine in my Downloads", "my landlord's voicemail"): the others of its kind beside it, for when this
// is not the one, said as the call to make (a model told only that others exist answered from the
// wrong one). `self` is the file's own name, left out; empty when the question names it or there
// are no others.
std::string LocalAgentTools::othersBeside(const std::string& path, const std::string& absolute, const std::string& self, bool picture) const
{
    if (question_.empty() || question_.find(lower(self)) != std::string::npos)
        return {};
    const size_t slash = absolute.find_last_of("/\\");
    const size_t pathSlash = path.find_last_of("/\\");
    const std::string folder = path.substr(0, pathSlash == std::string::npos ? 0 : pathSlash + 1);
    std::string others, first;
    int count = 0;
    if (slash != std::string::npos)
        for (const os::fs::DirEntry& e : os::fs::listDirectory(absolute.substr(0, slash))) {
            if (e.isDirectory || e.hidden || kindOf(e.name) != (picture ? FileKind::Picture : FileKind::Sound) || lower(e.name) == lower(self))
                continue;
            if (first.empty())
                first = folder + e.name;
            if (++count <= 8)
                others += (others.empty() ? "" : ", ") + folder + e.name;
        }
    if (!count)
        return {};

    const std::string tool = picture ? "look_at_image" : "listen_to";
    return std::string("\nOther ") + (picture ? "pictures" : "recordings") + " beside it: " + others + (count > 8 ? ", ..." : "") +
           ". If this " + (picture ? "picture" : "recording") + " is not the one the question is about, do not answer from it: call " +
           tool + " {\"path\": \"" + first + "\"} next" + (count > 1 ? ", then the others" : "") + ".";
}

void LocalAgentTools::lookAtImage(const Json& args, Done done)
{
    std::string error;
    const std::string path = args.getString("path");
    const std::string absolute = resolve(path, error);
    if (absolute.empty())
        return reply(done, error);
    const std::optional<std::string> bytes = os::fs::readFile(absolute);
    if (!bytes)
        return reply(done, "cannot read " + path);
    float box[4];
    std::string why;
    if (!lookBox(args, box, why))
        return reply(done, why + ".\n" + usage("look_at_image"));
    services::DecodedImage image;
    std::string note;
    // Whole (up to 8192 across): a part zoomed into keeps the picture's own detail.
    if (!services::decodeImage(*bytes, image, note, 8192))
        return reply(done, kindOf(path) != FileKind::Picture ? path + " is not a picture. " + whichTool(path, hears())
                                                             : path + " is a picture this cannot read: " + note);
    const int left = std::clamp(int(box[0] * float(image.width)), 0, image.width - 1);
    const int top = std::clamp(int(box[1] * float(image.height)), 0, image.height - 1);
    const int right = std::clamp(int(std::ceil(box[2] * float(image.width))), left + 1, image.width);
    const int bottom = std::clamp(int(std::ceil(box[3] * float(image.height))), top + 1, image.height);
    const int tokens = pictureTokens(args.getString("detail"));
    const std::string part = boxText(box);
    Output out;
    out.media.push_back(picture(fileName(path) + (part.empty() ? "" : ", " + part),
                                image.rgba.data() + (size_t(top) * size_t(image.width) + size_t(left)) * 4, right - left,
                                bottom - top, image.width * 4, tokens, tokenPixels()));
    out.text = "Looked at " + path + (part.empty() ? "" : ", " + part) + " (" + image.format + ", " +
               std::to_string(image.sourceWidth) + " x " + std::to_string(image.sourceHeight) + " pixels; about " +
               std::to_string(tokens) + " tokens)." + blankNote(out.media.back());
    // A picture the question does not name, picked from a folder ("a photo of a parking fine in my
    // Downloads"): the other pictures beside it are said, for when this is not the one (a model that
    // found a receipt there said so and stopped).
    if (part.empty())
        out.text += othersBeside(path, absolute, fileName(path), true);
    // The user sees what it looked at: the picture, or the part it zoomed into.
    if (part.empty())
        out.shown.push_back(absolute);
    else if (const std::string saved = savePicture(out.media.back(), "zoom"); !saved.empty())
        out.shown.push_back(saved);
    done(std::move(out));
}

void LocalAgentTools::listenTo(const Json& args, Done done)
{
    std::string error;
    const std::string path = args.getString("path");
    const std::string absolute = resolve(path, error);
    if (absolute.empty())
        return reply(done, error);
    const std::string ext = extension(path);
    if (ext != "wav" && ext != "mp3" && ext != "flac")
        return reply(done, kindOf(path) != FileKind::Sound ? path + " is not a recording. " + whichTool(path, hears())
                                                           : path + " is a recording this cannot play: WAV, MP3 and FLAC can be heard.");
    const os::fs::FileInfo st = os::fs::stat(absolute);
    if (!st.exists)
        return reply(done, path + " does not exist");
    if (st.size > kSoundBytes)
        return reply(done, path + " is too long to listen to whole (" + std::to_string(st.size >> 20) + " MB; up to 25 MB).");
    std::optional<std::string> bytes = os::fs::readFile(absolute);
    if (!bytes)
        return reply(done, "cannot read " + path);
    const std::string others = othersBeside(path, absolute, fileName(path), false);
    // Ears of its own: the sound goes to the model. A speech model as its ears: what it writes down.
    if (!nativeEars()) {
        std::weak_ptr<bool> alive = alive_;
        return writeDown(path, std::move(*bytes), [alive, path, others, done = std::move(done)](const interfaces::LocalChatResult& r) {
            if (!alive.lock())
                return;
            if (!r.ok)
                return reply(done, "Listening to " + path + " failed: " + r.error);
            reply(done, "What is said in " + path + " (written down word for word by your ears, a speech model: speech "
                        "only, not other sounds or music):\n" + (r.text.empty() ? std::string("[no speech]") : r.text) + others);
        });
    }
    Output out;
    LocalMedia sound;
    sound.kind = LocalMedia::Kind::Audio;
    sound.name = fileName(path);
    sound.data = std::move(*bytes);
    out.media.push_back(std::move(sound));
    out.text = "Listened to " + path + "." + others;
    done(std::move(out));
}

void LocalAgentTools::writeDown(const std::string& path, std::string bytes, std::function<void(const interfaces::LocalChatResult&)> done)
{
    if (!engine_) {
        interfaces::LocalChatResult none;
        none.error = "this build has no local model";
        return done(none);
    }
    const viewmodel::AgentViewModel::LocalSetup setup = vm_.localSetup();
    interfaces::LocalChatRequest request;
    if (nativeEars()) {
        request.modelPath = setup.modelPath;
        request.drafterPath = setup.drafterPath;
        request.projectorPath = setup.projectorPath;
        request.useGpu = setup.gpu;
        request.compactCache = setup.compactCache;
        request.contextTokens = 0; // as the agent's chats: the model stays loaded
    } else {
        // Its speech model, on the CPU beside it: the model on the GPU stays loaded.
        request.modelPath = setup.earsPath;
        request.projectorPath = setup.earsProjectorPath;
        request.cpuOnly = true;
        request.contextTokens = 4096; // a window of speech and its words
    }
    request.threads = setup.threads;
    request.maxTokens = 8192;
    request.transcribe = true;
    interfaces::LocalChatMessage message;
    message.role = "user";
    message.text.clear(); // the engine's own instruction: the words as spoken
    LocalMedia sound;
    sound.kind = LocalMedia::Kind::Audio;
    sound.name = fileName(path);
    sound.data = std::move(bytes);
    message.media.push_back(std::move(sound));
    request.messages.push_back(std::move(message));
    std::weak_ptr<bool> alive = alive_;
    viewmodel::NotepadViewModel* vm = &vm_;
    engine_->chat(
        request,
        [alive, vm](const std::string& stage) {
            if (alive.lock())
                vm->statusText.set(stage);
        },
        [](const std::string&) {}, std::move(done));
}

void LocalAgentTools::openFile(const Json& args, Done done)
{
    std::string error;
    const std::string path = args.getString("path");
    const std::string absolute = resolve(path, error);
    if (absolute.empty())
        return reply(done, error);
    vm_.openFile(absolute);
    if (const int line = argInt(args, "line", 0); line > 0)
        handleAutomation(vm_, "open_file", Json::object().set("path", path).set("line", line));
    reply(done, "Opened " + path + " for the user.");
}

void LocalAgentTools::editFile(const Json& args, Done done)
{
    if (vm_.agent().autonomyIndex.get() != 0)
        return reply(done, "the user chose Plan only: describe the change instead");
    std::string error;
    const std::string path = args.getString("path");
    const std::string absolute = resolve(path, error);
    if (absolute.empty())
        return reply(done, error);
    bool open = false;
    const std::string before = fileText(absolute, open);
    const bool crlf = before.find("\r\n") != std::string::npos;
    const std::string oldText = asFileText(args.getString("old_text"), crlf);
    const std::string newText = asFileText(args.getString("new_text"), crlf);
    if (oldText.empty())
        return reply(done, "old_text is empty: give the exact text to replace (or use write_file for a whole file)");
    size_t at = before.find(oldText), length = oldText.size();
    if (at != std::string::npos && before.find(oldText, at + 1) != std::string::npos)
        return reply(done, "old_text is in " + path + " more than once: include a line or two around it");
    if (at == std::string::npos) {
        // Not as given: the same lines, ignoring spaces at their ends.
        size_t begin = 0, end = 0;
        if (!findLines(before, oldText, begin, end))
            return reply(done, "old_text is not in " + path + " (or is there more than once). Call read_file {\"path\": " +
                                   Json(path).dump() + "} again and copy the lines exactly, without the line numbers.");
        at = begin;
        length = end - begin;
    }
    std::string after = before;
    after.replace(at, length, newText);
    vm_.agent().proposeChange(vm_.workspaceRelative(absolute), before, after);
    reply(done, "Proposed the change to " + path + "; the user reviews it in the agent panel.");
}

void LocalAgentTools::writeFile(const Json& args, Done done)
{
    if (vm_.agent().autonomyIndex.get() != 0)
        return reply(done, "the user chose Plan only: describe the file instead");
    std::string error;
    const std::string path = args.getString("path");
    const std::string absolute = resolve(path, error);
    if (absolute.empty())
        return reply(done, error);
    bool open = false;
    const std::string before = fileText(absolute, open);
    vm_.agent().proposeChange(vm_.workspaceRelative(absolute), before,
                              asFileText(args.getString("content"), before.find("\r\n") != std::string::npos));
    reply(done, std::string(before.empty() ? "Proposed the new file " : "Proposed new contents for ") + path +
                    "; the user reviews it in the agent panel.");
}

void LocalAgentTools::readSkill(const Json& args, Done done)
{
    std::string name = lower(args.getString("name"));
    name.erase(std::remove_if(name.begin(), name.end(), [](char c) { return !(std::isalnum((unsigned char)c) || c == '-'); }),
               name.end());
    const std::optional<std::string> skill = name.empty() ? std::nullopt : os::fs::readAsset("skills/" + name + "/SKILL.md");
    reply(done, skill ? *skill : "There is no skill called '" + args.getString("name") + "'; the guide lists them.");
}

// ---- Memory ---------------------------------------------------------------------------------------

namespace {

std::string dateText(std::time_t t)
{
    char buf[16] = {};
    if (const std::tm* tm = std::localtime(&t))
        std::strftime(buf, sizeof(buf), "%Y-%m-%d", tm);
    return buf;
}

std::string today() { return dateText(std::time(nullptr)); }

const char* const kMemoryKinds[] = {"preference", "lesson", "fact", "note", "temporary"};

std::string memoryKind(const std::string& kind)
{
    const std::string k = lower(kind);
    for (const char* known : kMemoryKinds)
        if (k == known || k == std::string(known) + "s")
            return known;
    if (k == "temp" || k == "session")
        return "temporary";
    if (k == "mistake" || k == "correction")
        return "lesson";
    return k.empty() ? "note" : std::string();
}

const int kNoteDays = 7; // a note is kept this long

} // namespace

Json LocalAgentTools::loadMemories() const
{
    Json kept = Json::array();
    const std::optional<std::string> text = memoryPath_.empty() ? std::nullopt : os::fs::readFile(memoryPath_);
    if (!text)
        return kept;
    const std::optional<Json> all = Json::parse(*text);
    if (!all || !all->isArray())
        return kept;
    const std::string now = today();
    for (const Json& m : all->items()) {
        const std::string expires = m.getString("expires");
        if (!expires.empty() && expires < now)
            continue; // a note past its days
        if (m.getString("kind") == "temporary" && m.getString("session") != session_)
            continue; // another run's
        kept.push(m);
    }
    return kept;
}

bool LocalAgentTools::saveMemories(const Json& list) const
{
    if (memoryPath_.empty())
        return false;
    os::fs::createDirectories(viewmodel::FileDialogViewModel::parent(memoryPath_));
    std::string out = "[\n"; // one memory a line: the user can read and edit the file too
    const auto& items = list.items();
    for (size_t i = 0; i < items.size(); ++i)
        out += "  " + items[i].dump() + (i + 1 < items.size() ? ",\n" : "\n");
    out += "]\n";
    return os::fs::writeFile(memoryPath_, out);
}

std::string LocalAgentTools::memorySummary() const
{
    const Json list = loadMemories();
    if (list.items().empty())
        return "You remember nothing yet. When the user tells you a preference, corrects you, or you learn something "
               "that will matter again, remember it.\n";
    std::string out = "What you remember (keep it right with remember, amend_memory and forget_memory):\n";
    size_t shown = 0;
    for (const char* kind : kMemoryKinds)
        for (const Json& m : list.items()) {
            if (m.getString("kind") != kind)
                continue;
            const std::string line = "- #" + std::to_string(int(m.getNumber("id"))) + " [" + kind + "] " + m.getString("text") + "\n";
            if (out.size() + line.size() > 3000)
                continue;
            out += line;
            ++shown;
        }
    if (shown < list.items().size())
        out += "(" + std::to_string(list.items().size() - shown) +
               " more: recall them. The list is long: compact it with rewrite_memories when you can.)\n";
    return out;
}

void LocalAgentTools::remember(const Json& args, Done done)
{
    const std::string kind = memoryKind(args.getString("kind"));
    std::string text = args.getString("text");
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.pop_back();
    if (kind.empty())
        return reply(done, "Unknown kind '" + args.getString("kind") + "': use preference, lesson, fact, note or temporary.");
    if (text.empty())
        return reply(done, "Nothing to remember: give the text. " + usage("remember"));
    Json list = loadMemories();
    int next = 1;
    for (const Json& m : list.items()) {
        next = std::max(next, int(m.getNumber("id")) + 1);
        if (lower(m.getString("text")) == lower(text) && m.getString("kind") == kind)
            return reply(done, "Already remembered as #" + std::to_string(int(m.getNumber("id"))) + ".");
    }
    Json m = Json::object().set("id", next).set("kind", kind).set("text", text).set("date", today());
    if (kind == "note")
        m.set("expires", dateText(std::time(nullptr) + kNoteDays * 24 * 3600));
    if (kind == "temporary")
        m.set("session", session_);
    list.push(std::move(m));
    if (!saveMemories(list))
        return reply(done, "Could not save the memory (" + memoryPath_ + ").");
    reply(done, "Remembered as #" + std::to_string(next) + " (" + kind + ").");
}

void LocalAgentTools::recall(const Json& args, Done done)
{
    const std::string query = lower(args.getString("query"));
    const std::string kind = args.getString("kind").empty() ? std::string() : memoryKind(args.getString("kind"));
    std::string out;
    int count = 0;
    const Json memories = loadMemories(); // kept: items() points into it
    for (const Json& m : memories.items()) {
        if (!kind.empty() && m.getString("kind") != kind)
            continue;
        if (!query.empty() && lower(m.getString("text")).find(query) == std::string::npos)
            continue;
        out += "#" + std::to_string(int(m.getNumber("id"))) + " [" + m.getString("kind") + ", " + m.getString("date") +
               (m.getString("expires").empty() ? "" : ", until " + m.getString("expires")) + "] " + m.getString("text") + "\n";
        ++count;
    }
    reply(done, count == 0 ? std::string("No memories") + (query.empty() ? "." : " containing '" + query + "'.")
                           : std::to_string(count) + " memories:\n" + out);
}

void LocalAgentTools::amendMemory(const Json& args, Done done)
{
    const int id = argInt(args, "id", -1);
    Json out = Json::array();
    bool found = false;
    const Json memories = loadMemories(); // kept: items() points into it
    for (const Json& m : memories.items()) {
        Json copy = m;
        if (int(m.getNumber("id")) == id) {
            copy.set("text", args.getString("text")).set("date", today());
            found = true;
        }
        out.push(std::move(copy));
    }
    if (!found)
        return reply(done, "There is no memory #" + std::to_string(id) + ": recall to see their ids.");
    saveMemories(out);
    reply(done, "Memory #" + std::to_string(id) + " now says: " + args.getString("text"));
}

void LocalAgentTools::forgetMemory(const Json& args, Done done)
{
    const int id = argInt(args, "id", -1);
    Json out = Json::array();
    bool found = false;
    const Json memories = loadMemories(); // kept: items() points into it
    for (const Json& m : memories.items()) {
        if (int(m.getNumber("id")) == id)
            found = true;
        else
            out.push(m);
    }
    if (!found)
        return reply(done, "There is no memory #" + std::to_string(id) + ": recall to see their ids.");
    saveMemories(out);
    reply(done, "Forgot memory #" + std::to_string(id) + ".");
}

void LocalAgentTools::rewriteMemories(const Json& args, Done done)
{
    const std::string kind = memoryKind(args.getString("kind"));
    if (kind.empty() || kind == "temporary")
        return reply(done, "Give a kind to rewrite: preference, lesson, fact or note.");
    Json out = Json::array();
    int next = 1, before = 0;
    const Json memories = loadMemories(); // kept: items() points into it
    for (const Json& m : memories.items()) {
        next = std::max(next, int(m.getNumber("id")) + 1);
        if (m.getString("kind") == kind)
            ++before;
        else
            out.push(m);
    }
    int after = 0;
    const std::string lines = args.getString("lines");
    for (size_t from = 0; from <= lines.size();) {
        size_t nl = lines.find('\n', from);
        if (nl == std::string::npos)
            nl = lines.size();
        std::string line = lines.substr(from, nl - from);
        from = nl + 1;
        while (!line.empty() && (line.front() == '-' || line.front() == '*' || std::isspace(static_cast<unsigned char>(line.front()))))
            line.erase(line.begin());
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
            line.pop_back();
        if (line.empty())
            continue;
        Json m = Json::object().set("id", next++).set("kind", kind).set("text", line).set("date", today());
        if (kind == "note")
            m.set("expires", dateText(std::time(nullptr) + kNoteDays * 24 * 3600));
        out.push(std::move(m));
        ++after;
    }
    saveMemories(out);
    reply(done, "Rewrote the " + kind + " memories: " + std::to_string(before) + " became " + std::to_string(after) + ".");
}

// ---- Calculator -----------------------------------------------------------------------------------

namespace {

struct Calculator {
    std::string_view s;
    size_t i = 0;
    std::string error;

    void space()
    {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
            ++i;
    }
    bool eat(char c)
    {
        space();
        if (i < s.size() && s[i] == c) {
            ++i;
            return true;
        }
        return false;
    }
    double fail(const std::string& why)
    {
        if (error.empty())
            error = why;
        return 0.0;
    }
    double sum()
    {
        double v = product();
        for (;;) {
            if (eat('+'))
                v += product();
            else if (eat('-'))
                v -= product();
            else
                return v;
        }
    }
    double product()
    {
        double v = power();
        for (;;) {
            if (eat('*')) {
                v *= power();
            } else if (eat('/')) {
                const double d = power();
                if (d == 0)
                    return fail("division by zero");
                v /= d;
            } else if (eat('%')) {
                const double d = power();
                if (d == 0)
                    return fail("modulo by zero");
                v = std::fmod(v, d);
            } else if (word("of")) { // "15% of 2340"
                v *= power();
            } else {
                return v;
            }
        }
    }
    double power()
    {
        const double base = unary();
        if (eat('^'))
            return std::pow(base, power()); // right to left
        return base;
    }
    double unary()
    {
        if (eat('-'))
            return -unary();
        if (eat('+'))
            return unary();
        return primary();
    }
    bool word(const char* w)
    {
        space();
        const size_t n = std::strlen(w);
        if (s.compare(i, n, w) != 0 || (i + n < s.size() && std::isalnum(static_cast<unsigned char>(s[i + n]))))
            return false;
        i += n;
        return true;
    }
    // A number, as a percentage when '%' follows it and no number does ("15%", "15% of 200"; "7 % 3"
    // stays the remainder).
    double number()
    {
        const double v = digits();
        size_t k = i;
        while (k < s.size() && std::isspace(static_cast<unsigned char>(s[k])))
            ++k;
        if (k < s.size() && s[k] == '%') {
            size_t after = k + 1;
            while (after < s.size() && std::isspace(static_cast<unsigned char>(s[after])))
                ++after;
            const bool operand = after < s.size() && (std::isdigit(static_cast<unsigned char>(s[after])) || s[after] == '(' || s[after] == '.') &&
                                 s.compare(after, 2, "of") != 0;
            if (!operand) {
                i = k + 1;
                return v / 100.0;
            }
        }
        return v;
    }
    double digits()
    {
        std::string text;
        while (i < s.size()) {
            const char c = s[i];
            if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
                text += c;
            } else if (c == '_') {
            } else if ((c == 'e' || c == 'E') && i + 1 < s.size() &&
                       (std::isdigit(static_cast<unsigned char>(s[i + 1])) || s[i + 1] == '-' || s[i + 1] == '+')) {
                text += c;
                text += s[++i];
            } else {
                break;
            }
            ++i;
        }
        return std::strtod(text.c_str(), nullptr);
    }
    double call(const std::string& name, const std::vector<double>& a)
    {
        auto one = [&]() { return a.size() == 1 ? a[0] : fail(name + " takes one number"); };
        if (name == "sqrt")
            return one() < 0 ? fail("sqrt of a negative number") : std::sqrt(a[0]);
        if (name == "abs")
            return std::fabs(one());
        if (name == "floor")
            return std::floor(one());
        if (name == "ceil")
            return std::ceil(one());
        if (name == "exp")
            return std::exp(one());
        if (name == "ln")
            return std::log(one());
        if (name == "log")
            return a.size() == 2 ? std::log(a[0]) / std::log(a[1]) : std::log10(one());
        if (name == "sin")
            return std::sin(one());
        if (name == "cos")
            return std::cos(one());
        if (name == "tan")
            return std::tan(one());
        if (name == "pow")
            return a.size() == 2 ? std::pow(a[0], a[1]) : fail("pow takes two numbers");
        if (name == "round") {
            if (a.empty())
                return fail("round takes a number");
            const double f = std::pow(10.0, a.size() > 1 ? a[1] : 0.0);
            return std::round(a[0] * f) / f;
        }
        if (a.empty())
            return fail(name + " needs numbers");
        if (name == "min")
            return *std::min_element(a.begin(), a.end());
        if (name == "max")
            return *std::max_element(a.begin(), a.end());
        double total = 0;
        for (double v : a)
            total += v;
        if (name == "sum")
            return total;
        if (name == "avg" || name == "mean" || name == "average")
            return total / double(a.size());
        return fail("unknown function '" + name + "'");
    }
    double primary()
    {
        space();
        if (eat('(')) {
            const double v = sum();
            if (!eat(')'))
                return fail("a ')' is missing");
            return v;
        }
        if (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.'))
            return number();
        if (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) {
            std::string name;
            while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_'))
                name += char(std::tolower(static_cast<unsigned char>(s[i++])));
            if (name == "pi")
                return 3.14159265358979323846;
            if (name == "e")
                return 2.71828182845904523536;
            if (!eat('('))
                return fail("unknown name '" + name + "'");
            std::vector<double> a;
            if (!eat(')')) {
                do
                    a.push_back(sum());
                while (eat(','));
                if (!eat(')'))
                    return fail("a ')' is missing after " + name + "(...");
            }
            return call(name, a);
        }
        return fail(i < s.size() ? std::string("unexpected '") + s[i] + "'" : "the expression ends too soon");
    }
};

std::string numberText(double v)
{
    if (std::isfinite(v) && std::fabs(v) < 1e15 && v == std::floor(v))
        return std::to_string(static_cast<long long>(v));
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.10g", v);
    return buf;
}

// "1,234,567.5" -> "1234567.5", and currency signs dropped: the model copies numbers as written.
std::string plainNumbers(const std::string& text)
{
    std::string out;
    for (size_t k = 0; k < text.size(); ++k) {
        const char c = text[k];
        const bool digitBefore = !out.empty() && std::isdigit(static_cast<unsigned char>(out.back()));
        const bool threeAfter = k + 3 < text.size() + 1 && k + 3 <= text.size() && std::isdigit(static_cast<unsigned char>(text[k + 1])) &&
                                std::isdigit(static_cast<unsigned char>(text[k + 2])) && std::isdigit(static_cast<unsigned char>(text[k + 3])) &&
                                (k + 4 >= text.size() || !std::isdigit(static_cast<unsigned char>(text[k + 4])));
        if (c == ',' && digitBefore && threeAfter && k + 3 < text.size())
            continue; // a thousands separator, not between a function's arguments
        if (c == '$')
            continue;
        out += c;
    }
    return out;
}

} // namespace

// Several sums in one call, separated by ';' or new lines (or a list in "expressions"); each may be
// named ("total = 86.40 * 1.18") and later ones use the names ("each = total / 3").
namespace {

// Days since 1970-01-01 of a calendar date (proleptic Gregorian).
long long civilDays(int y, int m, int d)
{
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = unsigned(y - era * 400);
    const unsigned doy = unsigned((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long long>(doe) - 719468;
}

void civilFromDays(long long days, int& year, int& month, int& day)
{
    days += 719468;
    const long long era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = unsigned(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    day = int(doy - (153 * mp + 2) / 5 + 1);
    month = int(mp < 10 ? mp + 3 : mp - 9);
    year = int(static_cast<long long>(yoe) + era * 400 + (month <= 2));
}

std::string civilDate(long long days)
{
    int y = 0, m = 0, d = 0;
    civilFromDays(days, y, m, d);
    static const char* const names[] = {"January", "February", "March", "April", "May", "June", "July", "August",
                                        "September", "October", "November", "December"};
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%d %s %d", d, names[m - 1], y);
    return buf;
}

// The dates in an expression as day numbers, so that "14 July 2026 - 29 May 2026" is 46 (a small
// model counted the days by hand and got 44), and "2026-03-26 + 14" a date. Written as 2026-07-14,
// 14 July 2026, 14th July, July 14, 2026 or 26/03/2026 (day first, as a 13 or more says, else as
// written in most of the world). A date without its year takes the year of another in it.
std::string datesAsDays(const std::string& expression, int& found)
{
    static const char* const months[] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
    static const char* const full[] = {"january", "february", "march", "april", "may", "june", "july", "august",
                                       "september", "october", "november", "december"};
    std::string low = expression;
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    struct Hit {
        size_t at = 0, length = 0;
        int y = 0, m = 0, d = 0;
    };
    std::vector<Hit> hits;
    auto digits = [&](size_t i, size_t& end) {
        end = i;
        while (end < low.size() && std::isdigit(static_cast<unsigned char>(low[end])))
            ++end;
        return end > i ? std::atoi(low.substr(i, end - i).c_str()) : -1;
    };
    auto month = [&](size_t i, size_t& end) {
        for (int k = 0; k < 12; ++k) {
            const size_t n = low.compare(i, std::strlen(full[k]), full[k]) == 0 ? std::strlen(full[k])
                             : low.compare(i, 3, months[k]) == 0                 ? 3
                                                                                 : 0;
            if (n == 0)
                continue;
            end = i + n;
            if (n == 3 && end < low.size() && low[end] == '.')
                ++end;
            if (end < low.size() && std::isalpha(static_cast<unsigned char>(low[end])))
                return 0; // "march" matched as "mar": the longer was tried first; "mayor" is no month
            return k + 1;
        }
        return 0;
    };
    auto spaces = [&](size_t i) {
        while (i < low.size() && (low[i] == ' ' || low[i] == ','))
            ++i;
        return i;
    };
    auto ordinal = [&](size_t i) {
        for (const char* s : {"st", "nd", "rd", "th"})
            if (low.compare(i, 2, s) == 0 && !(i + 2 < low.size() && std::isalpha(static_cast<unsigned char>(low[i + 2]))))
                return i + 2;
        return i;
    };
    auto year = [&](size_t i, size_t& end) {
        const size_t s = spaces(i);
        size_t e = s;
        const int y = digits(s, e);
        if (y >= 1900 && y <= 2200 && e - s == 4) {
            end = e;
            return y;
        }
        end = i;
        return 0;
    };
    for (size_t i = 0; i < low.size();) {
        const bool wordStart = i == 0 || !std::isalnum(static_cast<unsigned char>(low[i - 1]));
        if (!wordStart) {
            ++i;
            continue;
        }
        size_t e = i;
        if (std::isdigit(static_cast<unsigned char>(low[i]))) {
            const int a = digits(i, e);
            // 2026-07-14
            if (e - i == 4 && e < low.size() && (low[e] == '-' || low[e] == '/')) {
                size_t e2 = e + 1, e3 = 0;
                const int m = digits(e + 1, e2);
                if (m >= 1 && m <= 12 && e2 < low.size() && low[e2] == low[e]) {
                    const int d = digits(e2 + 1, e3);
                    if (d >= 1 && d <= 31) {
                        hits.push_back({i, e3 - i, a, m, d});
                        i = e3;
                        continue;
                    }
                }
            }
            // 26/03/2026, 26.03.2026
            if (e - i <= 2 && e < low.size() && (low[e] == '/' || low[e] == '.')) {
                size_t e2 = e + 1, e3 = 0;
                const int b = digits(e + 1, e2);
                if (b >= 1 && e2 - (e + 1) <= 2 && e2 < low.size() && low[e2] == low[e]) {
                    const int y = digits(e2 + 1, e3);
                    if (e3 - (e2 + 1) == 4 && y >= 1900) {
                        const bool monthFirst = b > 12 && a <= 12;
                        const int d = monthFirst ? b : a, m = monthFirst ? a : b;
                        if (m >= 1 && m <= 12 && d >= 1 && d <= 31) {
                            hits.push_back({i, e3 - i, y, m, d});
                            i = e3;
                            continue;
                        }
                    }
                }
            }
            // 14 July 2026, 14th of July
            if (a >= 1 && a <= 31 && e - i <= 2) {
                size_t s = spaces(ordinal(e));
                if (low.compare(s, 3, "of ") == 0)
                    s += 3;
                size_t me = 0;
                if (s > e || ordinal(e) > e)
                    if (const int m = month(s, me)) {
                        size_t ye = me;
                        const int y = year(me, ye);
                        hits.push_back({i, ye - i, y, m, a});
                        i = ye;
                        continue;
                    }
            }
            i = e;
            continue;
        }
        // July 14, 2026
        if (std::isalpha(static_cast<unsigned char>(low[i]))) {
            size_t me = 0;
            if (const int m = month(i, me)) {
                const size_t s = spaces(me);
                size_t de = s;
                const int d = s > me ? digits(s, de) : -1;
                if (d >= 1 && d <= 31 && de - s <= 2) {
                    size_t ye = ordinal(de);
                    const int y = year(ye, ye);
                    hits.push_back({i, ye - i, y, m, d});
                    i = ye;
                    continue;
                }
            }
            while (i < low.size() && std::isalpha(static_cast<unsigned char>(low[i])))
                ++i;
            continue;
        }
        ++i;
    }
    found = int(hits.size());
    if (hits.empty())
        return expression;
    int known = 0;
    for (const Hit& h : hits)
        known = known ? known : h.y;
    if (!known) {
        const std::time_t now = std::time(nullptr);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &now);
#else
        localtime_r(&now, &tm);
#endif
        known = tm.tm_year + 1900;
    }
    std::string out = expression;
    for (auto h = hits.rbegin(); h != hits.rend(); ++h)
        out.replace(h->at, h->length, "(" + std::to_string(civilDays(h->y ? h->y : known, h->m, h->d)) + ")");
    return out;
}

// A cell that is one date and nothing else ("2026-03-04", "4 March 2026", "04/03/2026").
bool cellDate(const std::string& text, int& year, int& month, int& day)
{
    int found = 0;
    const std::string days = datesAsDays(text, found);
    if (found != 1 || days.size() < 3 || days.front() != '(' || days.back() != ')')
        return false;
    for (size_t k = 1; k + 1 < days.size(); ++k)
        if (!std::isdigit(static_cast<unsigned char>(days[k])) && !(k == 1 && days[k] == '-'))
            return false;
    civilFromDays(std::atoll(days.c_str() + 1), year, month, day);
    return true;
}

// A month asked for, with its year or without: "March", "Mar 2026", "March 2026", "2026-03",
// "03/2026". A small model filtered a date column with "Date contains March", then day by day.
bool monthAsked(const std::string& text, int& year, int& month)
{
    static const char* const names[] = {"january", "february", "march", "april", "may", "june", "july", "august",
                                        "september", "october", "november", "december"};
    std::string t;
    for (char c : text)
        if (c != '\'' && c != '"')
            t += char(std::tolower(static_cast<unsigned char>(c)));
    while (!t.empty() && t.back() == ' ')
        t.pop_back();
    while (!t.empty() && t.front() == ' ')
        t.erase(t.begin());
    year = 0;
    month = 0;
    // 2026-03, 2026/03, 03/2026, 3-2026
    if (t.size() >= 6 && t.size() <= 7 && std::count_if(t.begin(), t.end(), [](char c) { return c == '-' || c == '/'; }) == 1) {
        const size_t sep = t.find_first_of("-/");
        const std::string a = t.substr(0, sep), b = t.substr(sep + 1);
        auto allDigits = [](const std::string& s) { return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }); };
        if (allDigits(a) && allDigits(b)) {
            const bool yearFirst = a.size() == 4;
            year = std::atoi((yearFirst ? a : b).c_str());
            month = std::atoi((yearFirst ? b : a).c_str());
            return (yearFirst ? b : a).size() <= 2 && month >= 1 && month <= 12 && year >= 1900;
        }
    }
    // March, Mar, March 2026, Mar. 2026
    std::string word = t.substr(0, t.find(' '));
    if (!word.empty() && word.back() == '.')
        word.pop_back();
    for (int k = 0; k < 12; ++k) {
        const std::string name = names[k];
        if (word != name && !(word.size() == 3 && name.rfind(word, 0) == 0) && !(word == "sept" && k == 8))
            continue;
        const std::string rest = t.find(' ') == std::string::npos ? std::string() : t.substr(t.find(' ') + 1);
        if (!rest.empty()) {
            if (rest.size() != 4 || !std::all_of(rest.begin(), rest.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }))
                return false;
            year = std::atoi(rest.c_str());
        }
        month = k + 1;
        return true;
    }
    return false;
}

} // namespace

void LocalAgentTools::calculate(const Json& args, Done done)
{
    std::vector<std::string> parts;
    auto split = [&parts](const std::string& text) {
        std::string part;
        for (char c : text + ";") {
            if (c == ';' || c == '\n') {
                if (!trimmed(part).empty())
                    parts.push_back(trimmed(part));
                part.clear();
            } else {
                part += c;
            }
        }
    };
    split(args.getString("expression"));
    if (const Json* list = args.find("expressions"); list && list->isArray())
        for (const Json& e : list->items())
            split(e.isString() ? e.asString() : e.dump());
    else
        split(args.getString("expressions"));
    // An expression written without quotes reaches the tool as a number (its sums lost): say how to
    // write it, or the model calls again the same way.
    if (const Json* e = args.find("expression"); parts.empty() && e && e->isNumber())
        return reply(done, "Your expression reached calculate as a bare number (" + e->dump() +
                               "), not as text: write the whole expression in quotes, as a string - calculate "
                               "{\"expression\": \"12.50 + 8 + 3.25\"}.");
    if (parts.empty())
        return reply(done, "calculate needs an expression. " + usage("calculate"));
    std::vector<std::pair<std::string, double>> named; // longest names first when substituted
    std::vector<std::string> dated;                    // names given a date
    std::string out;
    bool tiny = false; // a result below 0.001: a unit (millions) likely missed
    for (const std::string& part : parts) {
        std::string label, expression = part;
        // "name = expression": a name is letters, digits, spaces and _ starting with a letter.
        if (const size_t eq = part.find('='); eq != std::string::npos && eq > 0 && part.find("==") == std::string::npos) {
            const std::string left = trimmed(part.substr(0, eq));
            const bool word = !left.empty() && std::isalpha(static_cast<unsigned char>(left[0])) &&
                              std::all_of(left.begin(), left.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == ' '; });
            if (word) {
                label = left;
                expression = trimmed(part.substr(eq + 1));
            }
        }
        // Its dates as days; a name given a date earlier counts as one.
        int dates = 0;
        const std::string shown = expression;
        expression = datesAsDays(expression, dates);
        for (const std::string& name : dated)
            dates += lower(expression).find(lower(name)) != std::string::npos;
        auto evaluate = [&named](const std::string& expr, std::string& error) {
            // Earlier names, as their values.
            std::string text = expr;
            for (const auto& [name, value] : named)
                for (size_t at = lower(text).find(lower(name)); at != std::string::npos; at = lower(text).find(lower(name), at)) {
                    const bool before = at > 0 && (std::isalnum(static_cast<unsigned char>(text[at - 1])) || text[at - 1] == '_');
                    const size_t end = at + name.size();
                    const bool after = end < text.size() && (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_');
                    if (before || after) {
                        at = end;
                        continue;
                    }
                    const std::string v = "(" + numberText(value) + ")";
                    text.replace(at, name.size(), v);
                    at += v.size();
                }
            const std::string cleaned = plainNumbers(text); // kept: the calculator views it (string_view)
            Calculator calc;
            calc.s = cleaned;
            const double v = calc.sum();
            calc.space();
            if (calc.error.empty() && calc.i < calc.s.size())
                calc.error = std::string("unexpected '") + calc.s[calc.i] + "'";
            error = calc.error;
            return v;
        };
        // A comparison ("9 + 5 + 22 = 36", "a >= b"): both sides worked out, then true or false.
        std::string op;
        size_t opAt = std::string::npos;
        if (label.empty()) {
            int depth = 0;
            for (size_t k = 0; k < expression.size() && op.empty(); ++k) {
                const char c = expression[k];
                depth += c == '(' ? 1 : c == ')' ? -1 : 0;
                if (depth != 0)
                    continue;
                for (const char* o : {"==", "!=", "<=", ">=", "=", "<", ">"})
                    if (expression.compare(k, std::strlen(o), o) == 0) {
                        op = o;
                        opAt = k;
                        break;
                    }
            }
        }
        std::string error;
        if (!op.empty()) {
            std::string leftError, rightError;
            const double a = evaluate(expression.substr(0, opAt), leftError);
            const double b = evaluate(expression.substr(opAt + op.size()), rightError);
            error = leftError.empty() ? rightError : leftError;
            if (error.empty()) {
                const bool same = std::abs(a - b) <= 1e-9 * std::max({1.0, std::abs(a), std::abs(b)});
                const bool holds = op == "=" || op == "==" ? same : op == "!=" ? !same : op == "<" ? a < b && !same
                                   : op == ">" ? a > b && !same : op == "<=" ? a < b || same : a > b || same;
                out += shown + ": " + (holds ? "true" : "false") + " (" + numberText(a) + " " + op + " " + numberText(b) + ")\n";
                continue;
            }
        }
        const double v = op.empty() ? evaluate(expression, error) : 0.0;
        if (!error.empty())
            return reply(done, out + "Cannot calculate '" + shown + "': " + error + ". " + usage("calculate"));
        // With dates: two or more give the days between them; one, with days added or taken, a date.
        std::string value = numberText(v);
        if (dates == 1 && std::abs(v) > 3000 && std::abs(v - std::round(v)) < 1e-9) {
            value = civilDate(std::llround(v));
            if (!label.empty())
                dated.push_back(label);
        } else if (dates >= 2) {
            value += " days";
        }
        out += (label.empty() ? shown : label + " = " + shown) + " = " + value + "\n";
        tiny = tiny || (v != 0 && std::abs(v) < 0.001);
        if (!label.empty()) {
            named.emplace_back(label, v);
            std::stable_sort(named.begin(), named.end(), [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
        }
    }
    if (!out.empty())
        out.pop_back();
    if (tiny)
        out += "\n(A result this small often means a figure was given in millions or thousands, as a table's "
               "heading says: write it out - 5.5 million is 5.5 * 1000000 - and calculate again.)";
    reply(done, out);
}

// ---- Tables ---------------------------------------------------------------------------------------

namespace {

std::string trimmed(std::string s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    if (s.size() >= 2 && ((s.front() == '\'' && s.back() == '\'') || (s.front() == '"' && s.back() == '"')))
        s = s.substr(1, s.size() - 2);
    return s;
}

// A cell as a number: currency signs, thousands separators and a % are allowed; (12) is -12.
bool cellNumber(const std::string& text, double& out)
{
    std::string t;
    bool negative = false;
    for (char c : text) {
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.' || ((c == 'e' || c == 'E') && !t.empty())) {
            t += c;
        } else if (c == '(' || (c == '-' && t.empty())) {
            negative = true;
        } else if (c == ',' || c == ' ' || c == '$' || c == '%' || c == ')' || (c & 0x80)) {
            continue;
        } else {
            return false;
        }
    }
    if (t.empty() || !(std::isdigit(static_cast<unsigned char>(t.front())) || t.front() == '.'))
        return false;
    char* end = nullptr;
    out = std::strtod(t.c_str(), &end);
    if (!end || *end)
        return false;
    out = negative ? -out : out;
    return true;
}

std::vector<std::string> splitList(const std::string& text)
{
    std::vector<std::string> out;
    for (size_t from = 0; from <= text.size();) {
        size_t comma = text.find(',', from);
        if (comma == std::string::npos)
            comma = text.size();
        if (std::string part = trimmed(text.substr(from, comma - from)); !part.empty())
            out.push_back(std::move(part));
        from = comma + 1;
    }
    return out;
}

} // namespace

void LocalAgentTools::queryTable(const Json& args, Done done)
{
    std::string error;
    const std::string path = args.getString("path");
    const std::string absolute = resolve(path, error);
    if (absolute.empty())
        return reply(done, error);
    // A folder ("how many .wav files in audio"): its files, counted, with their sizes.
    if (os::fs::stat(absolute).isDirectory)
        return listFiles(Json::object().set("path", path), [done = std::move(done), path](viewmodel::AgentViewModel::ToolOutput out) {
            out.text = path + " is a folder, not a table: here are its files (list_files), with their sizes and counts.\n" + out.text;
            done(std::move(out));
        });
    std::optional<std::string> bytes = os::fs::readFile(absolute);
    if (!bytes)
        return reply(done, "cannot read " + path);
    std::vector<std::string> header;
    std::vector<std::vector<std::string>> rows;
    std::string note;
    const std::string ext = extension(path);
    if (ext == "xlsx" || ext == "xlsm") {
        model::viewer::ViewerDocument doc = model::viewer::readViewerDocument(absolute, std::move(*bytes));
        if (doc.book.sheets.empty())
            return reply(done, path + " has no sheets this can read" + (doc.error.empty() ? "." : ": " + doc.error));
        const model::viewer::Sheet* sheet = &doc.book.sheets.front();
        const std::string wanted = trimmed(args.getString("sheet"));
        bool found = wanted.empty();
        std::string names;
        for (const model::viewer::Sheet& s : doc.book.sheets) {
            names += (names.empty() ? "" : ", ") + s.name;
            if (!wanted.empty() && lower(s.name) == lower(wanted)) {
                sheet = &s;
                found = true;
            }
        }
        // A sheet it does not have: often a value meant as a filter ("2014"). Totals over the first
        // sheet instead would answer another question.
        if (!found)
            return reply(done, "There is no sheet '" + wanted + "' in " + path + ". Its sheets: " + names +
                                   ". Leave sheet out for the first; to pick rows by a value, use where with the column "
                                   "that holds it (column = " + wanted + ").");
        bool first = true;
        for (const model::viewer::Row& row : sheet->rows) {
            if (row.cells.empty())
                continue;
            std::vector<std::string> values(std::max<size_t>(sheet->columnCount, row.cells.back().column + 1));
            for (const model::viewer::Cell& c : row.cells)
                values[c.column] = c.text;
            if (first)
                header = std::move(values);
            else
                rows.push_back(std::move(values));
            first = false;
        }
        if (doc.book.sheets.size() > 1)
            note = "Sheets: " + names + " (this is " + sheet->name + ").\n";
    } else if (kindOf(path) == FileKind::Text) {
        const model::csv::Table t = model::csv::parse(*bytes, model::csv::detectDelimiter(*bytes, path));
        for (size_t r = 0; r < t.rowCount(); ++r) {
            std::vector<std::string> values;
            for (int c = 0; c < t.columns; ++c)
                values.emplace_back(t.cell(r, c));
            if (r == 0)
                header = std::move(values);
            else
                rows.push_back(std::move(values));
        }
    } else {
        return reply(done, path + " is not a table. " + whichTool(path, hears()));
    }
    if (header.empty())
        return reply(done, path + " is empty.");
    std::string columns;
    for (const std::string& h : header)
        columns += (columns.empty() ? "" : ", ") + trimmed(h);
    auto column = [&](const std::string& name) -> int {
        const std::string want = lower(trimmed(name));
        for (size_t c = 0; c < header.size(); ++c)
            if (lower(trimmed(header[c])) == want)
                return int(c);
        return -1;
    };
    auto unknown = [&](const std::string& name) {
        const std::string l = lower(name);
        return "There is no column '" + name + "'. The columns are: " + columns +
               (l == "month" || l == "year" || l == "months"
                    ? std::string(". For a month, ask it of the column of dates: Date = March (or March 2026, 2026-03).")
                    : std::string());
    };
    auto cell = [](const std::vector<std::string>& row, int c) { return size_t(c) < row.size() ? trimmed(row[size_t(c)]) : std::string(); };

    // where: "Country = Canada and Units Sold > 1000"
    struct Condition {
        int column;
        std::string op, value;
        int month = 0, year = 0; // "= March", "contains 2026-03": a month of a date column
    };
    std::vector<Condition> conditions;
    const std::string where = args.getString("where");
    // The conditions: joined by "and", "&&" or ";", or by a comma when a column name and a comparison
    // follow it ("Segment = Government, Year = 2014"; a comma inside a value stays).
    std::vector<std::string> parts;
    {
        static const char* const compare[] = {"==", "!=", "<>", ">=", "<=", "=", ">", "<", " contains "};
        auto startsCondition = [&](size_t at) {
            const std::string rest = lower(where.substr(at));
            for (const std::string& h : header) {
                const std::string name = lower(trimmed(h));
                if (name.empty() || rest.compare(0, name.size(), name) != 0)
                    continue;
                const std::string after = rest.substr(name.size());
                const size_t op = after.find_first_not_of(' ');
                for (const char* c : compare)
                    if (op != std::string::npos && after.compare(op, std::strlen(c), c[0] == ' ' ? c + 1 : c) == 0)
                        return true;
                if (after.compare(0, std::strlen(" contains "), " contains ") == 0)
                    return true;
            }
            return false;
        };
        const std::string l = lower(where);
        size_t from = 0;
        for (size_t i = 0; i < where.size(); ++i) {
            size_t skip = 0;
            if (l.compare(i, 5, " and ") == 0)
                skip = 5;
            else if (l.compare(i, 2, "&&") == 0)
                skip = 2;
            else if (where[i] == ';')
                skip = 1;
            else if (where[i] == ',') {
                size_t next = i + 1;
                while (next < where.size() && where[next] == ' ')
                    ++next;
                if (startsCondition(next))
                    skip = 1;
            }
            if (skip) {
                parts.push_back(where.substr(from, i - from));
                from = i + skip;
                i = from - 1;
            }
        }
        parts.push_back(where.substr(from));
    }
    for (const std::string& part : parts) {
        static const char* const ops[] = {" contains ", "==", ">=", "<=", "!=", "<>", "=", ">", "<"};
        bool parsed = false;
        for (const char* op : ops) {
            const size_t at = lower(part).find(op);
            if (at == std::string::npos)
                continue;
            const int c = column(part.substr(0, at));
            if (c < 0)
                return reply(done, unknown(trimmed(part.substr(0, at))));
            const std::string o = trimmed(op);
            conditions.push_back({c, o == "==" ? std::string("=") : o, trimmed(part.substr(at + std::strlen(op)))});
            // A month asked of a column of dates: the rows of that month (and year, when said).
            if (Condition& k = conditions.back(); k.op == "=" || k.op == "contains") {
                int y = 0, m = 0, d = 0, dated = 0, seen = 0;
                if (monthAsked(k.value, k.year, k.month)) {
                    for (const auto& row : rows) {
                        const std::string v = cell(row, c);
                        if (v.empty())
                            continue;
                        dated += cellDate(v, y, m, d);
                        if (++seen == 20)
                            break;
                    }
                    if (dated * 2 <= seen)
                        k.month = k.year = 0;
                }
            }
            parsed = true;
            break;
        }
        if (!parsed && !trimmed(part).empty())
            return reply(done, "Cannot read the condition '" + part + "': write column = value, column > 10 or column contains text.");
    }
    auto matches = [&](const std::vector<std::string>& row) {
        for (const Condition& k : conditions) {
            const std::string value = cell(row, k.column);
            if (k.month) {
                int y = 0, m = 0, d = 0;
                if (!cellDate(value, y, m, d) || m != k.month || (k.year && y != k.year))
                    return false;
                continue;
            }
            double a = 0, b = 0;
            const bool numeric = cellNumber(value, a) && cellNumber(k.value, b);
            const int cmp = numeric ? (a < b ? -1 : a > b ? 1 : 0) : lower(value).compare(lower(k.value));
            const bool ok = k.op == "=" ? cmp == 0 : k.op == "!=" || k.op == "<>" ? cmp != 0 : k.op == ">" ? cmp > 0
                            : k.op == "<" ? cmp < 0 : k.op == ">=" ? cmp >= 0 : k.op == "<=" ? cmp <= 0
                                                                                          : lower(value).find(lower(k.value)) != std::string::npos;
            if (!ok)
                return false;
        }
        return true;
    };
    std::vector<const std::vector<std::string>*> picked;
    for (const auto& row : rows)
        if (matches(row))
            picked.push_back(&row);
    // Nothing matches: a word written out where the file abbreviates it ("Sunday", the file says
    // "Sun"), or the other way round, is read as the one value it can mean.
    std::string readAs;
    if (picked.empty() && !rows.empty()) {
        for (Condition& k : conditions) {
            double n = 0;
            if (k.op != "=" || k.value.size() < 3 || cellNumber(k.value, n))
                continue;
            const std::string want = lower(k.value);
            std::string only;
            bool several = false;
            for (const auto& row : rows) {
                const std::string v = cell(row, k.column), low = lower(v);
                // One begins the other and the rest is the end of the same word ("Sun" + "day"): not
                // another value or condition run on ("Government, Year = 2014").
                const std::string& shorter = low.size() < want.size() ? low : want;
                const std::string& longer = low.size() < want.size() ? want : low;
                const bool sameWord = shorter.size() >= 3 && longer.rfind(shorter, 0) == 0 && longer.size() - shorter.size() <= 9 &&
                                      std::all_of(longer.begin() + long(shorter.size()), longer.end(),
                                                  [](char ch) { return std::isalpha(static_cast<unsigned char>(ch)) != 0; });
                if (!sameWord || low == lower(only))
                    continue;
                several = several || !only.empty();
                only = v;
            }
            if (!only.empty() && !several) {
                readAs += (readAs.empty() ? "" : ", ") + ("'" + k.value + "' read as '" + only + "'");
                k.value = only;
            }
        }
        if (!readAs.empty())
            for (const auto& row : rows)
                if (matches(row))
                    picked.push_back(&row);
    }
    // Still nothing: show what the filtered columns hold, to call again with.
    if (picked.empty() && !conditions.empty() && !rows.empty()) {
        std::string hint = path + ": no rows match '" + where + "'.";
        for (const Condition& k : conditions) {
            std::vector<std::string> seen;
            size_t more = 0;
            for (const auto& row : rows) {
                const std::string v = cell(row, k.column);
                if (v.empty() || std::find(seen.begin(), seen.end(), v) != seen.end())
                    continue;
                if (seen.size() < 20)
                    seen.push_back(v);
                else
                    ++more;
            }
            hint += "\n" + trimmed(header[size_t(k.column)]) + " holds: ";
            for (size_t i = 0; i < seen.size(); ++i)
                hint += (i ? ", " : "") + seen[i];
            if (more)
                hint += ", ...";
        }
        return reply(done, hint + "\nCall query_table again with one of these values exactly.");
    }
    // A label written several ways ("Taxi", "Transport - taxi"): "Category = Taxi" leaves out the rows
    // whose value holds the same word among others. The query is also made with "contains", and both
    // results are given, those rows named, for the model to judge (a model shown only the exact
    // result summed it). `exact_values` (set here, not by the model) is the query as made.
    if (!picked.empty() && !args.getBool("exact_values")) {
        std::string wider, others;
        size_t extra = 0;
        for (const Condition& k : conditions) {
            double n = 0;
            const bool widen = k.op == "=" && k.value.size() >= 3 && !cellNumber(k.value, n);
            const std::string& value = k.value;
            const size_t before = extra;
            if (widen) {
                const std::string want = lower(k.value);
                std::vector<std::string> seen;
                for (const auto& row : rows) {
                    const std::string v = cell(row, k.column), low = lower(v);
                    // As "contains" picks them, so the rows said are the rows added.
                    if (low != want && low.find(want) != std::string::npos) {
                        ++extra;
                        if (std::find(seen.begin(), seen.end(), v) == seen.end() && seen.size() < 8)
                            seen.push_back(v);
                    }
                }
                for (const std::string& v : seen)
                    others += (others.empty() ? "'" : ", '") + v + "'";
            }
            wider += (wider.empty() ? "" : " and ") + trimmed(header[size_t(k.column)]) +
                     (extra > before ? " contains " : " " + k.op + " ") + value;
        }
        if (extra > 0) {
            Json widerArgs = args, exactArgs = args;
            widerArgs.set("where", wider);
            widerArgs.set("exact_values", true);
            exactArgs.set("exact_values", true);
            const std::string head = "Other rows hold the same word in another way of writing it: " + others + " (" +
                                     std::to_string(extra) + " rows). If they are the same thing, this is the result with them (" + wider + "):\n";
            return queryTable(widerArgs, [this, exactArgs = std::move(exactArgs), head, where, done = std::move(done)](viewmodel::AgentViewModel::ToolOutput w) mutable {
                queryTable(exactArgs, [w = std::move(w), head, where, done = std::move(done)](viewmodel::AgentViewModel::ToolOutput e) mutable {
                    w.text = head + w.text + "\n\nExactly as called (" + where + "), without them:\n" + e.text;
                    done(std::move(w));
                });
            });
        }
    }
    // The question names a value of a column ("red cars in Lisbon": red of colour, Lisbon of city)
    // that this query neither picks rows by nor groups by: said first, as a small model drops one of
    // two filters. A column it filters by another value (one step for Sunday, the next for Saturday)
    // is not said.
    // When it names one value of such a column, the query is also made with that value added, and its
    // result comes first: a small model told so in a note went on without it, three calls in a row.
    // `as_called` (set here, not by the model) is the query as made, without that.
    std::string added; // "species = Gentoo and island = Biscoe"
    if (!question_.empty() && !args.getBool("as_called")) {
        std::vector<std::string> grouped;
        for (const std::string& g : splitList(args.getString("group_by")))
            grouped.push_back(lower(trimmed(g)));
        grouped.push_back(lower(trimmed(args.getString("group_by"))));
        for (size_t c = 0; c < header.size(); ++c) {
            const std::string name = trimmed(header[c]);
            if (name.empty() || std::find(grouped.begin(), grouped.end(), lower(name)) != grouped.end() ||
                std::any_of(conditions.begin(), conditions.end(), [&](const Condition& k) { return k.column == int(c); }))
                continue;
            std::vector<std::string> values;
            bool text = true;
            for (const auto& row : rows) {
                const std::string v = cell(row, int(c));
                double n = 0;
                if (v.empty() || std::find(values.begin(), values.end(), v) != values.end())
                    continue;
                if (cellNumber(v, n) || values.size() >= 12) {
                    text = false; // numbers, or too many values to be categories
                    break;
                }
                values.push_back(v);
            }
            if (!text || values.size() < 2)
                continue;
            std::string named;
            int count = 0;
            for (const std::string& v : values)
                if (wordStarts(question_, lower(v))) {
                    named += (named.empty() ? "" : ", ") + v;
                    ++count;
                }
            if (named.empty())
                continue;
            if (count == 1 && lower(named).find(" and ") == std::string::npos && named.find(';') == std::string::npos)
                added += (added.empty() ? "" : " and ") + name + " = " + named;
            else
                note += "(My question names " + named + ", a value of the column " + name +
                        ", and this query does not pick rows by it: add it to where (" + name + " = " +
                        named.substr(0, named.find(',')) + "), or group_by " + name + " to see each.)\n";
        }
    }
    if (!added.empty()) {
        Json filtered = args;
        filtered.set("where", trimmed(where).empty() ? added : where + " and " + added);
        Json asCalled = args;
        asCalled.set("as_called", true);
        return queryTable(filtered, [this, asCalled = std::move(asCalled), added, done = std::move(done)](viewmodel::AgentViewModel::ToolOutput f) mutable {
            queryTable(asCalled, [f = std::move(f), added, done = std::move(done)](viewmodel::AgentViewModel::ToolOutput a) mutable {
                f.text = "My question names it, so this is with " + added + " added to where:\n" + f.text +
                         "\n\nAs called, without " + added + ":\n" + a.text;
                done(std::move(f));
            });
        });
    }
    // Rows asked for, with a column of a few values each repeated ("Product, Units Sold" sorted, limit
    // 1): the totals per value come first, then the rows. Models read the top row as the product that
    // sold most, and counted smokers from five rows shown. `as_rows` (set here) is the call as made.
    if (args.getString("group_by").empty() && args.getString("aggregate").empty() && !args.getBool("as_rows") && picked.size() > 4) {
        std::string category;
        std::vector<std::string> sums;
        for (const std::string& name : splitList(args.getString("columns"))) {
            const int c = column(name);
            if (c < 0)
                continue;
            std::vector<std::string> values;
            bool numbers = true, few = true;
            for (const auto* row : picked) {
                const std::string v = cell(*row, c);
                double n = 0;
                numbers = numbers && (v.empty() || cellNumber(v, n));
                if (!v.empty() && std::find(values.begin(), values.end(), v) == values.end()) {
                    values.push_back(v);
                    if (values.size() > 12) {
                        few = false;
                        break;
                    }
                }
            }
            if (numbers)
                sums.push_back("sum(" + trimmed(header[size_t(c)]) + ")");
            else if (category.empty() && few && values.size() >= 2 && picked.size() >= values.size() * 2)
                category = trimmed(header[size_t(c)]);
        }
        if (!category.empty()) {
            Json grouped = args, asRows = args;
            grouped.set("group_by", category);
            std::string aggregate = "count";
            for (const std::string& s : sums)
                aggregate += ", " + s;
            grouped.set("aggregate", aggregate);
            // Sorted by a column it totals (as its sum), else as the groups come.
            const std::string sort = lower(args.getString("sort"));
            std::string bySum;
            for (const std::string& s : sums)
                if (!sort.empty() && sort.find(lower(s.substr(4, s.size() - 5))) != std::string::npos)
                    bySum = s + (sort.find("asc") != std::string::npos ? " asc" : " desc");
            grouped.set("sort", bySum);
            grouped.set("columns", std::string());
            grouped.set("limit", 30);
            grouped.set("as_rows", true);
            asRows.set("as_rows", true);
            return queryTable(grouped, [this, asRows = std::move(asRows), category, done = std::move(done)](viewmodel::AgentViewModel::ToolOutput g) mutable {
                queryTable(asRows, [g = std::move(g), category, done = std::move(done)](viewmodel::AgentViewModel::ToolOutput r) mutable {
                    g.text = category + " repeats from row to row: all rows of each " + category + " together first (for 'which " +
                             category + " has the most', 'how many are ...'):\n" + g.text + "\n\nThe rows, as called:\n" + r.text;
                    done(std::move(g));
                });
            });
        }
    }
    std::string out = note + path + ": " + std::to_string(picked.size()) + " of " + std::to_string(rows.size()) +
                      " rows match" + (readAs.empty() ? "" : " (" + readAs + ")") + ". Columns: " + columns + "\n";
    const int limit = std::clamp(argInt(args, "limit", 30), 1, 500);

    // Totals, by group when asked.
    if (const std::vector<std::string> aggregates = splitList(args.getString("aggregate")); !aggregates.empty()) {
        struct Agg {
            std::string fn, label;
            int column = -1;
            std::string expr; // a sum over each row ("qty * unit_cost") instead of one column
        };
        // The columns by name, longest first: put in an expression by name ("Units Sold" before "Units").
        std::vector<int> byLength;
        for (size_t c = 0; c < header.size(); ++c)
            if (!trimmed(header[c]).empty())
                byLength.push_back(int(c));
        std::sort(byLength.begin(), byLength.end(), [&](int a, int b) { return trimmed(header[size_t(a)]).size() > trimmed(header[size_t(b)]).size(); });
        // An expression of a row's columns, worked out with its values (false: a cell is no number,
        // or it is no expression of columns).
        auto rowExpression = [&](const std::string& expr, const std::vector<std::string>* row, double& v, std::string& why) {
            std::string text = expr;
            for (const int c : byLength) {
                const std::string name = lower(trimmed(header[size_t(c)]));
                for (size_t at = lower(text).find(name); at != std::string::npos; at = lower(text).find(name, at)) {
                    const size_t end = at + name.size();
                    const auto word = [&](size_t k) { return k < text.size() && (std::isalnum(static_cast<unsigned char>(text[k])) || text[k] == '_'); };
                    if ((at > 0 && word(at - 1)) || word(end)) {
                        at = end;
                        continue;
                    }
                    double n = 1;
                    if (row && !cellNumber(cell(*row, c), n))
                        return false;
                    const std::string value = "(" + numberText(n) + ")";
                    text.replace(at, name.size(), value);
                    at += value.size();
                }
            }
            Calculator calc;
            calc.s = text;
            v = calc.sum();
            calc.space();
            if (calc.error.empty() && calc.i < calc.s.size())
                calc.error = std::string("unexpected '") + calc.s[calc.i] + "'";
            why = calc.error;
            return why.empty();
        };
        std::vector<Agg> aggs;
        for (const std::string& a : aggregates) {
            Agg g;
            g.label = a;
            const size_t open = a.find('(');
            g.fn = lower(trimmed(open == std::string::npos ? a : a.substr(0, open)));
            if (open != std::string::npos) {
                const size_t close = a.rfind(')');
                // "sum(qty) * sum(unit_cost)": an aggregate is one function of one column or expression
                // (its brackets close after the last, never before).
                int depth = 0;
                bool split = false;
                for (size_t k = open + 1; close != std::string::npos && k < close; ++k) {
                    depth += a[k] == '(' ? 1 : a[k] == ')' ? -1 : 0;
                    split = split || depth < 0;
                }
                if (close == std::string::npos || close < open || split || !trimmed(a.substr(close + 1)).empty())
                    return reply(done, "'" + trimmed(a) + "' is not one aggregate. Write each as fn(column) or fn(an expression of "
                                       "the row's columns), with fn sum, avg, min, max or count: for the value of the stock, "
                                       "sum(qty * unit_cost), not sum(qty) * sum(unit_cost).");
                const std::string name = a.substr(open + 1, close - open - 1);
                g.column = column(name);
                if (g.column < 0) {
                    double v = 0;
                    std::string why;
                    if (name.find_first_of("*/+-") == std::string::npos || !rowExpression(name, nullptr, v, why))
                        return reply(done, unknown(trimmed(name)) + ". Inside an aggregate: a column, or an expression of columns "
                                           "(sum(qty * unit_cost)).");
                    g.expr = trimmed(name);
                }
            }
            if (g.fn == "average" || g.fn == "mean")
                g.fn = "avg";
            else if (g.fn == "total")
                g.fn = "sum";
            else if (g.fn == "minimum")
                g.fn = "min";
            else if (g.fn == "maximum")
                g.fn = "max";
            // "sum" alone with one column named in columns: sum of that column.
            if (open == std::string::npos && g.fn != "count")
                if (const std::vector<std::string> cols = splitList(args.getString("columns")); cols.size() == 1) {
                    g.column = column(cols[0]);
                    g.label = g.fn + "(" + trimmed(cols[0]) + ")";
                }
            if (g.fn != "count" && ((g.column < 0 && g.expr.empty()) || (g.fn != "sum" && g.fn != "avg" && g.fn != "min" && g.fn != "max")))
                return reply(done, "Write each aggregate as sum(column), avg(column), min(column), max(column) or count.");
            aggs.push_back(g);
        }
        // By one column, or by several ("day, time": a group for each pair of values).
        const std::string groupName = args.getString("group_by");
        std::vector<int> groupBy;
        if (!groupName.empty()) {
            if (const int one = column(groupName); one >= 0) {
                groupBy.push_back(one);
            } else {
                for (const std::string& name : splitList(groupName)) {
                    const int c = column(name);
                    if (c < 0)
                        return reply(done, unknown(trimmed(name)));
                    groupBy.push_back(c);
                }
            }
        }
        const int group = groupBy.empty() ? -1 : groupBy.front();
        std::vector<std::string> order;
        std::map<std::string, std::vector<const std::vector<std::string>*>> groups;
        for (const auto* row : picked) {
            std::string key;
            for (const int c : groupBy)
                key += (key.empty() ? "" : " | ") + cell(*row, c);
            if (groupBy.empty())
                key = "all rows";
            if (!groups.count(key))
                order.push_back(key);
            groups[key].push_back(row);
        }
        std::string groupHeader;
        for (const int c : groupBy)
            groupHeader += (groupHeader.empty() ? "" : " | ") + trimmed(header[size_t(c)]);
        out += groupBy.empty() ? std::string("rows") : groupHeader;
        for (const Agg& g : aggs)
            out += " | " + g.label;
        out += "\n";
        // Each group's values first, so the groups can be sorted by one ("sum(Profit) desc").
        std::vector<std::pair<std::string, std::vector<double>>> results;
        // Without groups, the row a min or max is on (a question asks "on which day").
        std::vector<const std::vector<std::string>*> extremeRow(aggs.size(), nullptr);
        std::vector<int> extremeRows(aggs.size(), 0);
        for (const std::string& key : order) {
            std::vector<double> values;
            for (size_t k = 0; k < aggs.size(); ++k) {
                const Agg& g = aggs[k];
                double total = 0, low = 0, high = 0;
                int count = 0;
                for (const auto* row : groups[key]) {
                    double v = 0;
                    std::string why;
                    if (g.fn == "count") {
                        ++count;
                    } else if (g.expr.empty() ? cellNumber(cell(*row, g.column), v) : rowExpression(g.expr, row, v, why)) {
                        const double extreme = g.fn == "min" ? low : high;
                        if (count == 0 || (g.fn == "min" ? v < extreme : v > extreme)) {
                            extremeRow[k] = row;
                            extremeRows[k] = 1;
                        } else if (v == extreme) {
                            ++extremeRows[k];
                        }
                        low = count == 0 ? v : std::min(low, v);
                        high = count == 0 ? v : std::max(high, v);
                        total += v;
                        ++count;
                    }
                }
                values.push_back(g.fn == "count" ? count : g.fn == "sum" ? total : g.fn == "avg" ? (count ? total / count : 0.0)
                                 : g.fn == "min" ? low : high);
            }
            results.emplace_back(key, std::move(values));
        }
        std::string sort = lower(trimmed(args.getString("sort")));
        bool descending = false;
        if (sort.size() > 5 && sort.compare(sort.size() - 5, 5, " desc") == 0) {
            descending = true;
            sort = trimmed(sort.substr(0, sort.size() - 5));
        } else if (sort.size() > 4 && sort.compare(sort.size() - 4, 4, " asc") == 0) {
            sort = trimmed(sort.substr(0, sort.size() - 4));
        }
        auto squeezed = [](std::string s) {
            s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c); }), s.end());
            return lower(s);
        };
        if (!sort.empty()) {
            int by = -1;
            for (size_t k = 0; k < aggs.size(); ++k)
                if (squeezed(aggs[k].label) == squeezed(sort) || (aggs.size() == 1 && column(sort) == aggs[k].column))
                    by = int(k);
            if (by >= 0)
                std::stable_sort(results.begin(), results.end(), [&](const auto& a, const auto& b) {
                    return descending ? a.second[size_t(by)] > b.second[size_t(by)] : a.second[size_t(by)] < b.second[size_t(by)];
                });
            else if (group >= 0 && std::find(groupBy.begin(), groupBy.end(), column(sort)) != groupBy.end())
                std::stable_sort(results.begin(), results.end(), [&](const auto& a, const auto& b) {
                    return descending ? lower(a.first) > lower(b.first) : lower(a.first) < lower(b.first);
                });
            else // the totals all the same, unsorted, saying how they could be sorted
                out = "(Not sorted: '" + args.getString("sort") + "' is not a column of this result; sort by " +
                      (group >= 0 ? groupHeader + " or " : std::string()) + args.getString("aggregate") +
                      ", with desc for largest first.)\n" + out;
        }
        // A limit on groups in the order they come: the first ones, not the largest (it was read as the
        // largest: "the hottest day" was the first day).
        if (int(results.size()) > limit && sort.empty())
            out = "(These are the first " + std::to_string(limit) + " of " + std::to_string(results.size()) +
                  " groups in the order they come, not the largest: add sort, e.g. " + aggs.front().label +
                  " desc, for the largest first.)\n" + out;
        int shown = 0;
        for (const auto& [key, values] : results) {
            if (++shown > limit)
                break;
            out += key;
            for (double value : values)
                out += " | " + numberText(std::round(value * 1e6) / 1e6);
            out += "\n";
        }
        if (int(results.size()) > limit)
            out += "[" + std::to_string(results.size() - size_t(limit)) + " more groups]\n";
        // Columns asked for with a total of all rows ("count" and "item, qty"): the rows too, so it
        // can name them, not only count them.
        if (groupBy.empty()) {
            std::vector<int> cols;
            for (const std::string& name : splitList(args.getString("columns")))
                if (const int c = column(name); c >= 0)
                    cols.push_back(c);
            if (!cols.empty()) {
                out += "\nThe rows:\n";
                for (size_t k = 0; k < cols.size(); ++k)
                    out += (k ? " | " : "") + trimmed(header[size_t(cols[k])]);
                out += "\n";
                int listed = 0;
                for (const auto* row : picked) {
                    if (++listed > limit)
                        break;
                    for (size_t k = 0; k < cols.size(); ++k)
                        out += (k ? " | " : "") + cell(*row, cols[k]);
                    out += "\n";
                }
                if (int(picked.size()) > limit)
                    out += "[" + std::to_string(picked.size() - size_t(limit)) + " more rows]\n";
            }
        }
        if (groupBy.empty())
            for (size_t k = 0; k < aggs.size(); ++k) {
                if ((aggs[k].fn != "min" && aggs[k].fn != "max") || !extremeRow[k])
                    continue;
                std::string row;
                for (size_t c = 0; c < header.size(); ++c)
                    if (!trimmed(header[c]).empty())
                        row += (row.empty() ? "" : ", ") + trimmed(header[c]) + " = " + cell(*extremeRow[k], int(c));
                out += aggs[k].label + " is on the row: " + row +
                       (extremeRows[k] > 1 ? " (the first of " + std::to_string(extremeRows[k]) + " rows with it)" : std::string()) + "\n";
            }
        return reply(done, out);
    }

    // Rows: sorted, the columns asked for.
    std::string sort = trimmed(args.getString("sort"));
    if (!sort.empty()) {
        bool descending = false;
        const std::string l = lower(sort);
        if (l.size() > 5 && l.compare(l.size() - 5, 5, " desc") == 0) {
            descending = true;
            sort = trimmed(sort.substr(0, sort.size() - 5));
        } else if (l.size() > 4 && l.compare(l.size() - 4, 4, " asc") == 0) {
            sort = trimmed(sort.substr(0, sort.size() - 4));
        }
        const int c = column(sort);
        if (c < 0)
            return reply(done, unknown(sort));
        std::stable_sort(picked.begin(), picked.end(), [&](const auto* a, const auto* b) {
            const std::string x = cell(*a, c), y = cell(*b, c);
            double p = 0, q = 0;
            if (cellNumber(x, p) && cellNumber(y, q))
                return descending ? p > q : p < q;
            return descending ? lower(x) > lower(y) : lower(x) < lower(y);
        });
    }
    std::vector<int> shownColumns;
    for (const std::string& name : splitList(args.getString("columns"))) {
        const int c = column(name);
        if (c < 0)
            return reply(done, unknown(name));
        shownColumns.push_back(c);
    }
    if (shownColumns.empty())
        for (size_t c = 0; c < header.size(); ++c)
            shownColumns.push_back(int(c));
    for (size_t k = 0; k < shownColumns.size(); ++k)
        out += (k ? " | " : "") + trimmed(header[size_t(shownColumns[k])]);
    out += "\n";
    int shown = 0;
    for (const auto* row : picked) {
        if (++shown > limit)
            break;
        for (size_t k = 0; k < shownColumns.size(); ++k)
            out += (k ? " | " : "") + cell(*row, shownColumns[k]);
        out += "\n";
    }
    if (int(picked.size()) > limit)
        out += "[" + std::to_string(picked.size() - size_t(limit)) + " more rows: narrow with where, or total with aggregate]\n";
    reply(done, out);
}

// ---- Binaries -------------------------------------------------------------------------------------

void LocalAgentTools::inspectBinary(const Json& args, Done done)
{
    std::string error;
    const std::string path = args.getString("path");
    const std::string absolute = resolve(path, error);
    if (absolute.empty())
        return reply(done, error);
    const os::fs::FileInfo info = os::fs::stat(absolute);
    if (!info.exists)
        return reply(done, path + " does not exist");
    const model::bin::Reader reader = [&absolute](std::uint64_t offset, size_t size, std::string& out) {
        std::optional<std::string> part = os::fs::readFileRange(absolute, offset, size);
        if (!part)
            return false;
        out = std::move(*part);
        return true;
    };
    std::string head;
    reader(0, 4096, head);
    if (!model::bin::recognises(head))
        return reply(done, path + " is not a program or library EasyNotepad reads (PE, ELF, objects, archives). "
                               "read_bytes shows its raw bytes.");
    const model::bin::Image image = model::bin::read(reader, info.size);
    if (!image.error.empty())
        return reply(done, path + " could not be read: " + image.error);
    const std::string filter = lower(args.getString("filter"));
    std::string out = path + ": " + image.summary + "\n";
    int lines = 0;
    auto add = [&](const std::string& line) {
        if (++lines <= 200)
            out += line + "\n";
    };
    auto symbols = [&](const std::string& which, size_t most) {
        size_t listed = 0, matching = 0;
        for (const model::bin::Symbol& s : image.symbols) {
            if ((which == "exports" || which == "functions") && s.side != model::bin::Side::Export)
                continue;
            if (which == "imports" && s.side != model::bin::Side::Import)
                continue;
            if (which == "functions" && s.kind != model::bin::SymbolKind::Function)
                continue;
            const std::string shown(image.text(s.shown));
            if (!filter.empty() && lower(shown).find(filter) == std::string::npos)
                continue;
            if (++matching > most)
                continue;
            std::string line = "  " + shown;
            if (s.demangled)
                line += "    (" + std::string(image.text(s.name)) + ")";
            if (s.library != model::bin::kNoIndex && s.library < image.libraries.size())
                line += "    from " + std::string(image.text(image.libraries[s.library].name));
            if (s.kind == model::bin::SymbolKind::Data)
                line += "    [data]";
            // A PE export forwarded to another DLL ("NTDLL.RtlGetVersion"); an ELF symbol's version.
            if (std::string detail(image.text(s.detail)); !detail.empty()) {
                if (detail.rfind("\xE2\x86\x92", 0) == 0) // the binary view's own arrow
                    detail = trimmed(detail.substr(3));
                if (s.side == model::bin::Side::Export && image.format == model::bin::Format::Pe)
                    line += "    -> forwarded to " + detail;
                else if (s.library == model::bin::kNoIndex)
                    line += "    [" + detail + "]";
            }
            add(line);
            ++listed;
        }
        if (matching == 0)
            add(filter.empty() ? "  (none)" : "  (none containing '" + filter + "')");
        else if (matching > listed)
            add("  [" + std::to_string(matching - listed) + " more " + which + ": ask for list " + which + ", or a filter]");
    };
    auto libraries = [&]() {
        for (const model::bin::Library& l : image.libraries)
            add("  " + std::string(image.text(l.name)) + "  (" + std::to_string(l.symbols) + " imports" + (l.delayed ? ", delay-loaded" : "") + ")");
        if (image.libraries.empty())
            add("  (none)");
    };
    // One list or several ("exports, imports").
    std::string lists = lower(args.getString("list", "overview"));
    for (char& c : lists)
        if (c == ';' || c == '+' || c == '&' || c == '/')
            c = ',';
    for (std::string which : splitList(lists)) {
        if (which.back() != 's' && which != "overview" && which != "all")
            which += 's'; // "export" -> "exports"
        if (which == "overview" || which == "all") {
            for (const model::bin::Property& p : image.overview)
                add(p.heading ? std::string(image.text(p.key)) + ":" : "  " + std::string(image.text(p.key)) + ": " + std::string(image.text(p.value)));
            // The usual question answered at once: what it offers, and what it needs.
            add("Libraries it uses:");
            libraries();
            add("Exports (functions and data it offers):");
            symbols("exports", 30);
            int imports = 0;
            for (const model::bin::Symbol& s : image.symbols)
                imports += s.side == model::bin::Side::Import;
            add(std::to_string(imports) + " imports, " + std::to_string(image.sections.size()) +
                " sections: list imports or sections for them.");
        } else if (which == "exports" || which == "imports" || which == "functions" || which == "symbols") {
            add(std::string(1, char(std::toupper(static_cast<unsigned char>(which[0])))) + which.substr(1) + ":");
            symbols(which, 150);
        } else if (which == "sections") {
            add("Sections:");
            for (const model::bin::Section& s : image.sections) {
                char buf[160];
                std::snprintf(buf, sizeof(buf), "  address 0x%llx, %llu bytes, %s%s%s", static_cast<unsigned long long>(s.address),
                              static_cast<unsigned long long>(s.memorySize), s.read ? "r" : "-", s.write ? "w" : "-", s.execute ? "x" : "-");
                add("  " + std::string(image.text(s.name)) + buf);
            }
        } else if (which == "librarys" || which == "libraries" || which == "dependencies" || which == "dlls") {
            add("Libraries:");
            libraries();
        } else {
            return reply(done, "Unknown list '" + which + "': overview, exports, imports, functions, sections or libraries "
                                                          "(several with commas).");
        }
    }
    if (lines > 200)
        out += "[" + std::to_string(lines - 200) + " more lines: use filter]\n";
    reply(done, out);
}

void LocalAgentTools::readBytes(const Json& args, Done done)
{
    std::string error;
    const std::string path = args.getString("path");
    const std::string absolute = resolve(path, error);
    if (absolute.empty())
        return reply(done, error);
    const std::uint64_t offset = std::uint64_t(std::max(0, argInt(args, "offset", 0)));
    const size_t length = size_t(std::clamp(argInt(args, "length", 256), 1, 4096));
    std::uint64_t total = 0;
    const std::optional<std::string> bytes = os::fs::readFileRange(absolute, offset, length, &total);
    if (!bytes)
        return reply(done, "cannot read " + path);
    std::string out = path + ": " + std::to_string(total) + " bytes; " + std::to_string(bytes->size()) + " from offset " +
                      std::to_string(offset) + ":\n";
    for (size_t row = 0; row < bytes->size(); row += 16) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%08llx  ", static_cast<unsigned long long>(offset + row));
        std::string hex = buf, text;
        for (size_t k = 0; k < 16; ++k) {
            if (row + k < bytes->size()) {
                const unsigned char c = static_cast<unsigned char>((*bytes)[row + k]);
                std::snprintf(buf, sizeof(buf), "%02x ", c);
                hex += buf;
                text += c >= 32 && c < 127 ? char(c) : '.';
            } else {
                hex += "   ";
            }
            if (k == 7)
                hex += ' ';
        }
        out += hex + " |" + text + "|\n";
    }
    reply(done, out);
}

// ---- Transcription --------------------------------------------------------------------------------

void LocalAgentTools::transcribe(const Json& args, Done done)
{
    std::string error;
    const std::string path = args.getString("path");
    const std::string absolute = resolve(path, error);
    if (absolute.empty())
        return reply(done, error);
    if (kindOf(path) != FileKind::Sound)
        return reply(done, path + " is not a recording. " + whichTool(path, hears()));
    const os::fs::FileInfo info = os::fs::stat(absolute);
    if (!info.exists)
        return reply(done, path + " does not exist");
    // Where the text goes: next to the recording, never over another file. Always word for word: a
    // small model given a say put its summary in place of the words.
    std::string target = absolute.substr(0, absolute.find_last_of('.')) + ".transcript.md";
    const std::string base = target;
    // Transcribed here before, since the recording last changed: that text again, not a second copy
    // ("call.transcript (2).md", "(3)": the next question read the wrong one, or all of them).
    if (const os::fs::FileInfo had = os::fs::stat(base); had.exists && !had.isDirectory && had.modifiedTime >= info.modifiedTime) {
        const std::string heading = "# Transcript of " + fileName(path) + "\n\n";
        if (std::optional<std::string> text = os::fs::readFile(base); text && text->rfind(heading, 0) == 0)
            return reply(done, "Transcribed before: the transcript of " + path + " is " + vm_.workspaceRelative(base) +
                                   ". The text:\n" + text->substr(heading.size()) +
                                   othersBeside(path, absolute, fileName(path), false));
    }
    const viewmodel::AgentViewModel::LocalSetup setup = vm_.localSetup();
    if (!engine_ || !setup.ready || (nativeEars() ? setup.projectorPath.empty() : setup.earsPath.empty()))
        return reply(done, "Transcribing needs the local model's ears: the user can download them in the agent set-up.");
    if (info.size > (200ull << 20))
        return reply(done, path + " is too large to transcribe here (over 200 MB).");
    std::optional<std::string> bytes = os::fs::readFile(absolute);
    if (!bytes)
        return reply(done, "cannot read " + path);
    for (int n = 2; os::fs::exists(target) && n < 100; ++n) {
        const size_t dot = base.find_last_of('.');
        target = base.substr(0, dot) + " (" + std::to_string(n) + ")" + base.substr(dot);
    }
    std::weak_ptr<bool> alive = alive_;
    viewmodel::NotepadViewModel* vm = &vm_;
    const std::string others = othersBeside(path, absolute, fileName(path), false);
    writeDown(
        path, std::move(*bytes),
        [alive, vm, path, target, others, done = std::move(done)](const interfaces::LocalChatResult& r) {
            if (!alive.lock())
                return;
            if (!r.ok)
                return reply(done, "Transcribing " + path + " failed: " + r.error);
            std::string writeError;
            if (!os::fs::writeFile(target, "# Transcript of " + fileName(path) + "\n\n" + r.text + "\n", &writeError))
                return reply(done, "Transcribed " + path + " but could not write " + target + ": " + writeError + "\n\n" + r.text);
            vm->openFile(target);
            size_t words = 0;
            for (size_t k = 0; k < r.text.size(); ++k)
                words += !std::isspace(static_cast<unsigned char>(r.text[k])) && (k == 0 || std::isspace(static_cast<unsigned char>(r.text[k - 1])));
            reply(done, "Wrote the transcript of " + path + " (" + std::to_string(words) + " words) to " + vm->workspaceRelative(target) +
                            " and opened it for the user. For minutes: the text does not say who speaks - use the names that "
                            "are said; \"I\" is a participant, not you; list every action item said, with its owner and date. "
                            "The text:\n" + r.text + others);
        });
}

// ---- Pictures made by tools -----------------------------------------------------------------------

std::string LocalAgentTools::imagesDirectory()
{
    const std::string dir = os::fs::executableDirectory();
    return dir.empty() ? std::string() : dir + "/temp/agent-images";
}

std::string LocalAgentTools::savePicture(const LocalMedia& rgb, const std::string& stem)
{
    const std::string dir = imagesDirectory();
    if (dir.empty() || rgb.width <= 0 || rgb.height <= 0 || rgb.data.size() != size_t(rgb.width) * size_t(rgb.height) * 3)
        return {};
    os::fs::createDirectories(dir);
    std::vector<unsigned char> rgba(size_t(rgb.width) * size_t(rgb.height) * 4);
    for (size_t i = 0, n = size_t(rgb.width) * size_t(rgb.height); i < n; ++i) {
        rgba[i * 4 + 0] = static_cast<unsigned char>(rgb.data[i * 3 + 0]);
        rgba[i * 4 + 1] = static_cast<unsigned char>(rgb.data[i * 3 + 1]);
        rgba[i * 4 + 2] = static_cast<unsigned char>(rgb.data[i * 3 + 2]);
        rgba[i * 4 + 3] = 255;
    }
    const std::string png = services::encodePng(rgba.data(), rgb.width, rgb.height);
    // A name of its own: the stem made safe, and the time.
    std::string safe;
    for (char c : stem.substr(0, 40))
        safe += std::isalnum(static_cast<unsigned char>(c)) || c == '-' ? c : '_';
    const std::string path = dir + "/" + safe + "-" +
                             std::to_string(std::chrono::system_clock::now().time_since_epoch().count() % 1000000000000LL) + ".png";
    return !png.empty() && os::fs::writeFile(path, png) ? path : std::string();
}

void LocalAgentTools::showImage(const Json& args, Done done)
{
    std::string path = args.getString("path");
    std::replace(path.begin(), path.end(), '\\', '/');
    std::string absolute;
    const std::string images = imagesDirectory();
    if (!images.empty() && path.rfind(images + "/", 0) == 0) {
        absolute = path; // a tool's picture
    } else {
        std::string error;
        absolute = resolve(path, error);
        if (absolute.empty())
            return reply(done, error);
    }
    if (!os::fs::exists(absolute))
        return reply(done, path + " does not exist");
    if (kindOf(absolute) != FileKind::Picture)
        return reply(done, path + " is not a picture. " + whichTool(path, hears()));
    Output out;
    out.text = "Shown " + fileName(path) + " to the user.";
    out.shown.push_back(absolute);
    done(std::move(out));
}

} // namespace enp::app
