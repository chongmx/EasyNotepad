// Word (.docx): word/document.xml into the shared flow blocks. Styles give headings (by name or
// outline level, through basedOn), numbering gives list markers, runs give bold / italic /
// strike / links, drawings give images; tables keep their cells' text; footnotes and endnotes
// follow the body as notes. Tracked deletions, field instructions and fallbacks are left out.
#include "enp/model/viewer/Viewer.h"

#include "Chart.h"
#include "Ooxml.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <set>

namespace enp::model::viewer {
namespace {

namespace md = markdown;
using ooxml::XmlReader;
using Event = XmlReader::Event;

md::Span textSpan(std::string text)
{
    md::Span s;
    s.text = std::move(text);
    return s;
}

// A run's formatting as a style or the run itself sets it; unset fields leave the one below.
struct RunProps {
    float size = 0.0f;           // points
    std::uint32_t color = 0;     // 0: not set
    bool autoColor = false;      // w:color auto: the default colour, over what a style set
    std::uint32_t highlight = 0;
    bool hasHighlight = false;
    signed char bold = -1, italic = -1, underline = -1, strike = -1, code = -1;
    std::string font; // w:rFonts' family; "+major" / "+minor": the theme's heading / body font; empty: not set

    void over(const RunProps& o)
    {
        if (o.size > 0.0f)
            size = o.size;
        if (!o.font.empty())
            font = o.font;
        if (o.color || o.autoColor)
            color = o.color, autoColor = o.autoColor;
        if (o.hasHighlight)
            highlight = o.highlight, hasHighlight = true;
        if (o.bold >= 0)
            bold = o.bold;
        if (o.italic >= 0)
            italic = o.italic;
        if (o.underline >= 0)
            underline = o.underline;
        if (o.strike >= 0)
            strike = o.strike;
        if (o.code >= 0)
            code = o.code;
    }
};

// A paragraph's layout as a style or the paragraph sets it (points).
struct ParaProps {
    int align = -1; // 0 left / justified, 1 centre, 2 right
    bool hasLeft = false, hasRight = false, hasFirst = false, hasBefore = false, hasShading = false;
    float left = 0.0f, right = 0.0f, first = 0.0f, before = 0.0f;
    std::uint32_t shading = 0;
    // Tab stops as set at this level: kind 4 clears an inherited stop at its position.
    std::vector<md::TabStop> tabs;

    void over(const ParaProps& o)
    {
        for (const md::TabStop& t : o.tabs) { // a stop replaces, or clears, the one at its position
            tabs.erase(std::remove_if(tabs.begin(), tabs.end(), [&](const md::TabStop& s) { return std::fabs(s.pos - t.pos) < 0.5f; }), tabs.end());
            tabs.push_back(t);
        }
        if (!o.tabs.empty())
            std::sort(tabs.begin(), tabs.end(), [](const md::TabStop& a, const md::TabStop& b) { return a.pos < b.pos; });
        if (o.align >= 0)
            align = o.align;
        if (o.hasLeft)
            left = o.left, hasLeft = true;
        if (o.hasRight)
            right = o.right, hasRight = true;
        if (o.hasFirst)
            first = o.first, hasFirst = true;
        if (o.hasBefore)
            before = o.before, hasBefore = true;
        if (o.hasShading)
            shading = o.shading, hasShading = true;
    }
};

std::uint32_t highlightColor(std::string_view name)
{
    static const std::pair<const char*, const char*> kNames[] = {
        {"yellow", "FFFF00"},  {"green", "00FF00"},     {"cyan", "00FFFF"},        {"magenta", "FF00FF"},    {"blue", "0000FF"},
        {"red", "FF0000"},     {"darkBlue", "000080"},  {"darkCyan", "008080"},    {"darkGreen", "008000"},  {"darkMagenta", "800080"},
        {"darkRed", "800000"}, {"darkYellow", "808000"}, {"darkGray", "808080"},   {"lightGray", "C0C0C0"},  {"black", "000000"},
        {"white", "FFFFFF"}};
    for (const auto& [n, hex] : kNames)
        if (name == n)
            return ooxml::hexColor(hex);
    return 0;
}

struct Style {
    std::string name;    // lower case
    std::string basedOn;
    int outline = -1;    // w:outlineLvl (0-based)
    RunProps run;
    ParaProps para;
    std::string numId;   // a numbered style
    int numLevel = -1;   // its list level; -1: the level whose pStyle names it (else 0)
};

struct Level {
    std::string format = "decimal"; // bullet, decimal, lowerLetter, upperLetter, lowerRoman, upperRoman, none
    std::string text = "%1.";
    int start = 1;
};

struct Numbering {
    std::map<std::string, std::map<int, Level>> abstracts; // abstractNumId -> levels
    std::map<std::string, std::string> nums;               // numId -> abstractNumId
    std::map<std::string, std::map<int, int>> overrides;   // numId -> level -> start
    std::map<std::string, std::vector<int>> counters;      // numId -> per level
    std::map<std::string, int> styleLevels;                // paragraph style -> the level whose pStyle names it

    const Level* level(const std::string& numId, int ilvl) const
    {
        const auto n = nums.find(numId);
        if (n == nums.end())
            return nullptr;
        const auto a = abstracts.find(n->second);
        if (a == abstracts.end())
            return nullptr;
        const auto l = a->second.find(ilvl);
        return l == a->second.end() ? nullptr : &l->second;
    }
};

bool on(const XmlReader& x)
{
    const std::string v = x.attribute("w:val");
    return v.empty() || (v != "0" && v != "false" && v != "off" && v != "none");
}

std::string roman(int n, bool upper)
{
    static const std::pair<int, const char*> kTable[] = {{1000, "m"}, {900, "cm"}, {500, "d"}, {400, "cd"}, {100, "c"}, {90, "xc"},
                                                         {50, "l"},   {40, "xl"},  {10, "x"},  {9, "ix"},   {5, "v"},   {4, "iv"}, {1, "i"}};
    std::string out;
    for (const auto& [value, digits] : kTable)
        while (n >= value && out.size() < 30) {
            out += digits;
            n -= value;
        }
    if (upper)
        for (char& c : out)
            c = char(c - 'a' + 'A');
    return out;
}

std::string counterText(int n, const std::string& format)
{
    if (format == "lowerLetter" || format == "upperLetter") {
        std::string s;
        const int letter = (n - 1) % 26, repeat = (n - 1) / 26 + 1;
        s.assign(std::size_t(std::min(repeat, 10)), char((format == "upperLetter" ? 'A' : 'a') + letter));
        return s;
    }
    if (format == "lowerRoman" || format == "upperRoman")
        return roman(n, format == "upperRoman");
    return std::to_string(n);
}

// A child of w:rPr: into `r`. False when it is not a property read here.
bool readRunProperty(const XmlReader& x, std::string_view n, RunProps& r)
{
    auto flag = [&]() { return static_cast<signed char>(on(x) ? 1 : 0); };
    if (n == "b")
        r.bold = flag();
    else if (n == "i")
        r.italic = flag();
    else if (n == "strike" || n == "dstrike")
        r.strike = flag();
    else if (n == "u")
        r.underline = static_cast<signed char>(x.attribute("w:val") != "none" && !x.attribute("w:val").empty() ? 1 : 0);
    else if (n == "sz")
        r.size = float(std::atof(x.attribute("w:val").c_str()) / 2.0);
    else if (n == "color") {
        const std::string v = x.attribute("w:val");
        if (v == "auto") {
            r.color = 0;
            r.autoColor = true;
        } else {
            r.color = ooxml::hexColor(v);
            r.autoColor = false;
        }
    } else if (n == "highlight") {
        r.highlight = highlightColor(x.attribute("w:val"));
        r.hasHighlight = true;
    } else if (n == "shd") {
        const std::string fill = x.attribute("w:fill");
        if (!fill.empty() && fill != "auto") {
            r.highlight = ooxml::hexColor(fill);
            r.hasHighlight = true;
        }
    } else if (n == "rFonts") {
        // The family Latin text is set in: its name, else the theme's font it refers to.
        std::string font = x.attribute("w:ascii");
        if (font.empty())
            font = x.attribute("w:hAnsi");
        if (font.empty()) {
            std::string theme = x.attribute("w:asciiTheme");
            if (theme.empty())
                theme = x.attribute("w:hAnsiTheme");
            if (theme.rfind("major", 0) == 0)
                font = "+major";
            else if (theme.rfind("minor", 0) == 0)
                font = "+minor";
        }
        if (!font.empty())
            r.font = std::move(font);
    } else {
        return false;
    }
    return true;
}

// A child of w:pPr: into `p` (twips to points).
bool readParaProperty(const XmlReader& x, std::string_view n, ParaProps& p)
{
    auto twips = [&](const char* a, const char* b = nullptr) {
        std::string v = x.attribute(a);
        if (v.empty() && b)
            v = x.attribute(b);
        return float(std::atof(v.c_str()) / 20.0);
    };
    if (n == "jc") {
        const std::string v = x.attribute("w:val");
        p.align = v == "center" ? 1 : v == "right" || v == "end" ? 2 : 0;
    } else if (n == "ind") {
        if (x.hasAttribute("w:left") || x.hasAttribute("w:start"))
            p.left = twips("w:left", "w:start"), p.hasLeft = true;
        if (x.hasAttribute("w:right") || x.hasAttribute("w:end"))
            p.right = twips("w:right", "w:end"), p.hasRight = true;
        if (x.hasAttribute("w:hanging"))
            p.first = -twips("w:hanging"), p.hasFirst = true;
        else if (x.hasAttribute("w:firstLine"))
            p.first = twips("w:firstLine"), p.hasFirst = true;
    } else if (n == "spacing") {
        if (x.hasAttribute("w:before"))
            p.before = twips("w:before"), p.hasBefore = true;
    } else if (n == "shd") {
        const std::string fill = x.attribute("w:fill");
        p.shading = fill.empty() || fill == "auto" ? 0 : ooxml::hexColor(fill);
        p.hasShading = true;
    } else if (n == "tab") { // w:tabs/w:tab: a tab stop
        const std::string v = x.attribute("w:val");
        if (v == "bar" || v == "num")
            return true; // a vertical line, or a list's: not a stop text moves to
        md::TabStop t;
        t.pos = twips("w:pos");
        t.kind = v == "center" ? 1 : v == "right" || v == "end" ? 2 : v == "decimal" ? 3 : v == "clear" ? 4 : 0;
        const std::string leader = x.attribute("w:leader");
        t.leader = leader == "dot" || leader == "middleDot" ? '.' : leader == "hyphen" ? '-' : leader == "underscore" || leader == "heavy" ? '_' : 0;
        if (p.tabs.size() < 64)
            p.tabs.push_back(t);
    } else if (n == "tabs") {
        // its w:tab children are read one by one
    } else {
        return false;
    }
    return true;
}

class DocxReader {
public:
    explicit DocxReader(ooxml::ZipArchive& zip) : zip_(zip) {}

    ViewerDocument read()
    {
        ViewerDocument doc;
        doc.format = "Word document";
        main_ = ooxml::mainPart(zip_, "word/document.xml");
        const std::optional<std::string> xml = zip_.read(main_);
        if (!xml) {
            doc.error = "the document part (" + main_ + ") is missing";
            return doc;
        }
        doc_ = &doc;
        rels_ = ooxml::readRelationships(zip_, main_);
        readStyles(ooxml::targetOfType(rels_, "styles"));
        readNumbering(ooxml::targetOfType(rels_, "numbering"));
        readNotes(ooxml::targetOfType(rels_, "footnotes"), "footnote", footnotes_);
        readNotes(ooxml::targetOfType(rels_, "endnotes"), "endnote", endnotes_);
        readNotes(ooxml::targetOfType(rels_, "comments"), "comment", comments_);
        theme_ = ooxml::readTheme(zip_, ooxml::targetOfType(rels_, "theme"));
        XmlReader x(*xml);
        body(x, rels_, main_);
        // The header above the body and the footer below it and its notes (as printed), each set
        // apart by a rule.
        if (!headerId_.empty()) {
            std::vector<md::Block> header = headerFooter(headerId_);
            if (!header.empty()) {
                md::Block rule;
                rule.kind = md::BlockKind::Rule;
                header.push_back(rule);
                doc.flow.blocks.insert(doc.flow.blocks.begin(), std::make_move_iterator(header.begin()), std::make_move_iterator(header.end()));
            }
        }
        if (!x.error().empty() && doc.flow.blocks.empty())
            doc.error = "the document's XML is broken: " + x.error();
        // Footnotes and endnotes, in the order they were referenced.
        if (!noteOrder_.empty()) {
            md::Block rule;
            rule.kind = md::BlockKind::Rule;
            doc.flow.blocks.push_back(rule);
            for (const auto& [kind, id] : noteOrder_) {
                const auto& notes = kind == 'f' ? footnotes_ : endnotes_;
                const auto it = notes.find(id);
                if (it == notes.end())
                    continue;
                md::Block b;
                b.kind = md::BlockKind::Numbered;
                b.marker = noteNumbers_[std::string(1, kind) + id] + ".";
                b.spans = it->second;
                doc.flow.blocks.push_back(std::move(b));
            }
        }
        if (!footerId_.empty()) {
            std::vector<md::Block> footer = headerFooter(footerId_);
            if (!footer.empty()) {
                md::Block rule;
                rule.kind = md::BlockKind::Rule;
                doc.flow.blocks.push_back(rule);
                for (md::Block& b : footer)
                    doc.flow.blocks.push_back(std::move(b));
            }
        }
        // Comments, in the order they were referenced: "c1." and who wrote them.
        if (!commentOrder_.empty()) {
            md::Block heading;
            heading.kind = md::BlockKind::Heading;
            heading.level = 3;
            heading.spans.push_back(textSpan("Comments"));
            doc.flow.blocks.push_back(std::move(heading));
            for (std::size_t i = 0; i < commentOrder_.size(); ++i) {
                const auto it = comments_.find(commentOrder_[i]);
                if (it == comments_.end())
                    continue;
                md::Block b;
                b.kind = md::BlockKind::Numbered;
                b.marker = "c" + std::to_string(i + 1) + ".";
                const auto author = commentAuthors_.find(commentOrder_[i]);
                if (author != commentAuthors_.end() && !author->second.empty()) {
                    md::Span who = textSpan(author->second + ": ");
                    who.style = md::Bold;
                    b.spans.push_back(std::move(who));
                }
                for (const md::Span& span : it->second)
                    b.spans.push_back(span);
                doc.flow.blocks.push_back(std::move(b));
            }
        }
        if (charts_)
            doc.notShown.push_back(std::to_string(charts_) + (charts_ == 1 ? " chart" : " charts"));
        if (unresolvedImages_)
            doc.notShown.push_back(std::to_string(unresolvedImages_) + " images (missing from the file)");
        for (std::size_t i = 0; i < doc.flow.blocks.size(); ++i)
            doc.flow.blocks[i].line = int(i);
        doc.shape = doc.error.empty() ? Shape::Flow : Shape::None;
        return doc;
    }

private:
    // ---- styles and numbering ------------------------------------------------------------------
    void readStyles(const std::string& part)
    {
        if (part.empty())
            return;
        const std::optional<std::string> xml = zip_.read(part);
        if (!xml)
            return;
        XmlReader x(*xml);
        Style* s = nullptr;
        bool inRpr = false, inPpr = false, inDefaults = false;
        for (Event e; (e = x.next()) != Event::Eof && e != Event::Error;) {
            if (e == Event::End) {
                const std::string_view n = x.localName();
                if (n == "style")
                    s = nullptr;
                else if (n == "rPr")
                    inRpr = false;
                else if (n == "pPr")
                    inPpr = false;
                else if (n == "docDefaults")
                    inDefaults = false;
                continue;
            }
            if (e != Event::Start)
                continue;
            const std::string_view n = x.localName();
            if (n == "docDefaults") {
                inDefaults = true;
            } else if (n == "tblPr" || n == "tblStylePr" || n == "trPr" || n == "tcPr") {
                x.skip(); // a table style's own parts: not a paragraph's
            } else if (n == "rPr" && (s || inDefaults)) {
                inRpr = true;
            } else if (n == "pPr" && (s || inDefaults)) {
                inPpr = true;
            } else if (inRpr && readRunProperty(x, n, s ? s->run : defaults_.run)) {
            } else if (inPpr && readParaProperty(x, n, s ? s->para : defaults_.para)) {
            } else if (n == "style") {
                s = &styles_[x.attribute("w:styleId")];
                if (x.attribute("w:default") == "1" && x.attribute("w:type") == "paragraph")
                    defaultStyle_ = x.attribute("w:styleId");
            } else if (!s) {
                continue;
            } else if (n == "name") {
                s->name = x.attribute("w:val");
                for (char& c : s->name)
                    c = char(std::tolower(static_cast<unsigned char>(c)));
            } else if (n == "basedOn") {
                s->basedOn = x.attribute("w:val");
            } else if (n == "outlineLvl") {
                s->outline = std::atoi(x.attribute("w:val").c_str());
            } else if (n == "numId") {
                s->numId = x.attribute("w:val");
            } else if (n == "ilvl") {
                s->numLevel = std::atoi(x.attribute("w:val").c_str());
            }
        }
    }

    // A paragraph style's heading level (1-6), through basedOn; 0 when it is not a heading.
    int headingLevel(const std::string& styleId) const
    {
        std::string id = styleId;
        for (int guard = 0; guard < 16 && !id.empty(); ++guard) {
            const auto it = styles_.find(id);
            if (it == styles_.end())
                break;
            const Style& s = it->second;
            if (s.name == "title")
                return 1;
            if (s.name == "subtitle")
                return 2;
            if (s.name.rfind("heading ", 0) == 0) {
                const int n = std::atoi(s.name.c_str() + 8);
                if (n >= 1)
                    return std::min(n, 6);
            }
            if (s.outline >= 0 && s.outline <= 8)
                return std::min(s.outline + 1, 6);
            id = s.basedOn;
        }
        return 0;
    }

    // A style's run and paragraph properties through its basedOn chain (the base first).
    void styleProps(const std::string& id, RunProps& run, ParaProps& para, int depth = 0) const
    {
        const Style* st = style(id);
        if (!st || depth > 16)
            return;
        if (!st->basedOn.empty() && st->basedOn != id)
            styleProps(st->basedOn, run, para, depth + 1);
        run.over(st->run);
        para.over(st->para);
    }

    const Style* style(const std::string& id) const
    {
        const auto it = styles_.find(id);
        return it == styles_.end() ? nullptr : &it->second;
    }

    void readNumbering(const std::string& part)
    {
        if (part.empty())
            return;
        const std::optional<std::string> xml = zip_.read(part);
        if (!xml)
            return;
        XmlReader x(*xml);
        std::string abstractId, numId;
        Level* level = nullptr;
        int levelIndex = 0;
        int overrideLevel = -1;
        for (Event e; (e = x.next()) != Event::Eof && e != Event::Error;) {
            if (e == Event::End) {
                const std::string_view n = x.localName();
                if (n == "abstractNum")
                    abstractId.clear();
                else if (n == "num")
                    numId.clear();
                else if (n == "lvl")
                    level = nullptr;
                else if (n == "lvlOverride")
                    overrideLevel = -1;
                continue;
            }
            if (e != Event::Start)
                continue;
            const std::string_view n = x.localName();
            if (n == "abstractNum") {
                abstractId = x.attribute("w:abstractNumId");
            } else if (n == "lvl" && !abstractId.empty()) {
                levelIndex = std::clamp(std::atoi(x.attribute("w:ilvl").c_str()), 0, 8);
                level = &numbering_.abstracts[abstractId][levelIndex];
            } else if (n == "pStyle" && level) { // headings numbered through their styles
                numbering_.styleLevels.emplace(x.attribute("w:val"), levelIndex);
            } else if (n == "numFmt" && level) {
                level->format = x.attribute("w:val");
            } else if (n == "lvlText" && level) {
                level->text = x.attribute("w:val");
            } else if (n == "start" && level) {
                level->start = std::atoi(x.attribute("w:val").c_str());
            } else if (n == "num") {
                numId = x.attribute("w:numId");
            } else if (n == "abstractNumId" && !numId.empty()) {
                numbering_.nums[numId] = x.attribute("w:val");
            } else if (n == "lvlOverride" && !numId.empty()) {
                overrideLevel = std::atoi(x.attribute("w:ilvl").c_str());
            } else if (n == "startOverride" && overrideLevel >= 0) {
                numbering_.overrides[numId][overrideLevel] = std::atoi(x.attribute("w:val").c_str());
            }
        }
    }

    // The marker of the next item of list numId at ilvl ("3.", "b)", "•"), advancing the counters.
    std::string nextMarker(const std::string& numId, int ilvl, bool& bullet)
    {
        ilvl = std::clamp(ilvl, 0, 8);
        const Level* lvl = numbering_.level(numId, ilvl);
        bullet = !lvl || lvl->format == "bullet" || lvl->format == "none";
        if (bullet)
            return {};
        std::vector<int>& c = numbering_.counters[numId];
        if (c.size() < 9) {
            c.assign(9, 0);
        }
        auto startOf = [&](int level) {
            const auto o = numbering_.overrides.find(numId);
            if (o != numbering_.overrides.end()) {
                const auto l = o->second.find(level);
                if (l != o->second.end())
                    return l->second;
            }
            const Level* lv = numbering_.level(numId, level);
            return lv ? lv->start : 1;
        };
        c[size_t(ilvl)] = c[size_t(ilvl)] == 0 ? startOf(ilvl) : c[size_t(ilvl)] + 1;
        for (int deeper = ilvl + 1; deeper < 9; ++deeper)
            c[size_t(deeper)] = 0;
        std::string out;
        const std::string& t = lvl->text;
        for (std::size_t i = 0; i < t.size(); ++i) {
            if (t[i] == '%' && i + 1 < t.size() && t[i + 1] >= '1' && t[i + 1] <= '9') {
                const int which = t[i + 1] - '1';
                const Level* other = numbering_.level(numId, which);
                const int value = c[size_t(which)] == 0 ? startOf(which) : c[size_t(which)];
                out += counterText(value, other ? other->format : "decimal");
                ++i;
            } else {
                out += t[i];
            }
        }
        return out.empty() ? std::to_string(c[size_t(ilvl)]) + "." : out;
    }

    void readNotes(const std::string& part, std::string_view element, std::map<std::string, md::Spans>& into)
    {
        if (part.empty())
            return;
        const std::optional<std::string> xml = zip_.read(part);
        if (!xml)
            return;
        const ooxml::Relationships rels = ooxml::readRelationships(zip_, part);
        XmlReader x(*xml);
        for (Event e; (e = x.next()) != Event::Eof && e != Event::Error;) {
            if (e != Event::Start || x.localName() != element)
                continue;
            const std::string id = x.attribute("w:id");
            const std::string type = x.attribute("w:type");
            if (element == "comment")
                commentAuthors_[id] = x.attribute("w:author");
            if (!type.empty() && type != "normal") { // separators
                x.skip();
                continue;
            }
            md::Spans spans;
            const int depth = x.depth();
            for (Event f; (f = x.next()) != Event::Eof && f != Event::Error;) {
                if (f == Event::End && x.depth() < depth)
                    break;
                if (f == Event::Start && x.localName() == "p") {
                    md::Block p = paragraph(x, rels, part, nullptr);
                    if (!spans.empty() && !p.spans.empty())
                        spans.push_back(textSpan(" "));
                    for (md::Span& s : p.spans)
                        spans.push_back(std::move(s));
                }
            }
            into[id] = std::move(spans);
        }
    }

    // ---- the body ----------------------------------------------------------------------------
    void body(XmlReader& x, const ooxml::Relationships& rels, const std::string& part)
    {
        for (Event e; (e = x.next()) != Event::Eof && e != Event::Error;) {
            if (e != Event::Start)
                continue;
            const std::string_view n = x.localName();
            if (n == "p") {
                std::vector<md::Block> extra;
                md::Block b = paragraph(x, rels, part, &extra);
                push(std::move(b));
                for (md::Block& more : extra)
                    push(std::move(more));
            } else if (n == "tbl") {
                std::vector<md::Block> blocks;
                table(x, rels, part, blocks);
                for (md::Block& b : blocks)
                    push(std::move(b));
            } else if (n == "sectPr") {
                section(x);
            } else if (n == "Fallback") {
                x.skip();
            }
            // body, sdt, sdtContent, customXml, AlternateContent/Choice: descended into
        }
    }

    // At the Start of w:sectPr: the default (else first) header and footer of the section; the last
    // section's are the document's (sections earlier in the body may have their own).
    void section(XmlReader& x)
    {
        const int depth = x.depth();
        std::string header, footer;
        for (Event e; (e = x.next()) != Event::Eof && e != Event::Error;) {
            if (e == Event::End && x.depth() < depth)
                break;
            if (e != Event::Start)
                continue;
            const std::string_view n = x.localName();
            if (n == "headerReference" || n == "footerReference") {
                std::string& slot = n == "headerReference" ? header : footer;
                if (slot.empty() || x.attribute("w:type") == "default")
                    slot = x.attribute("r:id");
            }
        }
        if (!header.empty())
            headerId_ = header;
        if (!footer.empty())
            footerId_ = footer;
    }

    // A header or footer part's paragraphs and tables as blocks.
    std::vector<md::Block> headerFooter(const std::string& id)
    {
        std::vector<md::Block> out;
        const auto it = rels_.find(id);
        if (it == rels_.end() || it->second.external)
            return out;
        const std::string& part = it->second.target;
        const std::optional<std::string> xml = zip_.read(part, std::size_t(8) << 20);
        if (!xml)
            return out;
        const ooxml::Relationships rels = ooxml::readRelationships(zip_, part);
        XmlReader x(*xml);
        for (Event e; (e = x.next()) != Event::Eof && e != Event::Error;) {
            if (e != Event::Start)
                continue;
            if (x.localName() == "p") {
                std::vector<md::Block> more;
                md::Block b = paragraph(x, rels, part, &more);
                if (!(b.kind == md::BlockKind::Paragraph && b.spans.empty()))
                    out.push_back(std::move(b));
                for (md::Block& m : more)
                    out.push_back(std::move(m));
            } else if (x.localName() == "tbl") {
                table(x, rels, part, out);
            }
        }
        return out;
    }

    // The number of a run's font family in the document's list (Span::font), adding it the first
    // time; theme references resolved. 0 when the document names none (Word then uses Times New
    // Roman) or the list is full.
    std::uint8_t fontNumber(const std::string& font)
    {
        std::string family = font == "+major" ? theme_.majorFont : font == "+minor" ? theme_.minorFont : font;
        if (family.empty())
            family = "Times New Roman";
        std::vector<std::string>& fonts = doc_->flow.fonts;
        for (std::size_t i = 0; i < fonts.size(); ++i)
            if (fonts[i] == family)
                return std::uint8_t(i + 1);
        if (fonts.size() >= 255)
            return 0;
        fonts.push_back(std::move(family));
        return std::uint8_t(fonts.size());
    }

    void push(md::Block b)
    {
        if (b.kind == md::BlockKind::Paragraph && b.spans.empty())
            b.spans.push_back(textSpan(" ")); // an empty paragraph keeps its line, as in Word
        doc_->flow.blocks.push_back(std::move(b));
    }

    static void appendText(md::Spans& spans, std::string_view text, const md::Span& look)
    {
        if (text.empty())
            return;
        if (!spans.empty()) {
            const md::Span& last = spans.back();
            if (last.style == look.style && last.url == look.url && last.color == look.color && last.highlight == look.highlight &&
                last.size == look.size && last.font == look.font && !(look.style & md::Image)) {
                spans.back().text += text;
                return;
            }
        }
        md::Span s = look;
        s.text = std::string(text);
        spans.push_back(std::move(s));
    }

    // At the Start of w:p: the paragraph as a block (heading, list item or paragraph). Images in
    // it stay inline as Image spans. `extra`: blocks that follow it (none yet).
    md::Block paragraph(XmlReader& x, const ooxml::Relationships& rels, const std::string& part, std::vector<md::Block>* extra)
    {
        md::Block b;
        std::string styleId, numId;
        int ilvl = -1, outline = -1;
        bool hasNumPr = false;
        md::Spans& spans = b.spans;
        std::string link;    // the hyperlink being read
        int linkDepth = -1;
        bool inField = false; // between fldChar begin and separate: the field's instruction
        RunProps runDirect;   // the run's own formatting
        std::string runStyleId;
        ParaProps paraDirect;
        bool inRpr = false, inPpr = false;
        // A run's text with its look: the defaults, the paragraph's style, the run's style, the run.
        auto add = [&](std::string_view text, const std::string& url, bool asLink) {
            RunProps r = defaults_.run;
            ParaProps unused;
            styleProps(styleId.empty() ? defaultStyle_ : styleId, r, unused);
            styleProps(runStyleId, r, unused);
            r.over(runDirect);
            md::Span look;
            look.style = (r.bold == 1 ? unsigned(md::Bold) : 0u) | (r.italic == 1 ? unsigned(md::Italic) : 0u) |
                         (r.strike == 1 ? unsigned(md::Strike) : 0u) | (r.underline == 1 ? unsigned(md::Underline) : 0u) |
                         (r.code == 1 ? unsigned(md::Code) : 0u) | (asLink ? unsigned(md::Link) : 0u);
            if (const Style* rs = style(runStyleId); rs && (rs->name.find("code") != std::string::npos))
                look.style |= md::Code;
            look.color = asLink ? 0u : r.color;
            look.highlight = r.highlight;
            look.size = r.size;
            look.font = fontNumber(r.font);
            look.url = url;
            appendText(spans, text, look);
        };
        const int depth = x.depth();
        for (Event e; (e = x.next()) != Event::Eof && e != Event::Error;) {
            if (e == Event::End) {
                if (x.depth() < depth)
                    break;
                const std::string_view n = x.localName();
                if (n == "hyperlink" && x.depth() + 1 == linkDepth) {
                    link.clear();
                    linkDepth = -1;
                } else if (n == "rPr") {
                    inRpr = false;
                } else if (n == "pPr") {
                    inPpr = false;
                }
                continue;
            }
            if (e != Event::Start)
                continue;
            const std::string_view n = x.localName();
            if (inPpr) {
                if (n == "pStyle")
                    styleId = x.attribute("w:val");
                else if (n == "numPr")
                    hasNumPr = true;
                else if (n == "ilvl")
                    ilvl = std::atoi(x.attribute("w:val").c_str());
                else if (n == "numId")
                    numId = x.attribute("w:val");
                else if (n == "outlineLvl")
                    outline = std::atoi(x.attribute("w:val").c_str());
                else if (readParaProperty(x, n, paraDirect)) {
                } else if (n == "rPr" || n == "sectPr" || n == "pPrChange")
                    x.skip(); // the paragraph mark's format; a section break; a tracked format change
                continue;
            }
            if (inRpr) {
                if (n == "rStyle")
                    runStyleId = x.attribute("w:val");
                else if (n == "rPrChange")
                    x.skip();
                else
                    readRunProperty(x, n, runDirect);
                continue;
            }
            if (n == "pPr") {
                inPpr = true;
            } else if (n == "r") {
                runDirect = RunProps();
                runStyleId.clear();
            } else if (n == "rPr") {
                inRpr = true;
            } else if (n == "t") {
                const std::string text = x.elementText();
                if (!inField)
                    add(text, link, !link.empty());
            } else if (n == "tab" || n == "ptab") {
                if (!inField)
                    add("\t", link, !link.empty()); // to the next tab stop (the renderer's)
            } else if (n == "br" || n == "cr") {
                const std::string type = x.attribute("w:type");
                if (type != "page" && type != "column")
                    add("\n", link, !link.empty()); // a line break: the renderer starts a new line
            } else if (n == "noBreakHyphen") {
                add("-", link, !link.empty());
            } else if (n == "sym") {
                add("\xE2\x96\xA1", link, !link.empty()); // a symbol-font glyph: a box
            } else if (n == "fldChar") {
                const std::string type = x.attribute("w:fldCharType");
                if (type == "begin")
                    inField = true;
                else if (type == "separate" || type == "end")
                    inField = false;
            } else if (n == "instrText" || n == "delText" || n == "del" || n == "moveFrom" || n == "Fallback" ||
                       n == "commentRangeStart" || n == "commentRangeEnd") {
                x.skip();
            } else if (n == "commentReference") {
                const std::string id = x.attribute("w:id");
                auto at = std::find(commentOrder_.begin(), commentOrder_.end(), id);
                if (at == commentOrder_.end()) {
                    commentOrder_.push_back(id);
                    at = commentOrder_.end() - 1;
                }
                add("[c" + std::to_string(at - commentOrder_.begin() + 1) + "]", {}, false);
            } else if (n == "hyperlink") {
                const std::string id = x.attribute("r:id");
                const auto it = rels.find(id);
                if (it != rels.end())
                    link = it->second.target;
                else if (!x.attribute("w:anchor").empty())
                    link = "#" + x.attribute("w:anchor");
                linkDepth = x.depth();
            } else if (n == "footnoteReference" || n == "endnoteReference") {
                const char kind = n == "footnoteReference" ? 'f' : 'e';
                const std::string id = x.attribute("w:id");
                const std::string key = std::string(1, kind) + id;
                if (!noteNumbers_.count(key)) {
                    noteNumbers_[key] = std::to_string(noteOrder_.size() + 1);
                    noteOrder_.push_back({kind, id});
                }
                add("[" + noteNumbers_[key] + "]", {}, false);
            } else if (n == "drawing" || n == "pict" || n == "object") {
                drawing(x, rels, part, spans, extra);
            }
        }

        // Heading, list item or paragraph.
        const Style* ps = style(styleId);
        int heading = headingLevel(styleId);
        if (outline >= 0 && outline <= 8)
            heading = std::min(outline + 1, 6);
        if (!hasNumPr && ps && !ps->numId.empty()) {
            numId = ps->numId;
            if (ilvl < 0) {
                ilvl = ps->numLevel;
                if (ilvl < 0) { // the level of the list that names this style (Appendix2 -> "%1.%2")
                    const auto it = numbering_.styleLevels.find(styleId);
                    ilvl = it != numbering_.styleLevels.end() ? it->second : 0;
                }
            }
            hasNumPr = true;
        }
        if (heading > 0 && !spans.empty()) {
            b.kind = md::BlockKind::Heading;
            b.level = heading;
            for (md::Span& s : spans)
                s.style &= ~unsigned(md::Bold); // headings are drawn bold anyway
            if (hasNumPr && !numId.empty() && numId != "0") { // numbered headings: "A.1 Introduction"
                bool bullet = false;
                const std::string marker = nextMarker(numId, std::max(0, ilvl), bullet);
                if (!marker.empty() && !bullet)
                    spans.insert(spans.begin(), textSpan(marker + " "));
            }
        } else if (hasNumPr && !numId.empty() && numId != "0" && !spans.empty()) {
            bool bullet = false;
            const std::string marker = nextMarker(numId, std::max(0, ilvl), bullet);
            b.kind = bullet ? md::BlockKind::Bullet : md::BlockKind::Numbered;
            b.level = std::clamp(ilvl, 0, 8);
            b.marker = marker;
        } else if (ps && (ps->name.find("quote") != std::string::npos)) {
            b.quote = 1;
        }
        RunProps baseRun = defaults_.run;
        ParaProps para = defaults_.para;
        styleProps(styleId.empty() ? defaultStyle_ : styleId, baseRun, para);
        para.over(paraDirect);
        b.size = baseRun.size;
        for (md::Span& span : spans)
            if (span.size == b.size)
                span.size = 0.0f; // the paragraph's own size
        b.textAlign = std::max(0, para.align);
        if (b.kind == md::BlockKind::Paragraph || b.kind == md::BlockKind::Heading) { // lists indent by level
            b.indentLeft = std::clamp(para.left, 0.0f, 288.0f);
            b.indentRight = std::clamp(para.right, 0.0f, 288.0f);
            b.firstLine = std::clamp(para.first, -b.indentLeft, 144.0f);
        }
        if (paraDirect.hasBefore || (ps && ps->para.hasBefore))
            b.spaceBefore = std::clamp(para.before - 6.0f, 0.0f, 36.0f); // beyond the renderer's own gap
        b.shading = para.shading;
        for (const md::TabStop& t : para.tabs) // the stops left after the styles' and the paragraph's own clears
            if (t.kind != 4)
                b.tabs.push_back(t);
        return b;
    }

    // At the Start of w:drawing / w:pict / w:object: its image (blip or VML imagedata) as an
    // Image span; a chart as a drawing block and a text box's paragraphs as quoted blocks, both
    // after the paragraph (in `extra`; dropped where there is none: in notes).
    void drawing(XmlReader& x, const ooxml::Relationships& rels, const std::string& part, md::Spans& spans, std::vector<md::Block>* extra)
    {
        std::string alt, embed;
        float width = 0.0f, height = 0.0f; // points, from wp:extent
        const int depth = x.depth();
        for (Event e; (e = x.next()) != Event::Eof && e != Event::Error;) {
            if (e == Event::End && x.depth() < depth)
                break;
            if (e != Event::Start)
                continue;
            const std::string_view n = x.localName();
            if (n == "extent" && width == 0.0f) {
                width = float(std::atof(x.attribute("cx").c_str()) / 12700.0);
                height = float(std::atof(x.attribute("cy").c_str()) / 12700.0);
            } else if (n == "docPr") {
                alt = x.attribute("descr");
                if (alt.empty())
                    alt = x.attribute("title");
            } else if (n == "blip" && embed.empty()) {
                embed = x.attribute("r:embed");
            } else if (n == "imagedata" && embed.empty()) {
                embed = x.attribute("r:id");
            } else if (n == "chart") {
                chartBlock(rels, x.attribute("r:id"), width, height, extra);
            } else if (n == "txbxContent") {
                textBox(x, rels, part, extra);
            }
        }
        if (embed.empty())
            return;
        ++images_;
        const auto it = rels.find(embed);
        if (it == rels.end() || it->second.external) {
            ++unresolvedImages_;
            return;
        }
        const std::string& target = it->second.target;
        if (!doc_->flow.media.count(target)) {
            std::optional<std::string> bytes = zip_.read(target, std::size_t(64) << 20);
            if (!bytes) {
                ++unresolvedImages_;
                return;
            }
            doc_->flow.media[target] = std::move(*bytes);
        }
        md::Span s;
        s.style = md::Image;
        s.text = alt.empty() ? "image" : alt;
        s.url = target;
        spans.push_back(std::move(s));
    }

    // At the Start of w:tbl: a table block of the cells' text (nested tables flattened into their cell).
    void table(XmlReader& x, const ooxml::Relationships& rels, const std::string& part, std::vector<md::Block>& out)
    {
        md::Block t;
        t.kind = md::BlockKind::Table;
        const int depth = x.depth();
        std::vector<md::Spans>* row = nullptr;
        md::Spans* cell = nullptr;
        int span = 1;
        std::vector<md::Block> images; // images in cells: shown after the table
        for (Event e; (e = x.next()) != Event::Eof && e != Event::Error;) {
            if (e == Event::End) {
                if (x.depth() < depth)
                    break;
                const std::string_view n = x.localName();
                if (n == "tc" && row && x.depth() == depth + 1) {
                    for (int i = 1; i < span && row->size() < 64; ++i)
                        row->emplace_back();
                    cell = nullptr;
                } else if (n == "tr" && x.depth() == depth) {
                    row = nullptr;
                }
                continue;
            }
            if (e != Event::Start)
                continue;
            const std::string_view n = x.localName();
            if (n == "tr" && x.depth() == depth + 1) {
                t.rows.emplace_back();
                row = &t.rows.back();
            } else if (n == "tc" && row && x.depth() == depth + 2) {
                row->emplace_back();
                cell = &row->back();
                span = 1;
            } else if (n == "gridSpan") {
                span = std::clamp(std::atoi(x.attribute("w:val").c_str()), 1, 64);
            } else if (n == "p" && cell) {
                md::Block p = paragraph(x, rels, part, &images); // a chart or text box in a cell follows the table
                if (!p.marker.empty())
                    p.spans.insert(p.spans.begin(), textSpan(p.marker + " "));
                else if (p.kind == md::BlockKind::Bullet)
                    p.spans.insert(p.spans.begin(), textSpan("\xE2\x80\xA2 "));
                md::Spans kept;
                for (md::Span& s : p.spans) {
                    if (s.style & md::Image) { // the renderer's table cells hold text: the image follows the table
                        md::Block img;
                        img.kind = md::BlockKind::Paragraph;
                        img.spans.push_back(std::move(s));
                        images.push_back(std::move(img));
                    } else {
                        kept.push_back(std::move(s));
                    }
                }
                if (!cell->empty() && !kept.empty())
                    cell->push_back(textSpan("\n")); // a cell's paragraphs, each on its own line
                for (md::Span& s : kept)
                    cell->push_back(std::move(s));
            } else if (n == "jc" && x.depth() == depth + 2) { // tblPr's: the table's own alignment
                const std::string v = x.attribute("w:val");
                t.textAlign = v == "center" ? 1 : v == "right" || v == "end" ? 2 : 0;
            } else if (n == "tblGrid" || n == "trPr") {
                x.skip();
            }
        }
        std::size_t columns = 0;
        for (const auto& r : t.rows)
            columns = std::max(columns, r.size());
        for (auto& r : t.rows)
            r.resize(columns);
        t.align.assign(columns, -1);
        if (!t.rows.empty() && columns > 0) {
            if (t.rows.size() == 1) // the renderer's first row is the header: a one-row table gets an empty body
                t.rows.emplace_back(columns);
            out.push_back(std::move(t));
        }
        for (md::Block& img : images)
            out.push_back(std::move(img));
    }

    // A chart (c:chart r:id) as a drawing block of the drawing's size.
    void chartBlock(const ooxml::Relationships& rels, const std::string& id, float width, float height, std::vector<md::Block>* extra)
    {
        const auto it = rels.find(id);
        chart::Chart c;
        if (!extra || it == rels.end() || it->second.external || !chart::read(zip_, it->second.target, theme_, c)) {
            ++charts_;
            return;
        }
        Page page;
        page.width = width > 16.0f ? width : 432.0f;
        page.height = height > 16.0f ? height : 252.0f;
        page.background = 0;
        chart::draw(c, theme_, Rect{0.0f, 0.0f, page.width, page.height}, page);
        md::Block b;
        b.kind = md::BlockKind::Drawing;
        b.level = int(doc_->flow.drawings.size());
        doc_->flow.drawings.push_back(std::move(page));
        extra->push_back(std::move(b));
    }

    // At the Start of w:txbxContent: its paragraphs and tables, quoted (set apart from the text).
    void textBox(XmlReader& x, const ooxml::Relationships& rels, const std::string& part, std::vector<md::Block>* extra)
    {
        std::vector<md::Block> blocks;
        const int depth = x.depth();
        for (Event e; (e = x.next()) != Event::Eof && e != Event::Error;) {
            if (e == Event::End && x.depth() < depth)
                break;
            if (e != Event::Start)
                continue;
            const std::string_view n = x.localName();
            if (n == "p") {
                std::vector<md::Block> more;
                md::Block b = paragraph(x, rels, part, &more);
                blocks.push_back(std::move(b));
                for (md::Block& m : more)
                    blocks.push_back(std::move(m));
            } else if (n == "tbl") {
                table(x, rels, part, blocks);
            }
        }
        if (!extra)
            return;
        for (md::Block& b : blocks) {
            if (b.kind == md::BlockKind::Paragraph && b.spans.empty())
                continue;
            b.quote = std::max(1, b.quote + 1);
            extra->push_back(std::move(b));
        }
    }

    ooxml::ZipArchive& zip_;
    std::string main_;
    ooxml::Relationships rels_;
    std::map<std::string, Style> styles_;
    Style defaults_;           // w:docDefaults
    std::string headerId_, footerId_; // the document's header and footer (relationship ids)
    std::string defaultStyle_; // the paragraph style a paragraph without one has ("Normal")
    Numbering numbering_;
    std::map<std::string, md::Spans> footnotes_, endnotes_, comments_;
    std::map<std::string, std::string> commentAuthors_;
    std::vector<std::string> commentOrder_;
    ooxml::Theme theme_;
    std::vector<std::pair<char, std::string>> noteOrder_;
    std::map<std::string, std::string> noteNumbers_;
    ViewerDocument* doc_ = nullptr;
    int charts_ = 0, images_ = 0, unresolvedImages_ = 0;
};

} // namespace

ViewerDocument readDocx(std::string bytes)
{
    ooxml::ZipArchive zip;
    std::string error;
    if (!zip.open(std::move(bytes), &error)) {
        ViewerDocument doc;
        doc.format = "Word document";
        doc.error = error;
        return doc;
    }
    return DocxReader(zip).read();
}

} // namespace enp::model::viewer
