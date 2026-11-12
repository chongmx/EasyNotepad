#include "enp/view/DocumentFonts.h"

#include "enp/model/package/Zip.h"
#include "enp/os/Memory.h"
#include "enp/os/Os.h"
#include "enp/view/Fonts.h"

#include "imgui_internal.h" // ImTextCharFromUtf8

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>

namespace enp::view {

namespace {

std::string lower(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        out += char(std::tolower(static_cast<unsigned char>(c)));
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

bool has(const std::string& s, const char* part)
{
    return s.find(part) != std::string::npos;
}

// The bundled family with the same metrics as `family` (by name), else the nearest kind: fixed
// pitch, serif or sans. The pack's files are <name>-<Regular|Bold|Italic|BoldItalic>.ttf.
const char* bundledFamily(const std::string& f)
{
    if (f == "calibri" || f == "calibri light" || f == "carlito")
        return "EnpDocSansC";
    if (f.rfind("cambria", 0) == 0 || f == "caladea")
        return "EnpDocSerifC";
    if (has(f, "courier") || has(f, "consol") || has(f, "mono") || has(f, "lucida console") || has(f, "menlo") || f == "cousine")
        return "EnpDocMono";
    if ((has(f, "serif") && !has(f, "sans")) || has(f, "times") || has(f, "roman") || has(f, "georgia") || has(f, "garamond") ||
        has(f, "antiqua") || has(f, "palatino") || has(f, "schoolbook") || has(f, "bookman") || f == "tinos" || has(f, "minion"))
        return "EnpDocSerif";
    return "EnpDocSans"; // Arial, Helvetica, Verdana, Segoe UI, Tahoma, Aptos, ...
}

const char* const kStyles[4] = {"Regular", "Bold", "Italic", "BoldItalic"};

// A font file's single line height in ems, as Word spaces lines: hhea's ascender, descender and
// line gap (else OS/2's Windows ascent and descent). The first face of a collection (.ttc).
float lineHeightOf(const unsigned char* p, std::size_t n)
{
    auto u16 = [&](std::size_t o) { return o + 2 <= n ? unsigned(p[o]) << 8 | p[o + 1] : 0u; };
    auto u32 = [&](std::size_t o) { return o + 4 <= n ? (std::uint32_t(u16(o)) << 16) | u16(o + 2) : 0u; };
    auto s16 = [&](std::size_t o) { return int(std::int16_t(u16(o))); };
    std::size_t base = 0;
    if (n >= 16 && std::memcmp(p, "ttcf", 4) == 0)
        base = u32(12);
    const unsigned tables = u16(base + 4);
    std::size_t head = 0, hhea = 0, os2 = 0;
    for (unsigned i = 0; i < tables && i < 64; ++i) {
        const std::size_t rec = base + 12 + 16 * std::size_t(i);
        if (rec + 16 > n)
            break;
        const std::uint32_t at = u32(rec + 8);
        if (std::memcmp(p + rec, "head", 4) == 0)
            head = at;
        else if (std::memcmp(p + rec, "hhea", 4) == 0)
            hhea = at;
        else if (std::memcmp(p + rec, "OS/2", 4) == 0)
            os2 = at;
    }
    const unsigned em = head ? u16(head + 18) : 0;
    if (em < 16)
        return 1.17f;
    int units = 0;
    if (hhea)
        units = s16(hhea + 4) - s16(hhea + 6) + s16(hhea + 8);
    if (units <= 0 && os2)
        units = int(u16(os2 + 74) + u16(os2 + 76));
    return units > 0 ? std::clamp(float(units) / float(em), 0.8f, 2.0f) : 1.17f;
}

// Families the system has under their own names, by style (regular, bold, italic, bold italic).
struct SystemFamily {
    const char* family;
    const char* dir;
    const char* files[4];
};
#if defined(_WIN32)
#define ENP_WINFONTS "C:/Windows/Fonts/"
const SystemFamily kSystem[] = {
    {"calibri", ENP_WINFONTS, {"calibri.ttf", "calibrib.ttf", "calibrii.ttf", "calibriz.ttf"}},
    {"calibri light", ENP_WINFONTS, {"calibril.ttf", nullptr, "calibrili.ttf", nullptr}},
    {"cambria", ENP_WINFONTS, {"cambria.ttc", "cambriab.ttf", "cambriai.ttf", "cambriaz.ttf"}},
    {"arial", ENP_WINFONTS, {"arial.ttf", "arialbd.ttf", "ariali.ttf", "arialbi.ttf"}},
    {"times new roman", ENP_WINFONTS, {"times.ttf", "timesbd.ttf", "timesi.ttf", "timesbi.ttf"}},
    {"courier new", ENP_WINFONTS, {"cour.ttf", "courbd.ttf", "couri.ttf", "courbi.ttf"}},
    {"georgia", ENP_WINFONTS, {"georgia.ttf", "georgiab.ttf", "georgiai.ttf", "georgiaz.ttf"}},
    {"verdana", ENP_WINFONTS, {"verdana.ttf", "verdanab.ttf", "verdanai.ttf", "verdanaz.ttf"}},
    {"tahoma", ENP_WINFONTS, {"tahoma.ttf", "tahomabd.ttf", nullptr, nullptr}},
    {"segoe ui", ENP_WINFONTS, {"segoeui.ttf", "segoeuib.ttf", "segoeuii.ttf", "segoeuiz.ttf"}},
    {"consolas", ENP_WINFONTS, {"consola.ttf", "consolab.ttf", "consolai.ttf", "consolaz.ttf"}},
    {"trebuchet ms", ENP_WINFONTS, {"trebuc.ttf", "trebucbd.ttf", "trebucit.ttf", "trebucbi.ttf"}},
    {"garamond", ENP_WINFONTS, {"GARA.TTF", "GARABD.TTF", "GARAIT.TTF", nullptr}},
    {"century gothic", ENP_WINFONTS, {"GOTHIC.TTF", "GOTHICB.TTF", "GOTHICI.TTF", "GOTHICBI.TTF"}},
    {"book antiqua", ENP_WINFONTS, {"BKANT.TTF", "ANTQUAB.TTF", "ANTQUAI.TTF", "ANTQUABI.TTF"}},
};
#elif defined(__APPLE__)
#define ENP_MACFONTS "/System/Library/Fonts/Supplemental/"
#define ENP_WORDFONTS "/Applications/Microsoft Word.app/Contents/Resources/DFonts/"
const SystemFamily kSystem[] = {
    {"calibri", ENP_WORDFONTS, {"Calibri.ttf", "Calibri Bold.ttf", "Calibri Italic.ttf", "Calibri Bold Italic.ttf"}},
    {"cambria", ENP_WORDFONTS, {"Cambria.ttc", "Cambria Bold.ttf", "Cambria Italic.ttf", "Cambria Bold Italic.ttf"}},
    {"arial", ENP_MACFONTS, {"Arial.ttf", "Arial Bold.ttf", "Arial Italic.ttf", "Arial Bold Italic.ttf"}},
    {"times new roman", ENP_MACFONTS, {"Times New Roman.ttf", "Times New Roman Bold.ttf", "Times New Roman Italic.ttf", "Times New Roman Bold Italic.ttf"}},
    {"courier new", ENP_MACFONTS, {"Courier New.ttf", "Courier New Bold.ttf", "Courier New Italic.ttf", "Courier New Bold Italic.ttf"}},
    {"georgia", ENP_MACFONTS, {"Georgia.ttf", "Georgia Bold.ttf", "Georgia Italic.ttf", "Georgia Bold Italic.ttf"}},
    {"verdana", ENP_MACFONTS, {"Verdana.ttf", "Verdana Bold.ttf", "Verdana Italic.ttf", "Verdana Bold Italic.ttf"}},
    {"tahoma", ENP_MACFONTS, {"Tahoma.ttf", "Tahoma Bold.ttf", nullptr, nullptr}},
    {"trebuchet ms", ENP_MACFONTS, {"Trebuchet MS.ttf", "Trebuchet MS Bold.ttf", "Trebuchet MS Italic.ttf", "Trebuchet MS Bold Italic.ttf"}},
};
#else
// Linux, Android, the web: the bundled fonts (the same metrics) for every family.
const SystemFamily* const kSystem = nullptr;
#endif

} // namespace

DocumentFonts::~DocumentFonts()
{
    if (ImGui::GetCurrentContext())
        for (const std::unique_ptr<Loaded>& l : loaded_)
            if (l->font)
                ImGui::GetIO().Fonts->RemoveFont(l->font);
}

DocumentFonts::Face DocumentFonts::face(std::string_view family, bool bold, bool italic)
{
    const int style = (bold ? 1 : 0) | (italic ? 2 : 0);
    const double now = ImGui::GetTime();
    for (Request& r : requests_)
        if (r.style == style && r.family == family) {
            if (r.source)
                r.source->lastUsed = now;
            return r.face;
        }
    // The first time: the family itself from the system in this style (else its regular face,
    // weight and slant faked), else the bundled face with its metrics.
    Request request;
    request.family = std::string(family);
    request.style = style;
    const std::string f = lower(family);
#if defined(_WIN32) || defined(__APPLE__)
    for (const SystemFamily& s : kSystem) {
        if (f != s.family)
            continue;
        for (const int st : {style, 0}) {
            if (!s.files[st])
                continue;
            if (Loaded* l = load(std::string(s.dir) + s.files[st])) {
                request.source = l;
                request.face = Face{l->font, (st & 1) != 0, (st & 2) != 0, l->lineHeight};
                break;
            }
        }
        break;
    }
#else
    (void)kSystem;
#endif
    // Any other installed family (Cooper Black, Corbel, ...), in this style or else regular.
    if (!request.source)
        for (const int st : {style, 0}) {
            const std::string path = os::fs::installedFontFile(family, (st & 1) != 0, (st & 2) != 0);
            if (Loaded* l = path.empty() ? nullptr : load(path)) {
                request.source = l;
                request.face = Face{l->font, (st & 1) != 0, (st & 2) != 0, l->lineHeight};
                break;
            }
        }
    if (!request.source) {
        // A heavy family (Cooper Black, Arial Black, Franklin Gothic Heavy) stands in as the bold face.
        const bool heavy = has(f, "black") || has(f, "heavy") || has(f, "ultra");
        const int packStyle = style | (heavy ? 1 : 0);
        if (Loaded* l = load(std::string("pack:") + bundledFamily(f) + "-" + kStyles[packStyle] + ".ttf")) {
            request.source = l;
            request.face = Face{l->font, bold || heavy, italic, l->lineHeight};
        }
    }
    if (request.source)
        request.source->lastUsed = now;
    os::mem::Scope scope("fonts");
    requests_.push_back(std::move(request));
    return requests_.back().face;
}

DocumentFonts::Loaded* DocumentFonts::load(const std::string& key)
{
    for (const std::unique_ptr<Loaded>& l : loaded_)
        if (l->key == key)
            return l.get();
    if (std::find(missing_.begin(), missing_.end(), key) != missing_.end())
        return nullptr;
    os::mem::Scope scope("fonts");
    auto l = std::make_unique<Loaded>();
    l->key = key;
    const void* bytes = nullptr;
    std::size_t size = 0;
    if (key.rfind("pack:", 0) == 0) {
        // The pack is read for this face and let go: the face's bytes alone are kept.
        if (std::optional<std::string> pack = os::fs::readAsset("fonts/document-fonts.zip")) {
            model::package::ZipArchive zip;
            if (zip.open(std::move(*pack), nullptr))
                if (std::optional<std::string> data = zip.read(key.substr(5), std::size_t(4) << 20))
                    l->data = std::move(*data);
        }
        bytes = l->data.data();
        size = l->data.size();
    } else if (l->file.open(key)) {
        bytes = l->file.data();
        size = l->file.size();
    }
    if (size >= 12 && size <= std::size_t(INT32_MAX)) {
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false; // the bytes stay ours: released with the face
        config.OversampleH = 2;
        std::snprintf(config.Name, sizeof(config.Name), "%s", key.c_str() + (key.size() > 39 ? key.size() - 39 : 0));
        // Read only: ImGui's loader parses the file in place and never writes to it.
        l->font = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<void*>(bytes), int(size), Fonts::kUiSize, &config);
        l->lineHeight = lineHeightOf(static_cast<const unsigned char*>(bytes), size);
    }
    if (!l->font) {
        missing_.push_back(key);
        return nullptr;
    }
    loaded_.push_back(std::move(l));
    return loaded_.back().get();
}

bool DocumentFonts::covers(ImFont* font, const char* b, const char* e)
{
    for (const char* p = b; p < e;) {
        const unsigned char lead = static_cast<unsigned char>(*p);
        if (lead < 0xC9) { // below U+0250: Latin, always there
            p += lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 1;
            continue;
        }
        unsigned int c = 0;
        const int n = ImTextCharFromUtf8(&c, p, e);
        if (n <= 0)
            break;
        p += n;
        if (c >= 0x250 && c <= 0xFFFF && !font->IsGlyphInFont(ImWchar(c)))
            return false;
    }
    return true;
}

void DocumentFonts::trim(double now, double idleSeconds)
{
    bool removed = false;
    for (auto it = loaded_.begin(); it != loaded_.end();) {
        Loaded& l = **it;
        const bool idle = idleSeconds <= 0.0 ? l.lastUsed < now : now - l.lastUsed > idleSeconds;
        if (!idle) {
            ++it;
            continue;
        }
        for (Request& r : requests_)
            if (r.source == &l) {
                r.source = nullptr;
                r.face = Face();
                r.stale = true;
            }
        if (ImGui::GetCurrentContext())
            ImGui::GetIO().Fonts->RemoveFont(l.font);
        it = loaded_.erase(it); // its file is unmapped, or its bytes freed
        removed = true;
    }
    if (removed)
        requests_.erase(std::remove_if(requests_.begin(), requests_.end(), [](const Request& r) { return r.stale; }), requests_.end());
    if (loaded_.empty()) { // nothing drawn in a document font: give back the bookkeeping too
        os::mem::release(loaded_);
        os::mem::release(requests_);
        os::mem::release(missing_);
    }
}

} // namespace enp::view
