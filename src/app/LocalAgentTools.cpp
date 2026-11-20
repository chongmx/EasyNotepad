#include "LocalAgentTools.h"

#include "AutomationController.h"

#include "enp/model/viewer/Viewer.h"
#include "enp/os/Dispatch.h"
#include "enp/os/Fs.h"
#include "enp/os/Memory.h"
#include "enp/services/ImageDecoder.h"
#include "enp/view/MainView.h"
#include "enp/view/UiAutomation.h"
#include "enp/viewmodel/FileDialogViewModel.h"
#include "enp/viewmodel/NotepadViewModel.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cmath>

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
constexpr int kLookSide = 1024;          // a picture's longest side, in pixels, as the model sees it
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

// RGBA (as decoded or captured) to RGB, shrunk (area-averaged by an integer factor) so the longest
// side is at most kLookSide.
LocalMedia picture(const std::string& name, const unsigned char* rgba, int width, int height, int stride)
{
    const int factor = std::max(1, int(std::ceil(double(std::max(width, height)) / kLookSide)));
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

void reply(const LocalAgentTools::Done& done, std::string text) { done(Output{std::move(text), {}}); }

} // namespace

LocalAgentTools::LocalAgentTools(viewmodel::NotepadViewModel& vm, std::function<view::MainView*()> view)
    : vm_(vm), view_(std::move(view))
{
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
}

std::vector<LocalTool> LocalAgentTools::tools(bool canEdit) const
{
    std::vector<LocalTool> t = {
        {"list_files", "Lists a folder of the user's open folder: sub-folders (ending in /) and files with their sizes.",
         schema({{"path", "string", "Folder, relative to the open folder; empty for the open folder itself"}})},
        {"read_file", "Reads a text file (code, notes, data), its lines numbered. An open file is read as the user has it, "
                      "unsaved edits included. Long files: ask for a range of lines.",
         schema({{"path", "string", "File, relative to the open folder"},
                 {"start_line", "integer", "First line, from 1 (default 1)"},
                 {"end_line", "integer", "Last line (default: 200 lines on)"}},
                {"path"})},
        {"search_files", "Finds text in every file of the open folder: each match's file, line and text.",
         schema({{"query", "string", "The text to find"}, {"include", "string", "Only these files, e.g. *.cpp;*.h (optional)"}},
                {"query"})},
        {"read_document", "Reads the text of a PDF, Word, Excel or PowerPoint file, a few pages at a time, and says how many "
                          "pages it has.",
         schema({{"path", "string", "Document, relative to the open folder"},
                 {"from_page", "integer", "First page or slide, from 1 (default 1); for Excel and reflowed Word, the part"},
                 {"to_page", "integer", "Last page (default: 3 pages on)"}},
                {"path"})},
        {"look_at_page", "Shows a page of a PDF, Word or PowerPoint document in the viewer and looks at it as a picture: "
                         "for tables, charts, drawings, layout and scanned pages. Zoom in on a region for small print.",
         schema({{"path", "string", "Document, relative to the open folder (default: the one shown)"},
                 {"page", "integer", "Page or slide, from 1 (default 1)"},
                 {"zoom", "integer", "Percent, e.g. 100, 200, 400; 0 fits the page width (default 0)"},
                 {"region", "string", "all, top, bottom, left, right, top-left, top-right, bottom-left, bottom-right or center"}})},
        {"look_at_image", "Looks at a picture file (PNG, JPEG, GIF, BMP, WebP, TIFF and more).",
         schema({{"path", "string", "Picture, relative to the open folder"}}, {"path"})},
        {"listen_to", "Listens to a recording (WAV, MP3 or FLAC): speech, music, sounds.",
         schema({{"path", "string", "Recording, relative to the open folder"}}, {"path"})},
        {"open_file", "Opens a file in the editor for the user to see, optionally at a line (documents open in the viewer).",
         schema({{"path", "string", "File, relative to the open folder"}, {"line", "integer", "Line to show, from 1 (optional)"}},
                {"path"})},
        {"read_skill", "Reads one of your skills: how to do a kind of task well with these tools. Read it before such a task.",
         schema({{"name", "string", "The skill's name, from the list in your guide"}}, {"name"})},
    };
    if (canEdit) {
        t.push_back({"edit_file",
                     "Proposes a change to a text file: `old_text` (exactly as in the file, found once) becomes `new_text`. "
                     "The user reviews it in the agent panel and accepts or rejects it.",
                     schema({{"path", "string", "File, relative to the open folder"},
                             {"old_text", "string", "The exact text to replace; enough lines to be found only once"},
                             {"new_text", "string", "What replaces it"}},
                            {"path", "old_text", "new_text"})});
        t.push_back({"write_file",
                     "Proposes a new file, or new contents for a whole file. The user reviews it in the agent panel and "
                     "accepts or rejects it.",
                     schema({{"path", "string", "File, relative to the open folder"}, {"content", "string", "The whole text"}},
                            {"path", "content"})});
    }
    return t;
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
         "- code, notes, data (.cpp .py .md .txt .json .csv ...): read_file; find text in them with search_files\n"
         "- documents (.pdf .docx .xlsx .pptx): read_document for the text; look_at_page to see a page (tables, charts)\n"
         "- pictures (.png .jpg .gif .webp ...): look_at_image\n"
         "- recordings (.wav .mp3 .flac): listen_to\n"
         "- a folder: list_files\n";
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
    std::optional<Json> parsed = Json::parse(call.arguments.empty() ? std::string("{}") : call.arguments);
    const Json args = parsed && parsed->isObject() ? std::move(*parsed) : Json::object();
    using Handler = void (LocalAgentTools::*)(const Json&, Done);
    static const std::vector<std::pair<const char*, Handler>> handlers = {
        {"list_files", &LocalAgentTools::listFiles},     {"read_file", &LocalAgentTools::readFile},
        {"search_files", &LocalAgentTools::searchFiles}, {"read_document", &LocalAgentTools::readDocument},
        {"look_at_page", &LocalAgentTools::lookAtPage},  {"look_at_image", &LocalAgentTools::lookAtImage},
        {"listen_to", &LocalAgentTools::listenTo},       {"open_file", &LocalAgentTools::openFile},
        {"edit_file", &LocalAgentTools::editFile},       {"write_file", &LocalAgentTools::writeFile},
        {"read_skill", &LocalAgentTools::readSkill},
    };
    for (const auto& [name, handler] : handlers)
        if (call.name == name) {
            (this->*handler)(args, std::move(done));
            return;
        }
    reply(done, "There is no tool called '" + call.name + "'.");
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
        const view::UiElement* area = view::UiAutomation::instance().find("viewer:document");
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
                                    bottom - top, width * 4));
        out.text = area ? "Looked at " + l.what + " (as shown in the viewer)."
                        : "The document is not shown in the viewer, so this is the whole window.";
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
    if (!services::decodeImage(*bytes, image, note, kLookSide * 2))
        return reply(done, kindOf(path) != FileKind::Picture ? path + " is not a picture. " + whichTool(path)
                                                             : path + " is a picture this cannot read: " + note);
    Output out;
    out.media.push_back(picture(fileName(path), image.rgba.data(), image.width, image.height, image.width * 4));
    out.text = "Looked at " + path + " (" + image.format + ", " + std::to_string(image.sourceWidth) + " x " +
               std::to_string(image.sourceHeight) + " pixels).";
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

} // namespace enp::app
