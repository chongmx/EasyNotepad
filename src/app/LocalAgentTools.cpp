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
constexpr int kLookSide = 800;           // a picture's longest side, in pixels, as the model sees it (normal detail)
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
std::string whichTool(const std::string& path)
{
    const std::string p = Json(path).dump(); // quoted and escaped
    switch (kindOf(path)) {
    case FileKind::Document:
        return "It is a document: call read_document {\"path\": " + p + "} for its text, or look_at_page {\"path\": " + p +
               ", \"page\": 1} to see a page.";
    case FileKind::Picture: return "It is a picture: call look_at_image {\"path\": " + p + "}.";
    case FileKind::Sound: return "It is a recording: call listen_to {\"path\": " + p + "}.";
    case FileKind::Text: break;
    }
    return "Call read_file {\"path\": " + p + "} for its text.";
}

bool looksBinary(const std::string& bytes)
{
    const size_t n = std::min<size_t>(bytes.size(), 8192);
    for (size_t i = 0; i < n; ++i)
        if (bytes[i] == '\0')
            return true;
    return false;
}

// The longest side a picture is given to the model at, for how carefully it looks: Gemma 4 spends 70 to
// 1120 tokens on a picture, about one per 48 x 48 pixels.
int sideFor(const std::string& detail)
{
    const std::string d = lower(detail);
    if (d == "low")
        return 400;
    if (d == "high" || d == "ocr" || d == "max")
        return 1600;
    return kLookSide;
}

// RGBA (as decoded or captured) to RGB, shrunk (area-averaged by an integer factor) so the longest
// side is at most kLookSide.
LocalMedia picture(const std::string& name, const unsigned char* rgba, int width, int height, int stride, int maxSide = kLookSide)
{
    const int factor = std::max(1, int(std::ceil(double(std::max(width, height)) / maxSide)));
    LocalMedia m;
    m.kind = LocalMedia::Kind::Image;
    m.name = name;
    m.width = std::max(1, width / factor);
    m.height = std::max(1, height / factor);
    m.data.resize(size_t(m.width) * size_t(m.height) * 3);
    for (int y = 0; y < m.height; ++y)
        for (int x = 0; x < m.width; ++x) {
            unsigned sum[3] = {0, 0, 0};
            for (int dy = 0; dy < factor; ++dy)
                for (int dx = 0; dx < factor; ++dx) {
                    const unsigned char* p = rgba + size_t(y * factor + dy) * size_t(stride) + size_t(x * factor + dx) * 4;
                    sum[0] += p[0];
                    sum[1] += p[1];
                    sum[2] += p[2];
                }
            unsigned char* q = reinterpret_cast<unsigned char*>(&m.data[(size_t(y) * size_t(m.width) + size_t(x)) * 3]);
            for (int c = 0; c < 3; ++c)
                q[c] = static_cast<unsigned char>(sum[c] / unsigned(factor * factor));
        }
    return m;
}

// The part of a rectangle a region names ("top", "bottom-left", "center", ...): x0, y0, x1, y1 as
// fractions. Unknown names mean all of it.
void regionFractions(const std::string& region, float f[4])
{
    f[0] = 0.0f, f[1] = 0.0f, f[2] = 1.0f, f[3] = 1.0f;
    const std::string r = lower(region);
    if (r.find("top") != std::string::npos)
        f[3] = 0.5f;
    if (r.find("bottom") != std::string::npos)
        f[1] = 0.5f;
    if (r.find("left") != std::string::npos)
        f[2] = 0.5f;
    if (r.find("right") != std::string::npos)
        f[0] = 0.5f;
    if (r == "center" || r == "middle")
        f[0] = 0.25f, f[1] = 0.25f, f[2] = 0.75f, f[3] = 0.75f;
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
}

std::vector<LocalTool> LocalAgentTools::tools(bool canEdit) const
{
    const char* detail = "How carefully to look: low (fast, ~70 tokens), normal (~280), high (small print, OCR: ~1100)";
    std::vector<LocalTool> t = {
        // The folder.
        {"list_files", "Lists a folder of the open folder: sub-folders (ending in /) and files with their sizes.",
         schema({{"path", "string", "Folder, relative to the open folder; empty for the open folder itself"}})},
        {"read_file", "Reads a text file (code, notes, data), its lines numbered. Long files: ask for a range of lines.",
         schema({{"path", "string", "File, relative to the open folder"},
                 {"start_line", "integer", "First line, from 1 (default 1)"},
                 {"end_line", "integer", "Last line (default: 200 lines on)"}},
                {"path"})},
        {"search_files", "Finds text in every text file of the open folder: each match's file, line and text.",
         schema({{"query", "string", "The text to find"}, {"include", "string", "Only these files, e.g. *.cpp;*.h (optional)"}},
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
        {"look_at_page", "Shows a page of a PDF, Word or PowerPoint document in the viewer and looks at it as a picture: "
                         "for tables, charts, drawings, layout and scanned pages.",
         schema({{"path", "string", "Document, relative to the open folder (default: the one shown)"},
                 {"page", "integer", "Page or slide, from 1 (default 1)"},
                 {"zoom", "integer", "Percent, e.g. 100, 200, 400; 0 fits the page width (default 0)"},
                 {"region", "string", "all, top, bottom, left, right, top-left, top-right, bottom-left, bottom-right or center"},
                 {"detail", "string", detail}})},
        {"look_at_image", "Looks at a picture file (PNG, JPEG, GIF, BMP, WebP, TIFF and more).",
         schema({{"path", "string", "Picture, relative to the open folder"}, {"detail", "string", detail}}, {"path"})},
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
                      "sin cos tan, pi, e. Use it for every sum, total, percentage or conversion.",
         schema({{"expression", "string", "e.g. 15% of 2340 + 7.5% of 980, (1200 * 1.08) - 50, round(17.5 / 3, 2)"}}, {"expression"})},
        {"query_table", "Filters, sorts and totals the rows of a CSV file or an Excel sheet. Totals per group: group_by and "
                        "aggregate. Which group has the most: group_by Country, aggregate sum(Profit), sort 'sum(Profit) desc', "
                        "limit 1. Call it again for another grouping.",
         schema({{"path", "string", "CSV, TSV or Excel file, relative to the open folder"},
                 {"sheet", "string", "Excel: the sheet's name (default the first)"},
                 {"where", "string", "Conditions joined by 'and', e.g. Country = Canada and Units Sold > 1000; also != >= <= contains"},
                 {"columns", "string", "Columns to show, comma-separated (default all)"},
                 {"group_by", "string", "A column to group by (optional)"},
                 {"aggregate", "string", "count, or sum/avg/min/max of a column, comma-separated, e.g. sum(Profit), count"},
                 {"sort", "string", "A column, or with group_by an aggregate such as sum(Profit); add ' desc' for largest first"},
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
    g += vm_.localSetup().projectorPath.empty()
             ? "You cannot see pictures or hear recordings yet: the user can download your eyes and ears in the agent "
               "set-up (the gear). Use read_document for documents meanwhile.\n"
             : "You can see pictures and pages (look_at_image, look_at_page) and hear recordings (listen_to).\n";
    g += "Which tool for which file:\n"
         "- code, notes (.cpp .py .md .txt .json ...): read_file; find text in them with search_files\n"
         "- tables (.csv .tsv .xlsx): query_table to filter, sort, group and total rows; read_document for an overview\n"
         "- documents (.pdf .docx .xlsx .pptx): read_document for the text; look_at_page to see a page (tables, charts)\n"
         "- pictures (.png .jpg .gif .webp ...): look_at_image\n"
         "- recordings (.wav .mp3 .flac): listen_to for a short one; transcribe for a meeting or anything long\n"
         "- programs and libraries (.exe .dll .so .o .a .lib): inspect_binary; read_bytes for raw bytes of any file\n"
         "- a folder: list_files\n"
         "Any arithmetic: calculate (never do sums in your head).\n"
         "Call a tool as name {\"argument\": value}. If a tool says the call was wrong, read its example and call it again.\n";
    g += memorySummary();
    // The skills: their names and what each is for (resources/skills/<name>/SKILL.md).
    std::string skills;
    if (const std::optional<std::string> index = os::fs::readAsset("skills/index.json"))
        if (const std::optional<Json> j = Json::parse(*index); j && j->isArray())
            for (const Json& s : j->items())
                skills += "- " + s.getString("name") + ": " + s.getString("description") + "\n";
    if (!skills.empty())
        g += "Your skills (read_skill <name> before a task of that kind):\n" + skills;
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
    const Json args = std::move(*parsed);

    using Handler = void (LocalAgentTools::*)(const Json&, Done);
    static const std::vector<std::pair<const char*, Handler>> handlers = {
        {"list_files", &LocalAgentTools::listFiles},       {"read_file", &LocalAgentTools::readFile},
        {"search_files", &LocalAgentTools::searchFiles},   {"read_document", &LocalAgentTools::readDocument},
        {"look_at_page", &LocalAgentTools::lookAtPage},    {"look_at_image", &LocalAgentTools::lookAtImage},
        {"listen_to", &LocalAgentTools::listenTo},         {"transcribe", &LocalAgentTools::transcribe},
        {"inspect_binary", &LocalAgentTools::inspectBinary}, {"read_bytes", &LocalAgentTools::readBytes},
        {"open_file", &LocalAgentTools::openFile},         {"calculate", &LocalAgentTools::calculate},
        {"query_table", &LocalAgentTools::queryTable},     {"remember", &LocalAgentTools::remember},
        {"recall", &LocalAgentTools::recall},              {"amend_memory", &LocalAgentTools::amendMemory},
        {"forget_memory", &LocalAgentTools::forgetMemory}, {"rewrite_memories", &LocalAgentTools::rewriteMemories},
        {"edit_file", &LocalAgentTools::editFile},         {"write_file", &LocalAgentTools::writeFile},
        {"read_skill", &LocalAgentTools::readSkill},
        {"show_image", &LocalAgentTools::showImage},
    };
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
    for (const os::fs::DirEntry& e : entries) {
        if (e.name == ".git")
            continue;
        out += e.isDirectory ? e.name + "/\n" : e.name + "  (" + std::to_string(e.size) + " bytes)\n";
    }
    if (more)
        out += "[more entries not listed]\n";
    reply(done, out);
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
        return reply(done, path + " is not a text file. " + whichTool(path));
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
    for (int i = first; i <= last; ++i) {
        std::string line = lines[size_t(i - 1)];
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        out += std::to_string(i) + "| " + line + "\n";
    }
    reply(done, out);
}

void LocalAgentTools::searchFiles(const Json& args, Done done)
{
    Json params = Json::object().set("query", args.getString("query"));
    if (!args.getString("include").empty())
        params.set("include", args.getString("include"));
    const Json result = handleAutomation(vm_, "search_files", params);
    if (!result.getString("error").empty())
        return reply(done, "search failed: " + result.getString("error"));
    std::string out;
    int count = 0;
    if (const Json* matches = result.find("results"))
        for (const Json& m : matches->items()) {
            if (++count > 60) {
                out += "[more matches not listed: narrow the search]\n";
                break;
            }
            out += m.getString("path") + ":" + std::to_string(int(m.getNumber("line"))) + ": " + m.getString("text") + "\n";
        }
    reply(done, count == 0 ? "No matches for '" + args.getString("query") + "'." : out);
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
    os::dispatch::runAsync([alive, absolute, path, from, to, done = std::move(done)]() mutable {
        std::string out;
        std::optional<std::string> bytes = os::fs::readFile(absolute);
        if (!bytes) {
            out = "cannot read " + path;
        } else if (model::viewer::viewerFormat(absolute, *bytes).empty()) {
            out = path + " is not a PDF, Word, Excel or PowerPoint document. " + whichTool(path);
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
                for (int i = from; i <= last; ++i) {
                    doc.pages.load(size_t(i - 1));
                    out += "\n--- Page " + std::to_string(i) + " ---\n" + model::viewer::pageText(doc.pages.pages[size_t(i - 1)]);
                    doc.pages.unload(size_t(i - 1));
                }
                if (from > count)
                    out += "There is no page " + std::to_string(from) + ".\n";
                else if (last < count)
                    out += "\n[pages " + std::to_string(last + 1) + "-" + std::to_string(count) + " not read]\n";
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
    const int page = std::max(1, argInt(args, "page", 1));
    const int zoomPercent = std::clamp(argInt(args, "zoom", 0), 0, 800);
    const bool shown = view->editor().viewerView().showPage(doc->id, page - 1, zoomPercent / 100.0f);
    Look look;
    look.done = std::move(done);
    look.region = args.getString("region", "all");
    look.side = sideFor(args.getString("detail"));
    look.what = (shown ? "page " + std::to_string(page) + " of " : std::string()) + fileName(path) +
                (zoomPercent > 0 ? " at " + std::to_string(zoomPercent) + "%" : std::string()) +
                (look.region == "all" || look.region.empty() ? std::string() : ", " + look.region);
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
        // The document's area, in the capture's pixels (it may be larger than ImGui's units).
        const view::UiElement* area = view::UiAutomation::instance().find("document-area");
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        const float sx = display.x > 0 ? float(width) / display.x : 1.0f;
        const float sy = display.y > 0 ? float(height) / display.y : 1.0f;
        float x0 = 0, y0 = 0, x1 = float(width), y1 = float(height);
        if (area) {
            x0 = area->min.x * sx, y0 = area->min.y * sy, x1 = area->max.x * sx, y1 = area->max.y * sy;
        }
        float f[4];
        regionFractions(l.region, f);
        const int left = std::clamp(int(x0 + (x1 - x0) * f[0]), 0, width - 1);
        const int top = std::clamp(int(y0 + (y1 - y0) * f[1]), 0, height - 1);
        const int right = std::clamp(int(x0 + (x1 - x0) * f[2]), left + 1, width);
        const int bottom = std::clamp(int(y0 + (y1 - y0) * f[3]), top + 1, height);
        Output out;
        out.media.push_back(picture(l.what, rgba.data() + (size_t(top) * size_t(width) + size_t(left)) * 4, right - left,
                                    bottom - top, width * 4, l.side));
        out.text = area ? "Looked at " + l.what + " (as shown in the viewer)."
                        : "The document is not shown in the viewer, so this is the whole window.";
        if (const std::string saved = savePicture(out.media.back(), "page"); !saved.empty())
            out.shown.push_back(saved); // the user sees the part it looked at
        l.done(std::move(out));
    }
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
    services::DecodedImage image;
    std::string note;
    const int side = sideFor(args.getString("detail"));
    if (!services::decodeImage(*bytes, image, note, side * 2))
        return reply(done, kindOf(path) != FileKind::Picture ? path + " is not a picture. " + whichTool(path)
                                                             : path + " is a picture this cannot read: " + note);
    Output out;
    out.media.push_back(picture(fileName(path), image.rgba.data(), image.width, image.height, image.width * 4, side));
    out.text = "Looked at " + path + " (" + image.format + ", " + std::to_string(image.sourceWidth) + " x " +
               std::to_string(image.sourceHeight) + " pixels).";
    out.shown.push_back(absolute); // the user sees what it looked at
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
        return reply(done, kindOf(path) != FileKind::Sound ? path + " is not a recording. " + whichTool(path)
                                                           : path + " is a recording this cannot play: WAV, MP3 and FLAC can be heard.");
    const os::fs::FileInfo st = os::fs::stat(absolute);
    if (!st.exists)
        return reply(done, path + " does not exist");
    if (st.size > kSoundBytes)
        return reply(done, path + " is too long to listen to whole (" + std::to_string(st.size >> 20) + " MB; up to 25 MB).");
    std::optional<std::string> bytes = os::fs::readFile(absolute);
    if (!bytes)
        return reply(done, "cannot read " + path);
    Output out;
    LocalMedia sound;
    sound.kind = LocalMedia::Kind::Audio;
    sound.name = fileName(path);
    sound.data = std::move(*bytes);
    out.media.push_back(std::move(sound));
    out.text = "Listened to " + path + ".";
    done(std::move(out));
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

void LocalAgentTools::calculate(const Json& args, Done done)
{
    const std::string expression = args.getString("expression");
    const std::string cleaned = plainNumbers(expression);
    Calculator calc;
    calc.s = cleaned;
    const double v = calc.sum();
    calc.space();
    if (calc.error.empty() && calc.i < calc.s.size())
        calc.error = std::string("unexpected '") + calc.s[calc.i] + "'";
    if (!calc.error.empty())
        return reply(done, "Cannot calculate '" + expression + "': " + calc.error + ". " + usage("calculate"));
    reply(done, expression + " = " + numberText(v));
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
        std::string names;
        for (const model::viewer::Sheet& s : doc.book.sheets) {
            names += (names.empty() ? "" : ", ") + s.name;
            if (!args.getString("sheet").empty() && lower(s.name) == lower(args.getString("sheet")))
                sheet = &s;
        }
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
        return reply(done, path + " is not a table. " + whichTool(path));
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
    auto unknown = [&](const std::string& name) { return "There is no column '" + name + "'. The columns are: " + columns; };
    auto cell = [](const std::vector<std::string>& row, int c) { return size_t(c) < row.size() ? trimmed(row[size_t(c)]) : std::string(); };

    // where: "Country = Canada and Units Sold > 1000"
    struct Condition {
        int column;
        std::string op, value;
    };
    std::vector<Condition> conditions;
    const std::string where = args.getString("where");
    for (size_t from = 0; from < where.size();) {
        const size_t cut = lower(where).find(" and ", from);
        const std::string part = where.substr(from, cut == std::string::npos ? std::string::npos : cut - from);
        from = cut == std::string::npos ? where.size() : cut + 5;
        static const char* const ops[] = {" contains ", ">=", "<=", "!=", "<>", "=", ">", "<"};
        bool parsed = false;
        for (const char* op : ops) {
            const size_t at = lower(part).find(op);
            if (at == std::string::npos)
                continue;
            const int c = column(part.substr(0, at));
            if (c < 0)
                return reply(done, unknown(trimmed(part.substr(0, at))));
            conditions.push_back({c, trimmed(op), trimmed(part.substr(at + std::strlen(op)))});
            parsed = true;
            break;
        }
        if (!parsed && !trimmed(part).empty())
            return reply(done, "Cannot read the condition '" + part + "': write column = value, column > 10 or column contains text.");
    }
    auto matches = [&](const std::vector<std::string>& row) {
        for (const Condition& k : conditions) {
            const std::string value = cell(row, k.column);
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
    std::string out = note + path + ": " + std::to_string(picked.size()) + " of " + std::to_string(rows.size()) +
                      " rows match. Columns: " + columns + "\n";
    const int limit = std::clamp(argInt(args, "limit", 30), 1, 500);

    // Totals, by group when asked.
    if (const std::vector<std::string> aggregates = splitList(args.getString("aggregate")); !aggregates.empty()) {
        struct Agg {
            std::string fn, label;
            int column = -1;
        };
        std::vector<Agg> aggs;
        for (const std::string& a : aggregates) {
            Agg g;
            g.label = a;
            const size_t open = a.find('(');
            g.fn = lower(trimmed(open == std::string::npos ? a : a.substr(0, open)));
            if (open != std::string::npos) {
                const size_t close = a.find(')', open);
                const std::string name = a.substr(open + 1, close == std::string::npos ? std::string::npos : close - open - 1);
                g.column = column(name);
                if (g.column < 0)
                    return reply(done, unknown(trimmed(name)));
            }
            if (g.fn == "average" || g.fn == "mean")
                g.fn = "avg";
            if (g.fn != "count" && (g.column < 0 || (g.fn != "sum" && g.fn != "avg" && g.fn != "min" && g.fn != "max")))
                return reply(done, "Write each aggregate as sum(column), avg(column), min(column), max(column) or count.");
            aggs.push_back(g);
        }
        const std::string groupName = args.getString("group_by");
        const int group = groupName.empty() ? -1 : column(groupName);
        if (!groupName.empty() && group < 0)
            return reply(done, unknown(groupName));
        std::vector<std::string> order;
        std::map<std::string, std::vector<const std::vector<std::string>*>> groups;
        for (const auto* row : picked) {
            const std::string key = group >= 0 ? cell(*row, group) : std::string("all rows");
            if (!groups.count(key))
                order.push_back(key);
            groups[key].push_back(row);
        }
        out += group >= 0 ? trimmed(header[size_t(group)]) : std::string("rows");
        for (const Agg& g : aggs)
            out += " | " + g.label;
        out += "\n";
        // Each group's values first, so the groups can be sorted by one ("sum(Profit) desc").
        std::vector<std::pair<std::string, std::vector<double>>> results;
        for (const std::string& key : order) {
            std::vector<double> values;
            for (const Agg& g : aggs) {
                double total = 0, low = 0, high = 0;
                int count = 0;
                for (const auto* row : groups[key]) {
                    double v = 0;
                    if (g.fn == "count") {
                        ++count;
                    } else if (cellNumber(cell(*row, g.column), v)) {
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
            else if (group >= 0 && column(sort) == group)
                std::stable_sort(results.begin(), results.end(), [&](const auto& a, const auto& b) {
                    return descending ? lower(a.first) > lower(b.first) : lower(a.first) < lower(b.first);
                });
            else
                return reply(done, "Sort the groups by one of: " + (group >= 0 ? trimmed(header[size_t(group)]) + ", " : std::string()) +
                                       args.getString("aggregate") + " (add desc for largest first).");
        }
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
        return reply(done, path + " is not a recording. " + whichTool(path));
    const viewmodel::AgentViewModel::LocalSetup setup = vm_.localSetup();
    if (!engine_ || !setup.ready || setup.projectorPath.empty())
        return reply(done, "Transcribing needs the local model's ears: the user can download them in the agent set-up.");
    const os::fs::FileInfo info = os::fs::stat(absolute);
    if (!info.exists)
        return reply(done, path + " does not exist");
    if (info.size > (200ull << 20))
        return reply(done, path + " is too large to transcribe here (over 200 MB).");
    std::optional<std::string> bytes = os::fs::readFile(absolute);
    if (!bytes)
        return reply(done, "cannot read " + path);
    // Where the text goes: next to the recording, never over another file. Always word for word: a
    // small model given a say put its summary in place of the words.
    std::string target = absolute.substr(0, absolute.find_last_of('.')) + ".transcript.md";
    const std::string base = target;
    for (int n = 2; os::fs::exists(target) && n < 100; ++n) {
        const size_t dot = base.find_last_of('.');
        target = base.substr(0, dot) + " (" + std::to_string(n) + ")" + base.substr(dot);
    }
    interfaces::LocalChatRequest request;
    request.modelPath = setup.modelPath;
    request.drafterPath = setup.drafterPath;
    request.projectorPath = setup.projectorPath;
    request.threads = setup.threads;
    request.useGpu = setup.gpu;
    request.contextTokens = 0; // as the agent's chats: the model stays loaded
    request.maxTokens = 8192;
    request.transcribe = true;
    interfaces::LocalChatMessage message;
    message.role = "user";
    message.text.clear(); // the engine's own instruction: the words as spoken
    LocalMedia sound;
    sound.kind = LocalMedia::Kind::Audio;
    sound.name = fileName(path);
    sound.data = std::move(*bytes);
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
        [](const std::string&) {},
        [alive, vm, path, target, done = std::move(done)](const interfaces::LocalChatResult& r) {
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
                            "The text:\n" + r.text);
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
        return reply(done, path + " is not a picture. " + whichTool(path));
    Output out;
    out.text = "Shown " + fileName(path) + " to the user.";
    out.shown.push_back(absolute);
    done(std::move(out));
}

} // namespace enp::app
