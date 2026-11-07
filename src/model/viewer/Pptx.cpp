// PowerPoint (.pptx): each slide into a fixed page. Shapes (preset rectangles, ellipses, lines,
// custom paths) with their fills and outlines, text bodies, pictures, tables and groups; master
// and layout shapes behind the slide's; placeholders take their position from the layout or the
// master and their text style from the master's title / body styles; theme colours through the
// master's colour map. Charts, SmartArt and media are drawn as labelled boxes.
#include "enp/model/viewer/Viewer.h"

#include "Chart.h"
#include "Ooxml.h"
#include "Shapes.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>

namespace enp::model::viewer {
namespace {

using ooxml::XmlReader;
using Event = XmlReader::Event;

constexpr double kEmuPerPoint = 12700.0;

struct Xf {
    double ox = 0.0, oy = 0.0, sx = 1.0 / kEmuPerPoint, sy = 1.0 / kEmuPerPoint;
    Rect map(double x, double y, double w, double h) const
    {
        return Rect{float(ox + x * sx), float(oy + y * sy), float(w * sx), float(h * sy)};
    }
};

struct LevelStyle {
    float size = 0.0f; // points; 0: not set
    std::uint32_t color = 0;
    bool hasColor = false;
    int bold = -1, italic = -1;
    int caps = -1;            // cap="all": 1; "none" / "small": 0; -1 not set
    int align = -1;           // 0 left, 1 centre, 2 right, 3 justified; -1 not set
    float spaceBefore = -1.0f; // points; -1 not set
    float spaceBeforePct = -1.0f; // of the text size; -1 not set
    float lineSpacing = -1.0f; // multiple of the line; -1 not set
    std::string bullet; // a character, "auto" for numbering, "" for none set, "none"

    // What `o` sets, over this.
    void over(const LevelStyle& o)
    {
        if (o.size > 0.0f)
            size = o.size;
        if (o.hasColor)
            color = o.color, hasColor = true;
        if (o.bold >= 0)
            bold = o.bold;
        if (o.italic >= 0)
            italic = o.italic;
        if (o.caps >= 0)
            caps = o.caps;
        if (o.align >= 0)
            align = o.align;
        if (o.spaceBefore >= 0.0f)
            spaceBefore = o.spaceBefore, spaceBeforePct = -1.0f;
        if (o.spaceBeforePct >= 0.0f)
            spaceBeforePct = o.spaceBeforePct, spaceBefore = -1.0f;
        if (o.lineSpacing > 0.0f)
            lineSpacing = o.lineSpacing;
        if (!o.bullet.empty())
            bullet = o.bullet;
    }
};

struct TextStyle {
    LevelStyle levels[9];
    void over(const TextStyle& o)
    {
        for (int i = 0; i < 9; ++i)
            levels[i].over(o.levels[i]);
    }
};

// A table style's look for one part of a table (the whole table, the header row, banded rows, ...).
struct TablePart {
    std::uint32_t fill = 0;
    bool hasFill = false;
    std::uint32_t text = 0;
    bool hasText = false;
    int bold = -1;
};
struct TableStyle {
    std::map<std::string, TablePart> parts; // wholeTbl firstRow lastRow firstCol lastCol band1H band2H band1V band2V
    std::uint32_t border = 0;               // the lines between cells (0: a light grey)
};

int alignOf(const std::string& a)
{
    return a == "ctr" ? 1 : a == "r" ? 2 : a == "just" || a == "dist" ? 3 : a == "l" ? 0 : -1;
}

struct Placeholder {
    Rect box;
    bool hasBox = false;
    int anchor = -1;
    TextStyle lst; // its txBody's lstStyle: what text in it, and in the placeholders that inherit it, looks like
};

// What a master or a layout gives the slides that use it.
struct Template {
    std::string part;
    ooxml::Relationships rels;
    std::map<std::string, Placeholder> byIdx, byType;
    std::uint32_t background = 0;
    Gradient backgroundGradient;
    bool hasBackground = false;
    std::string backgroundImage;
    bool showMasterShapes = true;
};

struct Master : Template {
    ooxml::Theme theme;
    std::map<std::string, std::string> clrMap;
    TextStyle title, body, other;
};

struct Context {
    const Master* master = nullptr;
    const Template* layout = nullptr;
    const ooxml::Relationships* rels = nullptr; // of the part being drawn (pictures)
    bool skipPlaceholders = false;
};

struct ShapeProps {
    bool hasXfrm = false;
    double x = 0, y = 0, w = 0, h = 0;
    bool flipH = false, flipV = false;
    float rotation = 0.0f; // degrees clockwise
    std::string geometry = "rect";
    std::vector<std::vector<Point>> custom; // custGeom paths in 0..1 of the box
    std::vector<bool> customClosed;
    std::uint32_t fill = 0;
    bool hasFill = false;
    std::uint32_t line = 0;
    bool hasLine = false;
    double lineWidth = 12700.0 * 0.75;
    Gradient gradient;        // with a gradFill
    dml::LineStyle lineStyle; // its dashes and ends
    Rect crop{0.0f, 0.0f, 1.0f, 1.0f}; // a picture's srcRect
    // From <p:style>: what the theme's references give when spPr says nothing.
    std::uint32_t styleFill = 0, styleLine = 0, styleFont = 0;
    bool hasStyleFill = false, hasStyleLine = false, hasStyleFont = false;
    Gradient styleGradient; // the theme's fill style, when it is a gradient
};

std::string phKey(const std::string& type)
{
    if (type.empty() || type == "obj")
        return "body";
    if (type == "ctrTitle")
        return "title";
    return type;
}

class PptxReader {
public:
    explicit PptxReader(ooxml::ZipArchive& zip) : zip_(zip) {}

    ViewerDocument read()
    {
        ViewerDocument doc;
        doc.format = "PowerPoint presentation";
        const std::string main = ooxml::mainPart(zip_, "ppt/presentation.xml");
        const std::optional<std::string> xml = zip_.read(main);
        if (!xml) {
            doc.error = "the presentation part (" + main + ") is missing";
            return doc;
        }
        const ooxml::Relationships rels = ooxml::readRelationships(zip_, main);
        presentationRels_ = rels;
        float width = 720.0f, height = 540.0f;
        std::vector<std::string> slides;
        XmlReader x(*xml);
        for (Event e; (e = x.next()) != Event::Eof && e != Event::Error;) {
            if (e != Event::Start)
                continue;
            if (x.localName() == "sldSz") {
                width = float(std::atof(x.attribute("cx").c_str()) / kEmuPerPoint);
                height = float(std::atof(x.attribute("cy").c_str()) / kEmuPerPoint);
            } else if (x.localName() == "sldId") {
                const auto it = rels.find(x.attribute("r:id"));
                if (it != rels.end() && slides.size() < 2000)
                    slides.push_back(it->second.target);
            }
        }
        if (width < 1.0f || height < 1.0f) {
            width = 720.0f;
            height = 540.0f;
        }
        doc_ = &doc;
        for (const std::string& part : slides) {
            Page page;
            page.width = width;
            page.height = height;
            slideNumber_ = int(doc.pages.pages.size()) + 1;
            slide(part, page);
            if (page.title.empty())
                page.title = "Slide " + std::to_string(doc.pages.pages.size() + 1);
            doc.pages.pages.push_back(std::move(page));
        }
        if (doc.pages.pages.empty()) {
            doc.error = "the presentation has no slides";
            return doc;
        }
        if (charts_)
            doc.notShown.push_back(std::to_string(charts_) + (charts_ == 1 ? " chart" : " charts") + " (shown as boxes)");
        if (diagrams_)
            doc.notShown.push_back(std::to_string(diagrams_) + " SmartArt diagrams (their text shown as an outline)");
        if (media_)
            doc.notShown.push_back(std::to_string(media_) + " videos or sounds");
        doc.shape = Shape::Pages;
        return doc;
    }

private:
    // ---- masters and layouts -----------------------------------------------------------------
    const Master* master(const std::string& part)
    {
        auto it = masters_.find(part);
        if (it != masters_.end())
            return it->second.get();
        auto m = std::make_unique<Master>();
        m->part = part;
        m->rels = ooxml::readRelationships(zip_, part);
        m->theme = ooxml::readTheme(zip_, ooxml::targetOfType(m->rels, "theme"));
        if (const std::optional<std::string> xml = zip_.read(part)) {
            XmlReader x(*xml);
            for (Event e; (e = x.next()) != Event::Eof && e != Event::Error;) {
                if (e != Event::Start)
                    continue;
                const std::string_view n = x.localName();
                if (n == "clrMap") {
                    for (const char* k : {"bg1", "tx1", "bg2", "tx2", "accent1", "accent2", "accent3", "accent4", "accent5", "accent6", "hlink", "folHlink"})
                        if (x.hasAttribute(k))
                            m->clrMap[k] = x.attribute(k);
                } else if (n == "titleStyle" || n == "bodyStyle" || n == "otherStyle") {
                    readTextStyle(x, n == "titleStyle" ? m->title : n == "bodyStyle" ? m->body : m->other, m->theme, &m->clrMap);
                }
            }
        }
        readTemplate(*m, m->theme, &m->clrMap);
        return (masters_[part] = std::move(m)).get();
    }

    const Template* layout(const std::string& part, const Master& m)
    {
        auto it = layouts_.find(part);
        if (it != layouts_.end())
            return it->second.get();
        auto l = std::make_unique<Template>();
        l->part = part;
        l->rels = ooxml::readRelationships(zip_, part);
        readTemplate(*l, m.theme, &m.clrMap);
        return (layouts_[part] = std::move(l)).get();
    }

    // A master's or layout's placeholders (their boxes), background and showMasterSp.
    void readTemplate(Template& t, const ooxml::Theme& theme, const std::map<std::string, std::string>* clrMap)
    {
        const std::optional<std::string> xml = zip_.read(t.part);
        if (!xml)
            return;
        XmlReader x(*xml);
        for (Event e; (e = x.next()) != Event::Eof && e != Event::Error;) {
            if (e != Event::Start)
                continue;
            const std::string_view n = x.localName();
            if (n == "sldLayout" || n == "sld") {
                const std::string show = x.attribute("showMasterSp");
                t.showMasterShapes = !(show == "0" || show == "false");
            } else if (n == "bg") {
                background(x, theme, clrMap, t.rels, t.background, t.hasBackground, t.backgroundImage, t.backgroundGradient);
            } else if (n == "sp") {
                std::string type, idx;
                Placeholder p;
                const int depth = x.depth();
                bool inSpPr = false;
                for (Event f; (f = x.next()) != Event::Eof && f != Event::Error;) {
                    if (f == Event::End) {
                        if (x.depth() < depth)
                            break;
                        if (x.localName() == "spPr")
                            inSpPr = false;
                        continue;
                    }
                    if (f != Event::Start)
                        continue;
                    const std::string_view m = x.localName();
                    if (m == "ph") {
                        type = x.hasAttribute("type") ? x.attribute("type") : "obj";
                        idx = x.attribute("idx");
                    } else if (m == "spPr") {
                        inSpPr = true;
                    } else if (inSpPr && m == "off") {
                        p.box.x = float(std::atof(x.attribute("x").c_str()) / kEmuPerPoint);
                        p.box.y = float(std::atof(x.attribute("y").c_str()) / kEmuPerPoint);
                        p.hasBox = true;
                    } else if (inSpPr && m == "ext") {
                        p.box.w = float(std::atof(x.attribute("cx").c_str()) / kEmuPerPoint);
                        p.box.h = float(std::atof(x.attribute("cy").c_str()) / kEmuPerPoint);
                    } else if (m == "bodyPr") {
                        const std::string a = x.attribute("anchor");
                        if (!a.empty())
                            p.anchor = a == "ctr" ? 1 : a == "b" ? 2 : 0;
                    } else if (m == "lstStyle") {
                        readTextStyle(x, p.lst, theme, clrMap);
                    } else if (m == "txBody" || m == "style") {
                        if (m == "style")
                            x.skip();
                    }
                }
                if (type.empty())
                    continue;
                if (!idx.empty())
                    t.byIdx.emplace(idx, p);
                t.byType.emplace(phKey(type), p);
            }
        }
    }

    // At the Start of a:lnSpc / a:spcBef: its spcPct or spcPts into `l` (to its End).
    static void readSpacing(XmlReader& x, bool line, LevelStyle& l)
    {
        const int depth = x.depth();
        for (Event e; (e = x.next()) != Event::Eof && e != Event::Error;) {
            if (e == Event::End && x.depth() < depth)
                break;
            if (e != Event::Start)
                continue;
            const double v = std::atof(x.attribute("val").c_str());
            if (x.localName() == "spcPct") {
                if (line)
                    l.lineSpacing = std::clamp(float(v / 100000.0), 0.5f, 3.0f);
                else
                    l.spaceBeforePct = std::clamp(float(v / 100000.0), 0.0f, 5.0f);
            } else if (x.localName() == "spcPts") {
                if (line)
                    ; // exact line heights: the run sizes give the line its height
                else
                    l.spaceBefore = std::clamp(float(v / 100.0), 0.0f, 200.0f);
            }
        }
    }

    static void readTextStyle(XmlReader& x, TextStyle& style, const ooxml::Theme& theme, const std::map<std::string, std::string>* clrMap)
    {
        const int depth = x.depth();
        int level = -1;
        for (Event e; (e = x.next()) != Event::Eof && e != Event::Error;) {
            if (e == Event::End && x.depth() < depth)
                break;
            if (e != Event::Start)
                continue;
            const std::string_view n = x.localName();
            if (n.size() == 7 && n.substr(0, 3) == "lvl" && n.substr(4) == "pPr") {
                level = std::clamp(n[3] - '1', 0, 8);
                style.levels[level].align = alignOf(x.attribute("algn"));
            } else if (level >= 0 && (n == "lnSpc" || n == "spcBef")) {
                readSpacing(x, n == "lnSpc", style.levels[level]);
            } else if (level >= 0 && n == "defRPr") {
                LevelStyle& l = style.levels[level];
                if (x.hasAttribute("sz"))
                    l.size = float(std::atof(x.attribute("sz").c_str()) / 100.0);
                if (x.hasAttribute("b"))
                    l.bold = x.attribute("b") == "1" ? 1 : 0;
                if (x.hasAttribute("i"))
                    l.italic = x.attribute("i") == "1" ? 1 : 0;
                if (x.hasAttribute("cap"))
                    l.caps = x.attribute("cap") == "all" ? 1 : 0;
            } else if (level >= 0 && n == "solidFill") {
                bool found = false;
                const std::uint32_t c = ooxml::readFillIn(x, theme, clrMap, &found);
                if (found) {
                    style.levels[level].color = c;
                    style.levels[level].hasColor = true;
                }
            } else if (level >= 0 && n == "buChar") {
                style.levels[level].bullet = x.attribute("char");
            } else if (level >= 0 && n == "buAutoNum") {
                style.levels[level].bullet = "auto";
            } else if (level >= 0 && n == "buNone") {
                style.levels[level].bullet = "none";
            }
        }
    }

    void background(XmlReader& x, const ooxml::Theme& theme, const std::map<std::string, std::string>* clrMap,
                    const ooxml::Relationships& rels, std::uint32_t& color, bool& has, std::string& image, Gradient& gradient)
    {
        const int depth = x.depth();
        for (Event e; (e = x.next()) != Event::Eof && e != Event::Error;) {
            if (e == Event::End && x.depth() < depth)
                break;
            if (e != Event::Start)
                continue;
            const std::string_view n = x.localName();
            if (n == "gradFill") {
                color = ooxml::readGradientFill(x, theme, clrMap, gradient);
                has = has || !gradient.stops.empty();
            } else if (n == "solidFill" || n == "bgRef") {
                // bgRef: the theme's background fill style (idx 1001 on) or fill style (1 to 999),
                // in the referenced colour.
                const int idx = n == "bgRef" ? std::atoi(x.attribute("idx").c_str()) : 0;
                bool found = false;
                const std::uint32_t c = ooxml::readFillIn(x, theme, clrMap, &found);
                if (found) {
                    color = c;
                    gradient = Gradient();
                    has = true;
                    if (idx > 0) {
                        std::uint32_t styled = c;
                        if (ooxml::themeFill(theme, idx >= 1001 ? idx - 1001 : idx - 1, idx >= 1001, c, clrMap, styled, gradient))
                            color = styled;
                        else
                            gradient = Gradient();
                    }
                }
            } else if (n == "blip") {
                const auto it = rels.find(x.attribute("r:embed"));
                if (it != rels.end() && !it->second.external) {
                    image = it->second.target;
                    has = true;
                }
            }
        }
    }

    // ---- a slide -----------------------------------------------------------------------------
    void slide(const std::string& part, Page& page)
    {
        const ooxml::Relationships rels = ooxml::readRelationships(zip_, part);
        const std::string layoutPart = ooxml::targetOfType(rels, "slideLayout");
        const ooxml::Relationships layoutRels = ooxml::readRelationships(zip_, layoutPart);
        const std::string masterPart = ooxml::targetOfType(layoutRels, "slideMaster");
        const Master* m = master(masterPart);
        const Template* l = layout(layoutPart, *m);
        Template slideTemplate; // the slide's own background and showMasterSp
        slideTemplate.part = part;
        slideTemplate.rels = rels;
        readTemplate(slideTemplate, m->theme, &m->clrMap);

        // Background: the slide's, else the layout's, else the master's, else the theme's light colour.
        const Template* bg = slideTemplate.hasBackground ? &slideTemplate : l->hasBackground ? l : m->hasBackground ? m : nullptr;
        page.background = bg && !bg->backgroundImage.empty() ? kWhite : bg ? bg->background : m->theme.color("lt1", kWhite);
        if (bg && bg->backgroundImage.empty())
            page.backgroundGradient = bg->backgroundGradient;
        if (bg && !bg->backgroundImage.empty()) {
            if (addMedia(bg->backgroundImage)) {
                ImageItem img;
                img.box = Rect{0, 0, page.width, page.height};
                img.media = bg->backgroundImage;
                page.add(img);
            }
        }

        Context c;
        c.master = m;
        c.layout = l;
        if (l->showMasterShapes && slideTemplate.showMasterShapes) {
            c.rels = &m->rels;
            c.skipPlaceholders = true;
            drawPart(m->part, c, page);
        }
        if (slideTemplate.showMasterShapes) {
            c.rels = &l->rels;
            c.skipPlaceholders = true;
            drawPart(l->part, c, page);
        }
        c.rels = &rels;
        c.skipPlaceholders = false;
        title_.clear();
        drawPart(part, c, page);
        page.title = title_;
        const std::string notes = ooxml::targetOfType(rels, "notesSlide");
        if (!notes.empty())
            page.notes = notesText(notes);
    }

    void drawPart(const std::string& part, const Context& c, Page& page)
    {
        const std::optional<std::string> xml = zip_.read(part);
        if (!xml)
            return;
        XmlReader x(*xml);
        for (Event e; (e = x.next()) != Event::Eof && e != Event::Error;) {
            if (e == Event::Start && x.localName() == "spTree") {
                tree(x, c, page, Xf{});
                return;
            }
        }
    }

    // At the Start of spTree or grpSp: the shapes inside, in order.
    void tree(XmlReader& x, const Context& c, Page& page, const Xf& xf)
    {
        const int depth = x.depth();
        for (Event e; (e = x.next()) != Event::Eof && e != Event::Error;) {
            if (e == Event::End && x.depth() < depth)
                break;
            if (e != Event::Start)
                continue;
            const std::string_view n = x.localName();
            if (n == "sp" || n == "cxnSp")
                shape(x, c, page, xf);
            else if (n == "pic")
                picture(x, c, page, xf);
            else if (n == "grpSp")
                group(x, c, page, xf);
            else if (n == "graphicFrame")
                frame(x, c, page, xf);
            else if (n == "Fallback")
                x.skip();
            else if (n == "nvGrpSpPr" || n == "grpSpPr")
                x.skip();
            // AlternateContent / Choice: descended into
        }
    }

    void group(XmlReader& x, const Context& c, Page& page, const Xf& xf)
    {
        double off[2] = {0, 0}, ext[2] = {1, 1}, chOff[2] = {0, 0}, chExt[2] = {1, 1};
        bool haveExt = false;
        const int depth = x.depth();
        for (Event e; (e = x.next()) != Event::Eof && e != Event::Error;) {
            if (e == Event::End && x.depth() < depth)
                break;
            if (e != Event::Start)
                continue;
            const std::string_view n = x.localName();
            if (n == "grpSpPr") {
                const int d2 = x.depth();
                for (Event f; (f = x.next()) != Event::Eof && f != Event::Error;) {
                    if (f == Event::End && x.depth() < d2)
                        break;
                    if (f != Event::Start)
                        continue;
                    const std::string_view m = x.localName();
                    if (m == "off") {
                        off[0] = std::atof(x.attribute("x").c_str());
                        off[1] = std::atof(x.attribute("y").c_str());
                    } else if (m == "ext") {
                        ext[0] = std::atof(x.attribute("cx").c_str());
                        ext[1] = std::atof(x.attribute("cy").c_str());
                        haveExt = true;
                    } else if (m == "chOff") {
                        chOff[0] = std::atof(x.attribute("x").c_str());
                        chOff[1] = std::atof(x.attribute("y").c_str());
                    } else if (m == "chExt") {
                        chExt[0] = std::atof(x.attribute("cx").c_str());
                        chExt[1] = std::atof(x.attribute("cy").c_str());
                    }
                }
            } else if (n == "nvGrpSpPr") {
                x.skip();
            } else {
                // The children: in the group's child space, mapped onto its box.
                Xf inner = xf;
                if (haveExt && chExt[0] > 0 && chExt[1] > 0) {
                    const double kx = ext[0] / chExt[0], ky = ext[1] / chExt[1];
                    inner.sx = xf.sx * kx;
                    inner.sy = xf.sy * ky;
                    inner.ox = xf.ox + xf.sx * (off[0] - chOff[0] * kx);
                    inner.oy = xf.oy + xf.sy * (off[1] - chOff[1] * ky);
                }
                if (n == "sp" || n == "cxnSp")
                    shape(x, c, page, inner);
                else if (n == "pic")
                    picture(x, c, page, inner);
                else if (n == "grpSp")
                    group(x, c, page, inner);
                else if (n == "graphicFrame")
                    frame(x, c, page, inner);
                else if (n == "Fallback")
                    x.skip();
            }
        }
    }

    // At the Start of xfrm (a:xfrm or p:xfrm): position, size and flips.
    static void readXfrm(XmlReader& x, ShapeProps& p)
    {
        p.hasXfrm = true;
        p.flipH = x.attribute("flipH") == "1";
        p.flipV = x.attribute("flipV") == "1";
        p.rotation = float(std::atof(x.attribute("rot").c_str()) / 60000.0);
        const int depth = x.depth();
        for (Event e; (e = x.next()) != Event::Eof && e != Event::Error;) {
            if (e == Event::End && x.depth() < depth)
                break;
            if (e != Event::Start)
                continue;
            if (x.localName() == "off") {
                p.x = std::atof(x.attribute("x").c_str());
                p.y = std::atof(x.attribute("y").c_str());
            } else if (x.localName() == "ext") {
                p.w = std::atof(x.attribute("cx").c_str());
                p.h = std::atof(x.attribute("cy").c_str());
            }
        }
    }

    // At the Start of spPr.
    void readSpPr(XmlReader& x, const Context& c, ShapeProps& p)
    {
        const ooxml::Theme& theme = c.master->theme;
        const auto* map = &c.master->clrMap;
        const int depth = x.depth();
        for (Event e; (e = x.next()) != Event::Eof && e != Event::Error;) {
            if (e == Event::End && x.depth() < depth)
                break;
            if (e != Event::Start)
                continue;
            const std::string_view n = x.localName();
            if (n == "xfrm") {
                readXfrm(x, p);
            } else if (n == "prstGeom") {
                p.geometry = x.attribute("prst");
                x.skip();
            } else if (n == "custGeom") {
                p.geometry = "custom";
                customGeometry(x, p);
            } else if (n == "noFill") {
                p.hasFill = true;
                p.fill = 0;
            } else if (n == "solidFill") {
                bool found = false;
                p.fill = ooxml::readFillIn(x, theme, map, &found);
                p.hasFill = found;
                p.gradient = Gradient();
            } else if (n == "gradFill") {
                p.fill = ooxml::readGradientFill(x, theme, map, p.gradient);
                p.hasFill = !p.gradient.stops.empty();
            } else if (n == "blipFill" || n == "pattFill") {
                x.skip(); // an image or pattern fill: left unfilled
            } else if (n == "ln") {
                dml::readLine(x, theme, map, p.lineStyle);
                if (p.lineStyle.hasWidth)
                    p.lineWidth = p.lineStyle.width * kEmuPerPoint;
                if (p.lineStyle.hasColor) {
                    p.hasLine = true;
                    p.line = p.lineStyle.color;
                }
            }
        }
    }

    // At the Start of custGeom: its paths, normalised to 0..1 of the shape's box.
    static void customGeometry(XmlReader& x, ShapeProps& p)
    {
        double pw = 1, ph = 1;
        std::vector<Point>* current = nullptr;
        std::string op;
        std::vector<Point> pts;
        auto flush = [&]() {
            if (op.empty() || !current)
                return;
            auto norm = [&](Point q) { return Point{float(q.x / pw), float(q.y / ph)}; };
            if ((op == "moveTo" || op == "lnTo") && !pts.empty()) {
                if (op == "moveTo") {
                    if (!current->empty()) {
                        p.custom.emplace_back();
                        p.customClosed.push_back(false);
                        current = &p.custom.back();
                    }
                }
                current->push_back(norm(pts[0]));
            } else if (op == "cubicBezTo" && pts.size() == 3 && !current->empty()) {
                const Point a = current->back();
                const Point b = norm(pts[0]), cc = norm(pts[1]), d = norm(pts[2]);
                for (int i = 1; i <= 12; ++i) {
                    const float t = float(i) / 12.0f, u = 1.0f - t;
                    current->push_back({u * u * u * a.x + 3 * u * u * t * b.x + 3 * u * t * t * cc.x + t * t * t * d.x,
                                        u * u * u * a.y + 3 * u * u * t * b.y + 3 * u * t * t * cc.y + t * t * t * d.y});
                }
            } else if (op == "quadBezTo" && pts.size() == 2 && !current->empty()) {
                const Point a = current->back();
                const Point b = norm(pts[0]), d = norm(pts[1]);
                for (int i = 1; i <= 10; ++i) {
                    const float t = float(i) / 10.0f, u = 1.0f - t;
                    current->push_back({u * u * a.x + 2 * u * t * b.x + t * t * d.x, u * u * a.y + 2 * u * t * b.y + t * t * d.y});
                }
            }
            op.clear();
            pts.clear();
        };
        const int depth = x.depth();
        for (Event e; (e = x.next()) != Event::Eof && e != Event::Error;) {
            if (e == Event::End) {
                if (x.depth() < depth)
                    break;
                const std::string_view n = x.localName();
                if (n == "moveTo" || n == "lnTo" || n == "cubicBezTo" || n == "quadBezTo")
                    flush();
                continue;
            }
            if (e != Event::Start)
                continue;
            const std::string_view n = x.localName();
            if (n == "path") {
                pw = std::max(1.0, std::atof(x.attribute("w").c_str()));
                ph = std::max(1.0, std::atof(x.attribute("h").c_str()));
                p.custom.emplace_back();
                p.customClosed.push_back(false);
                current = &p.custom.back();
            } else if (n == "moveTo" || n == "lnTo" || n == "cubicBezTo" || n == "quadBezTo") {
                op = std::string(n);
                pts.clear();
            } else if (n == "pt") {
                pts.push_back({float(std::atof(x.attribute("x").c_str())), float(std::atof(x.attribute("y").c_str()))});
            } else if (n == "close" && !p.customClosed.empty()) {
                p.customClosed.back() = true;
            } else if (n == "arcTo" && current && !current->empty()) {
                // An arc: approximated by its end left out (the next segment joins); rare in practice.
            } else if (n == "gdLst" || n == "avLst" || n == "ahLst" || n == "cxnLst" || n == "rect") {
                x.skip();
            }
        }
        // Paths written with guide names instead of numbers read as 0: drop degenerate ones.
        for (std::size_t i = p.custom.size(); i-- > 0;)
            if (p.custom[i].size() < 2) {
                p.custom.erase(p.custom.begin() + long(i));
                p.customClosed.erase(p.customClosed.begin() + long(i));
            }
    }

    void readStyleRefs(XmlReader& x, const Context& c, ShapeProps& p)
    {
        const int depth = x.depth();
        for (Event e; (e = x.next()) != Event::Eof && e != Event::Error;) {
            if (e == Event::End && x.depth() < depth)
                break;
            if (e != Event::Start)
                continue;
            const std::string_view n = x.localName();
            if (n == "fillRef" || n == "lnRef" || n == "fontRef") {
                const int idx = std::atoi(x.attribute("idx").c_str());
                bool found = false;
                const std::uint32_t color = ooxml::readFillIn(x, c.master->theme, &c.master->clrMap, &found);
                if (!found)
                    continue;
                if (n == "fillRef" && idx > 0) {
                    // The theme's fill style number idx, in the referenced colour (else that colour).
                    std::uint32_t styled = color;
                    if (!ooxml::themeFill(c.master->theme, idx - 1, false, color, &c.master->clrMap, styled, p.styleGradient))
                        styled = color;
                    p.styleFill = styled;
                    p.hasStyleFill = true;
                } else if (n == "lnRef" && idx > 0) {
                    p.styleLine = color;
                    p.hasStyleLine = true;
                } else if (n == "fontRef") {
                    p.styleFont = color;
                    p.hasStyleFont = true;
                }
            }
        }
    }

    void emitGeometry(const ShapeProps& p, Rect r, Page& page)
    {
        PathItem path;
        path.fill = p.hasFill ? p.fill : p.hasStyleFill ? p.styleFill : 0;
        path.stroke = p.hasLine ? p.line : p.hasStyleLine ? p.styleLine : 0;
        path.lineWidth = float(std::max(0.25, p.lineWidth / kEmuPerPoint));
        if (p.hasFill)
            path.gradient = p.gradient;
        else if (p.hasStyleFill)
            path.gradient = p.styleGradient;
        dml::LineStyle style = p.lineStyle;
        style.width = path.lineWidth;
        path.dash = style.pattern();
        path.head = style.head;
        path.tail = style.tail;
        const std::string& g = p.geometry;
        const bool line = g == "line" || g.find("Connector") != std::string::npos;
        if (line) {
            path.fill = 0;
            if (!path.stroke)
                path.stroke = kBlack;
            std::vector<Point> pts;
            dml::connectorPath(g, r, p.flipH, p.flipV, pts);
            path.subpaths.push_back(std::move(pts));
            path.closed.push_back(false);
        } else if (!path.fill && !path.stroke) {
            return;
        } else if (g == "custom") {
            for (std::size_t i = 0; i < p.custom.size(); ++i) {
                std::vector<Point> pts;
                for (Point q : p.custom[i]) {
                    if (p.flipH)
                        q.x = 1.0f - q.x;
                    if (p.flipV)
                        q.y = 1.0f - q.y;
                    pts.push_back({r.x + q.x * r.w, r.y + q.y * r.h});
                }
                path.subpaths.push_back(std::move(pts));
                path.closed.push_back(p.customClosed[i]);
            }
            if (path.subpaths.empty())
                return;
        } else {
            std::vector<Point> pts;
            dml::presetOutline(g, r, pts);
            path.subpaths.push_back(std::move(pts));
            path.closed.push_back(true);
        }
        for (std::vector<Point>& sub : path.subpaths)
            dml::rotate(sub, r, p.rotation);
        page.add(std::move(path));
    }

    // At the Start of sp / cxnSp.
    void shape(XmlReader& x, const Context& c, Page& page, const Xf& xf)
    {
        ShapeProps p;
        std::string phType, phIdx;
        bool isPlaceholder = false;
        TextBox box;
        bool hasText = false;
        ShapeProps textArea; // SmartArt's dsp:txXfrm: the text's own rectangle
        const int depth = x.depth();
        // The text body comes after spPr: read it once the shape's placeholder and box are known.
        for (Event e; (e = x.next()) != Event::Eof && e != Event::Error;) {
            if (e == Event::End && x.depth() < depth)
                break;
            if (e != Event::Start)
                continue;
            const std::string_view n = x.localName();
            if (n == "ph") {
                isPlaceholder = true;
                phType = x.hasAttribute("type") ? x.attribute("type") : "obj";
                phIdx = x.attribute("idx");
                if (c.skipPlaceholders) {
                    x.skip();
                    // A master's or layout's placeholder: its prompt text is not slide content.
                    const int target = depth - 1;
                    for (Event f; (f = x.next()) != Event::Eof && f != Event::Error;)
                        if (f == Event::End && x.depth() == target)
                            break;
                    return;
                }
            } else if (n == "spPr") {
                readSpPr(x, c, p);
            } else if (n == "style") {
                readStyleRefs(x, c, p);
            } else if (n == "txBody") {
                hasText = textBody(x, c, phType, isPlaceholder, p, box, phIdx);
            } else if (n == "txXfrm") {
                readXfrm(x, textArea);
            }
        }
        // Position: its own, else the layout's placeholder, else the master's.
        Rect r;
        bool placed = false;
        if (p.hasXfrm) {
            r = xf.map(p.x, p.y, p.w, p.h);
            placed = true;
        } else if (isPlaceholder) {
            for (const Template* t : {static_cast<const Template*>(c.layout), static_cast<const Template*>(c.master)}) {
                if (!t)
                    continue;
                const Placeholder* found = nullptr;
                // A slide's placeholder matches its layout's by idx, the master's by type only: a
                // master numbers its date, footer and slide number 2-4, the idx a layout's body
                // placeholders use too.
                if (!phIdx.empty() && t == c.layout) {
                    const auto it = t->byIdx.find(phIdx);
                    if (it != t->byIdx.end() && it->second.hasBox)
                        found = &it->second;
                }
                if (!found) {
                    const auto it = t->byType.find(phKey(phType));
                    if (it != t->byType.end() && it->second.hasBox)
                        found = &it->second;
                }
                if (found) {
                    r = found->box;
                    placed = true;
                    if (box.anchor < 0 && found->anchor >= 0)
                        box.anchor = found->anchor;
                    break;
                }
            }
        }
        if (!placed)
            return;
        emitGeometry(p, r, page);
        if (hasText) {
            const bool vertical = std::fabs(std::fmod(box.rotation, 180.0f)) > 45.0f; // textBody's vert: 90 or 270
            box.box = textArea.hasXfrm && textArea.w > 0.0 && textArea.h > 0.0 ? xf.map(textArea.x, textArea.y, textArea.w, textArea.h)
                                                                                : vertical ? dml::turnedBox(r, 90.0f) : r;
            box.rotation += p.rotation;
            if (box.anchor < 0)
                box.anchor = (phKey(phType) == "title" && phType == "ctrTitle") ? 1 : (phKey(phType) == "title" ? 1 : 0);
            if ((phType == "title" || phType == "ctrTitle") && title_.empty())
                for (const TextParagraph& para : box.paragraphs)
                    for (const TextRun& run : para.runs) {
                        if (!title_.empty() && run.text == "\n")
                            title_ += ' ';
                        else if (run.text != "\n")
                            title_ += run.text;
                    }
            page.add(std::move(box));
        }
    }

    const TextStyle& styleFor(const Context& c, const std::string& phType, bool placeholder) const
    {
        if (phType == "title" || phType == "ctrTitle")
            return c.master->title;
        if (placeholder && (phType == "body" || phType == "obj" || phType == "subTitle"))
            return c.master->body;
        return c.master->other;
    }

    // The placeholder of a layout this shape inherits from (by idx, else type), or of a master (pass
    // no idx: masters match by type only), or null.
    static const Placeholder* inherited(const Template* t, const std::string& phType, const std::string& phIdx)
    {
        if (!t)
            return nullptr;
        if (!phIdx.empty())
            if (const auto it = t->byIdx.find(phIdx); it != t->byIdx.end())
                return &it->second;
        if (const auto it = t->byType.find(phKey(phType)); it != t->byType.end())
            return &it->second;
        return nullptr;
    }

    // At the Start of txBody: its paragraphs into `box`. True when it holds any text.
    bool textBody(XmlReader& x, const Context& c, const std::string& phType, bool placeholder, const ShapeProps& p, TextBox& box,
                  const std::string& phIdx = std::string())
    {
        // What the text looks like: the master's text styles, then the master's and the layout's
        // placeholder, then the shape's own lstStyle (read below), then each paragraph and run.
        TextStyle style = styleFor(c, phType, placeholder);
        if (placeholder) {
            if (const Placeholder* m = inherited(c.master, phType, std::string()))
                style.over(m->lst);
            if (const Placeholder* l = inherited(c.layout, phType, phIdx))
                style.over(l->lst);
        }
        // The shape's style (p:style/a:fontRef) colours its text over the master's text styles;
        // the shape's own lstStyle and the runs still override it.
        if (p.hasStyleFont)
            for (LevelStyle& level : style.levels) {
                level.color = p.styleFont;
                level.hasColor = true;
            }
        const bool bodyPh = placeholder && (phType == "body" || phType == "obj");
        const ooxml::Theme& theme = c.master->theme;
        const auto* map = &c.master->clrMap;
        std::string tx1 = "dk1";
        if (const auto it = map->find("tx1"); it != map->end())
            tx1 = it->second;
        const std::uint32_t defaultColor = p.hasStyleFont ? p.styleFont : theme.color(tx1, kBlack);
        box.anchor = -1;
        bool any = false;
        int autoNumber = 0;
        const int depth = x.depth();
        for (Event e; (e = x.next()) != Event::Eof && e != Event::Error;) {
            if (e == Event::End && x.depth() < depth)
                break;
            if (e != Event::Start)
                continue;
            const std::string_view n = x.localName();
            if (n == "bodyPr") {
                const std::string a = x.attribute("anchor");
                if (!a.empty())
                    box.anchor = a == "ctr" ? 1 : a == "b" ? 2 : 0;
                const std::string vert = x.attribute("vert");
                box.rotation = vert == "vert" || vert == "eaVert" || vert == "mongolianVert" ? 90.0f : vert == "vert270" ? 270.0f : 0.0f;
                box.rotation += float(std::atof(x.attribute("rot").c_str()) / 60000.0);
                auto inset = [&](const char* name, float& v) {
                    if (x.hasAttribute(name))
                        v = float(std::atof(x.attribute(name).c_str()) / kEmuPerPoint);
                };
                inset("lIns", box.insetL);
                inset("tIns", box.insetT);
                inset("rIns", box.insetR);
                inset("bIns", box.insetB);
                box.wrap = x.attribute("wrap") != "none";
                // normAutofit fontScale: the text shrunk to fit
                const int d2 = x.depth();
                for (Event f; (f = x.next()) != Event::Eof && f != Event::Error;) {
                    if (f == Event::End && x.depth() < d2)
                        break;
                    if (f == Event::Start && x.localName() == "normAutofit" && x.hasAttribute("fontScale"))
                        fontScale_ = float(std::atof(x.attribute("fontScale").c_str()) / 100000.0);
                }
            } else if (n == "lstStyle") {
                TextStyle own;
                readTextStyle(x, own, theme, map);
                style.over(own);
            } else if (n == "p") {
                TextParagraph para;
                LevelStyle direct; // the paragraph's own pPr
                int level = 0;
                std::string bullet; // as the paragraph says ("none" to remove)
                bool numbered = false;
                float endSize = 0.0f;
                const int d2 = x.depth();
                for (Event f; (f = x.next()) != Event::Eof && f != Event::Error;) {
                    if (f == Event::End && x.depth() < d2)
                        break;
                    if (f != Event::Start)
                        continue;
                    const std::string_view m = x.localName();
                    if (m == "pPr") {
                        level = std::clamp(std::atoi(x.attribute("lvl").c_str()), 0, 8);
                        direct.align = alignOf(x.attribute("algn"));
                        const int d3 = x.depth();
                        for (Event g; (g = x.next()) != Event::Eof && g != Event::Error;) {
                            if (g == Event::End && x.depth() < d3)
                                break;
                            if (g != Event::Start)
                                continue;
                            const std::string_view k = x.localName();
                            if (k == "buNone")
                                bullet = "none";
                            else if (k == "buChar")
                                bullet = x.attribute("char");
                            else if (k == "buAutoNum")
                                numbered = true;
                            else if (k == "lnSpc" || k == "spcBef")
                                readSpacing(x, k == "lnSpc", direct);
                            else if (k == "spcAft" || k == "defRPr" || k == "tabLst")
                                x.skip();
                        }
                    } else if (m == "r" || m == "fld") {
                        const bool slideNumber = m == "fld" && x.attribute("type") == "slidenum";
                        LevelStyle ls = style.levels[level];
                        ls.over(direct);
                        TextRun run = textRun(x, ls, defaultColor, theme, map);
                        if (slideNumber) // the field shows the slide it is on
                            run.text = std::to_string(slideNumber_);
                        if (!run.text.empty())
                            para.runs.push_back(std::move(run));
                    } else if (m == "br") {
                        TextRun run;
                        run.text = "\n";
                        run.size = style.levels[level].size > 0 ? style.levels[level].size : 18.0f;
                        para.runs.push_back(run);
                        x.skip();
                    } else if (m == "endParaRPr") {
                        if (x.hasAttribute("sz"))
                            endSize = float(std::atof(x.attribute("sz").c_str()) / 100.0);
                        x.skip();
                    }
                }
                para.level = level;
                LevelStyle ls = style.levels[level];
                ls.over(direct);
                para.align = std::max(0, ls.align);
                if (ls.lineSpacing > 0.0f)
                    para.lineSpacing = ls.lineSpacing;
                if (ls.spaceBefore >= 0.0f)
                    para.spaceBefore = ls.spaceBefore;
                else if (ls.spaceBeforePct >= 0.0f)
                    para.spaceBefore = ls.spaceBeforePct * (ls.size > 0.0f ? ls.size : 18.0f);
                std::string b = bullet.empty() ? ls.bullet : bullet;
                if (numbered || (bullet.empty() && b == "auto")) {
                    para.bullet = std::to_string(++autoNumber) + ".";
                } else {
                    autoNumber = 0;
                    if (!b.empty() && b != "none" && b != "auto" && (bodyPh || !bullet.empty()))
                        para.bullet = b;
                }
                if (!para.runs.empty())
                    any = true;
                if (para.runs.empty()) { // an empty line keeps its height
                    TextRun run;
                    run.size = endSize > 0 ? endSize : ls.size > 0 ? ls.size : 18.0f;
                    para.runs.push_back(run);
                    para.bullet.clear();
                }
                if (fontScale_ < 1.0f)
                    for (TextRun& run : para.runs)
                        run.size *= fontScale_;
                box.paragraphs.push_back(std::move(para));
            }
        }
        fontScale_ = 1.0f;
        return any;
    }

    // At the Start of a:r / a:fld.
    TextRun textRun(XmlReader& x, const LevelStyle& level, std::uint32_t defaultColor, const ooxml::Theme& theme,
                    const std::map<std::string, std::string>* map)
    {
        TextRun run;
        run.size = level.size > 0 ? level.size : 18.0f;
        run.color = level.hasColor ? level.color : defaultColor;
        if (level.bold == 1)
            run.flags |= Bold;
        if (level.italic == 1)
            run.flags |= Italic;
        bool caps = level.caps == 1, link = false;
        const int depth = x.depth();
        for (Event e; (e = x.next()) != Event::Eof && e != Event::Error;) {
            if (e == Event::End && x.depth() < depth)
                break;
            if (e != Event::Start)
                continue;
            const std::string_view n = x.localName();
            if (n == "rPr") {
                if (x.hasAttribute("sz"))
                    run.size = std::max(1.0f, float(std::atof(x.attribute("sz").c_str()) / 100.0));
                if (x.hasAttribute("b"))
                    run.flags = std::uint8_t(x.attribute("b") == "1" ? run.flags | Bold : run.flags & ~Bold);
                if (x.attribute("i") == "1")
                    run.flags |= Italic;
                if (!x.attribute("u").empty() && x.attribute("u") != "none")
                    run.flags |= Underline;
                if (!x.attribute("strike").empty() && x.attribute("strike") != "noStrike")
                    run.flags |= Strike;
                if (x.hasAttribute("cap"))
                    caps = x.attribute("cap") == "all";
                const int d2 = x.depth();
                for (Event f; (f = x.next()) != Event::Eof && f != Event::Error;) {
                    if (f == Event::End && x.depth() < d2)
                        break;
                    if (f != Event::Start)
                        continue;
                    const std::string_view m = x.localName();
                    if (m == "solidFill") {
                        bool found = false;
                        const std::uint32_t c = ooxml::readFillIn(x, theme, map, &found);
                        if (found)
                            run.color = c;
                    } else if (m == "latin") {
                        const std::string face = x.attribute("typeface");
                        if (face.find("Courier") != std::string::npos || face.find("Consolas") != std::string::npos ||
                            face.find("Mono") != std::string::npos)
                            run.flags |= Mono;
                    } else if (m == "hlinkClick") {
                        link = !x.attribute("r:id").empty();
                        x.skip();
                    } else {
                        x.skip();
                    }
                }
            } else if (n == "t") {
                run.text += x.elementText();
            }
        }
        if (link) { // a hyperlink: the theme's link colour, underlined, as PowerPoint draws it
            run.color = theme.color("hlink", run.color);
            run.flags |= Underline;
        }
        if (caps)
            upperCase(run.text);
        return run;
    }

    // `cap="all"`: ASCII and Latin-1 letters in capitals (the rest as written).
    static void upperCase(std::string& s)
    {
        for (std::size_t i = 0; i < s.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            if (c >= 'a' && c <= 'z') {
                s[i] = char(c - 32);
            } else if (c == 0xC3 && i + 1 < s.size()) { // U+00E0-U+00FE (but U+00F7, the division sign)
                const unsigned char d = static_cast<unsigned char>(s[i + 1]);
                if (d >= 0xA0 && d <= 0xBE && d != 0xB7)
                    s[i + 1] = char(d - 0x20);
                ++i;
            }
        }
    }

    // At the Start of pic.
    void picture(XmlReader& x, const Context& c, Page& page, const Xf& xf)
    {
        ShapeProps p;
        std::string embed;
        bool video = false;
        const int depth = x.depth();
        for (Event e; (e = x.next()) != Event::Eof && e != Event::Error;) {
            if (e == Event::End && x.depth() < depth)
                break;
            if (e != Event::Start)
                continue;
            const std::string_view n = x.localName();
            if (n == "blip")
                embed = x.attribute("r:embed");
            else if (n == "srcRect") {
                auto pct = [&](const char* name) { return float(std::atof(x.attribute(name).c_str()) / 100000.0); };
                const float l = pct("l"), t = pct("t"), r = pct("r"), b = pct("b");
                if (l + r < 0.99f && t + b < 0.99f)
                    p.crop = Rect{l, t, 1.0f - l - r, 1.0f - t - b};
            } else if (n == "spPr")
                readSpPr(x, c, p);
            else if (n == "videoFile" || n == "audioFile" || n == "media")
                video = true;
        }
        if (video)
            ++media_;
        if (!p.hasXfrm || embed.empty() || !c.rels)
            return;
        const auto it = c.rels->find(embed);
        if (it == c.rels->end() || it->second.external || !addMedia(it->second.target))
            return;
        ImageItem img;
        img.box = xf.map(p.x, p.y, p.w, p.h);
        img.media = it->second.target;
        img.crop = p.crop;
        img.rotation = p.rotation;
        img.flipH = p.flipH;
        img.flipV = p.flipV;
        page.add(img);
        if (p.hasLine && p.line) {
            ShapeProps outline = p;
            outline.hasFill = true;
            outline.fill = 0;
            outline.geometry = "rect";
            emitGeometry(outline, img.box, page);
        }
    }

    // At the Start of graphicFrame: a table, or a chart / diagram as a labelled box.
    void frame(XmlReader& x, const Context& c, Page& page, const Xf& xf)
    {
        ShapeProps p;
        const int depth = x.depth();
        for (Event e; (e = x.next()) != Event::Eof && e != Event::Error;) {
            if (e == Event::End && x.depth() < depth)
                break;
            if (e != Event::Start)
                continue;
            const std::string_view n = x.localName();
            if (n == "xfrm") {
                readXfrm(x, p);
            } else if (n == "tbl" && p.hasXfrm) {
                table(x, c, page, xf.map(p.x, p.y, p.w, p.h), xf);
            } else if (n == "chart" && p.hasXfrm) {
                const Rect r = xf.map(p.x, p.y, p.w, p.h);
                chart::Chart chart;
                bool read = false;
                if (c.rels)
                    if (const auto it = c.rels->find(x.attribute("r:id")); it != c.rels->end() && !it->second.external)
                        read = chart::read(zip_, it->second.target, c.master->theme, chart);
                if (read) {
                    chart::draw(chart, c.master->theme, r, page);
                } else {
                    ++charts_;
                    placeholder(page, r, "Chart");
                }
                x.skip();
            } else if (n == "relIds" && p.hasXfrm) {
                const std::string data = x.attribute("r:dm");
                x.skip();
                if (!diagram(data, c, page, xf, p)) {
                    ++diagrams_;
                    if (!diagramText(data, c, page, xf.map(p.x, p.y, p.w, p.h)))
                        placeholder(page, xf.map(p.x, p.y, p.w, p.h), "SmartArt diagram");
                }
            } else if (n == "oleObj" && p.hasXfrm) {
                placeholder(page, xf.map(p.x, p.y, p.w, p.h), "Embedded object");
                x.skip();
            }
        }
    }

    // A labelled box where something is not drawn.
    static void placeholder(Page& page, Rect r, const char* label)
    {
        PathItem frameBox;
        frameBox.fill = rgba(128, 128, 128, 30);
        frameBox.stroke = rgba(128, 128, 128, 140);
        frameBox.subpaths.push_back({{r.x, r.y}, {r.x + r.w, r.y}, {r.x + r.w, r.y + r.h}, {r.x, r.y + r.h}});
        frameBox.closed.push_back(true);
        page.add(std::move(frameBox));
        TextBox t;
        t.box = r;
        t.anchor = 1;
        TextParagraph para;
        para.align = 1;
        TextRun run;
        run.text = std::string(label) + " (not shown yet)";
        run.size = 14.0f;
        run.color = rgba(110, 110, 110);
        para.runs.push_back(run);
        t.paragraphs.push_back(para);
        page.add(std::move(t));
    }

    // SmartArt without a drawing of it (written by other programs): its text as an outline in the
    // frame, by the data model's parent-child links, so what it says is there to read.
    bool diagramText(const std::string& dataId, const Context& c, Page& page, Rect r)
    {
        if (!c.rels)
            return false;
        const auto it = c.rels->find(dataId);
        if (it == c.rels->end() || it->second.external)
            return false;
        const std::optional<std::string> data = zip_.read(it->second.target, std::size_t(16) << 20);
        if (!data)
            return false;
        struct Node {
            std::string type, text;
            std::vector<std::pair<int, std::string>> children; // (order, id)
        };
        std::map<std::string, Node> nodes;
        std::string root;
        XmlReader d(*data);
        for (Event e; (e = d.next()) != Event::Eof && e != Event::Error;) {
            if (e != Event::Start)
                continue;
            const std::string_view n = d.localName();
            if (n == "pt") {
                const std::string id = d.attribute("modelId");
                Node& node = nodes[id];
                node.type = d.hasAttribute("type") ? d.attribute("type") : "node";
                if (node.type == "doc")
                    root = id;
                // Its dgm:t holds the text: a:p paragraphs of a:r runs of a:t.
                const int depth = d.depth();
                for (Event f; (f = d.next()) != Event::Eof && f != Event::Error;) {
                    if (f == Event::End && d.depth() < depth)
                        break;
                    if (f != Event::Start || d.localName() != "t" || d.depth() != depth + 1)
                        continue;
                    const int textDepth = d.depth();
                    for (Event g; (g = d.next()) != Event::Eof && g != Event::Error;) {
                        if (g == Event::End && d.depth() < textDepth)
                            break;
                        if (g != Event::Start)
                            continue;
                        if (d.localName() == "p" && !node.text.empty())
                            node.text += ' ';
                        else if (d.localName() == "t")
                            node.text += d.elementText();
                    }
                }
            } else if (n == "cxn") {
                const std::string type = d.hasAttribute("type") ? d.attribute("type") : "parOf";
                if (type == "parOf")
                    nodes[d.attribute("srcId")].children.push_back({std::atoi(d.attribute("srcOrd").c_str()), d.attribute("destId")});
            }
        }
        if (root.empty())
            return false;
        TextBox box;
        box.box = r;
        box.anchor = 1;
        const ooxml::Theme& theme = c.master->theme;
        int lines = 0;
        std::function<void(const std::string&, int)> walk = [&](const std::string& id, int level) {
            if (level > 8 || lines > 200)
                return;
            auto found = nodes.find(id);
            if (found == nodes.end())
                return;
            std::vector<std::pair<int, std::string>> children = found->second.children;
            std::sort(children.begin(), children.end());
            for (const auto& [order, child] : children) {
                const auto node = nodes.find(child);
                if (node == nodes.end() || node->second.type != "node")
                    continue;
                if (!node->second.text.empty()) {
                    TextParagraph para;
                    para.level = level;
                    para.bullet = level == 0 ? "\xE2\x96\xA0" : "\xE2\x80\xA2";
                    TextRun run;
                    run.text = node->second.text;
                    run.size = level == 0 ? 16.0f : 13.0f;
                    run.color = theme.color("dk1", kBlack);
                    run.flags = level == 0 ? Bold : 0;
                    para.runs.push_back(std::move(run));
                    box.paragraphs.push_back(std::move(para));
                    ++lines;
                }
                walk(child, level + 1);
            }
        };
        walk(root, 0);
        if (box.paragraphs.empty())
            return false;
        PathItem frame;
        frame.fill = ooxml::lumModOff(theme.color("accent1", rgba(68, 114, 196)), 0.2, 0.8);
        frame.stroke = theme.color("accent1", rgba(68, 114, 196));
        frame.subpaths.push_back({{r.x, r.y}, {r.x + r.w, r.y}, {r.x + r.w, r.y + r.h}, {r.x, r.y + r.h}});
        frame.closed.push_back(true);
        page.add(std::move(frame));
        page.add(std::move(box));
        return true;
    }

    // SmartArt: PowerPoint keeps the diagram drawn as plain shapes (a "diagramDrawing" part, in the
    // frame's coordinates), which are drawn like the slide's own. The data part names it.
    bool diagram(const std::string& dataId, const Context& c, Page& page, const Xf& xf, const ShapeProps& frame)
    {
        if (!c.rels)
            return false;
        std::string drawing;
        if (const auto it = c.rels->find(dataId); it != c.rels->end() && !it->second.external)
            if (const std::optional<std::string> data = zip_.read(it->second.target, std::size_t(16) << 20)) {
                XmlReader d(*data);
                for (Event e; (e = d.next()) != Event::Eof && e != Event::Error;)
                    if (e == Event::Start && d.localName() == "dataModelExt") {
                        const auto rel = c.rels->find(d.attribute("relId"));
                        if (rel != c.rels->end() && !rel->second.external)
                            drawing = rel->second.target;
                        break;
                    }
            }
        if (drawing.empty()) { // no link in the data part: the slide's only diagram drawing
            int found = 0;
            for (const auto& [id, r] : *c.rels)
                if (r.type == "diagramDrawing") {
                    drawing = r.target;
                    ++found;
                }
            if (found != 1)
                return false;
        }
        const std::optional<std::string> xml = zip_.read(drawing, std::size_t(16) << 20);
        if (!xml)
            return false;
        const ooxml::Relationships rels = ooxml::readRelationships(zip_, drawing);
        Context inner = c;
        inner.rels = &rels;
        inner.skipPlaceholders = false;
        Xf at = xf;
        at.ox = xf.ox + frame.x * xf.sx;
        at.oy = xf.oy + frame.y * xf.sy;
        XmlReader d(*xml);
        const std::size_t before = page.items.size();
        for (Event e; (e = d.next()) != Event::Eof && e != Event::Error;)
            if (e == Event::Start && d.localName() == "spTree") {
                tree(d, inner, page, at);
                break;
            }
        return page.items.size() > before;
    }

    // At the Start of a:tbl.
    // The table styles of the presentation (ppt/tableStyles.xml), read once.
    const TableStyle* tableStyle(const std::string& id, const ooxml::Theme& theme, const std::map<std::string, std::string>* map)
    {
        if (!tableStylesRead_) {
            tableStylesRead_ = true;
            const std::string part = ooxml::targetOfType(presentationRels_, "tableStyles");
            if (const std::optional<std::string> xml = part.empty() ? std::nullopt : zip_.read(part, std::size_t(8) << 20)) {
                XmlReader x(*xml);
                TableStyle* style = nullptr;
                TablePart* cur = nullptr;
                bool inText = false, inFill = false;
                for (Event e; (e = x.next()) != Event::Eof && e != Event::Error;) {
                    if (e == Event::End) {
                        const std::string_view n = x.localName();
                        if (n == "tcTxStyle")
                            inText = false;
                        else if (n == "fill")
                            inFill = false;
                        continue;
                    }
                    if (e != Event::Start)
                        continue;
                    const std::string_view n = x.localName();
                    if (n == "tblStyle") {
                        style = &tableStyles_[x.attribute("styleId")];
                    } else if (style && (n == "wholeTbl" || n == "firstRow" || n == "lastRow" || n == "firstCol" || n == "lastCol" ||
                                         n == "band1H" || n == "band2H" || n == "band1V" || n == "band2V")) {
                        cur = &style->parts[std::string(n)];
                    } else if (cur && n == "tcTxStyle") {
                        inText = true;
                        if (x.attribute("b") == "on")
                            cur->bold = 1;
                    } else if (cur && n == "fill") {
                        inFill = true;
                    } else if (cur && inFill && (n == "solidFill" || n == "noFill")) {
                        bool found = false;
                        cur->fill = n == "noFill" ? 0 : ooxml::readFillIn(x, theme, map, &found);
                        cur->hasFill = true;
                    } else if (cur && inText && (n == "schemeClr" || n == "srgbClr" || n == "prstClr" || n == "sysClr")) {
                        cur->text = ooxml::readColor(x, theme, map);
                        cur->hasText = true;
                    } else if (cur && (n == "tcBdr" || n == "effect")) {
                        x.skip();
                    }
                }
            }
        }
        if (const auto it = tableStyles_.find(id); it != tableStyles_.end())
            return &it->second;
        if (id.empty())
            return nullptr;
        // A built-in style the file does not define: Office's default look, Medium Style 2 in accent 1
        // (a strong header row, light banded rows, white lines between cells).
        TableStyle& built = tableStyles_[id];
        const std::uint32_t accent = theme.color("accent1", rgba(68, 114, 196)), light = theme.color("lt1", kWhite);
        built.parts["wholeTbl"] = TablePart{ooxml::lumModOff(accent, 0.2, 0.8), true, 0, false, -1};
        built.parts["band1H"] = TablePart{ooxml::lumModOff(accent, 0.4, 0.6), true, 0, false, -1};
        built.parts["band1V"] = built.parts["band1H"];
        for (const char* strong : {"firstRow", "lastRow", "firstCol", "lastCol"})
            built.parts[strong] = TablePart{accent, true, light, true, 1};
        built.border = light;
        return &built;
    }

    void table(XmlReader& x, const Context& c, Page& page, Rect r, const Xf& xf)
    {
        std::vector<float> columns;
        std::string styleId;
        bool firstRow = false, lastRow = false, firstCol = false, lastCol = false, bandRow = false, bandCol = false;
        std::vector<float> heights;
        struct CellBox {
            int row = 0, col = 0, span = 1, rowSpan = 1;
            bool merged = false;
            std::uint32_t fill = 0;
            bool hasFill = false; // its own (tcPr), over the table style's
            TextBox text;
            bool hasText = false;
        };
        std::vector<CellBox> cells;
        int rowIndex = -1, colIndex = 0;
        const int depth = x.depth();
        for (Event e; (e = x.next()) != Event::Eof && e != Event::Error;) {
            if (e == Event::End && x.depth() < depth)
                break;
            if (e != Event::Start)
                continue;
            const std::string_view n = x.localName();
            if (n == "tblPr") {
                auto flag = [&](const char* a) { return x.attribute(a) == "1" || x.attribute(a) == "true"; };
                firstRow = flag("firstRow");
                lastRow = flag("lastRow");
                firstCol = flag("firstCol");
                lastCol = flag("lastCol");
                bandRow = flag("bandRow");
                bandCol = flag("bandCol");
            } else if (n == "tableStyleId") {
                styleId = x.elementText();
            } else if (n == "gridCol") {
                columns.push_back(float(std::atof(x.attribute("w").c_str()) * xf.sx));
            } else if (n == "tr") {
                ++rowIndex;
                colIndex = 0;
                heights.push_back(float(std::atof(x.attribute("h").c_str()) * xf.sy));
            } else if (n == "tc") {
                CellBox cell;
                cell.row = rowIndex;
                cell.col = colIndex;
                cell.span = std::max(1, std::atoi(x.attribute("gridSpan").c_str()));
                cell.rowSpan = std::max(1, std::atoi(x.attribute("rowSpan").c_str()));
                cell.merged = x.attribute("hMerge") == "1" || x.attribute("vMerge") == "1";
                ++colIndex;
                const int d2 = x.depth();
                for (Event f; (f = x.next()) != Event::Eof && f != Event::Error;) {
                    if (f == Event::End && x.depth() < d2)
                        break;
                    if (f != Event::Start)
                        continue;
                    if (x.localName() == "txBody") {
                        ShapeProps none;
                        cell.hasText = textBody(x, c, "", false, none, cell.text);
                    } else if (x.localName() == "tcPr") {
                        const int d3 = x.depth();
                        for (Event g; (g = x.next()) != Event::Eof && g != Event::Error;) {
                            if (g == Event::End && x.depth() < d3)
                                break;
                            if (g != Event::Start)
                                continue;
                            if ((x.localName() == "solidFill" || x.localName() == "noFill") && x.depth() == d3 + 1) {
                                bool found = false;
                                cell.fill = x.localName() == "noFill" ? 0 : ooxml::readFillIn(x, c.master->theme, &c.master->clrMap, &found);
                                cell.hasFill = true;
                            } else if (x.depth() == d3 + 1) {
                                x.skip(); // borders (lnL, lnR, ...): drawn as thin lines below
                            }
                        }
                    }
                }
                cells.push_back(std::move(cell));
            }
        }
        if (columns.empty() || heights.empty())
            return;
        std::vector<float> xs(columns.size() + 1, r.x), ys(heights.size() + 1, r.y);
        for (std::size_t i = 0; i < columns.size(); ++i)
            xs[i + 1] = xs[i] + columns[i];
        for (std::size_t i = 0; i < heights.size(); ++i)
            ys[i + 1] = ys[i] + std::max(heights[i], 8.0f);
        const TableStyle* style = tableStyle(styleId, c.master->theme, &c.master->clrMap);
        const int rowCount = int(heights.size()), columnCount = int(columns.size());
        std::string tx1 = "dk1";
        if (const auto it = c.master->clrMap.find("tx1"); it != c.master->clrMap.end())
            tx1 = it->second;
        const std::uint32_t defaultText = c.master->theme.color(tx1, kBlack);
        for (CellBox& cell : cells) {
            if (cell.merged || cell.col >= int(columns.size()))
                continue;
            const int c1 = std::min(int(columns.size()), cell.col + cell.span);
            const int r1 = std::min(int(heights.size()), cell.row + cell.rowSpan);
            const Rect box{xs[size_t(cell.col)], ys[size_t(cell.row)], xs[size_t(c1)] - xs[size_t(cell.col)], ys[size_t(r1)] - ys[size_t(cell.row)]};
            // The table style's parts, weakest first: the whole table, bands, first / last column, first / last row.
            TablePart look;
            if (style) {
                auto apply = [&](const char* name) {
                    const auto it = style->parts.find(name);
                    if (it == style->parts.end())
                        return;
                    if (it->second.hasFill)
                        look.fill = it->second.fill, look.hasFill = true;
                    if (it->second.hasText)
                        look.text = it->second.text, look.hasText = true;
                    if (it->second.bold >= 0)
                        look.bold = it->second.bold;
                };
                apply("wholeTbl");
                const int bodyRow = cell.row - (firstRow ? 1 : 0), bodyCol = cell.col - (firstCol ? 1 : 0);
                if (bandCol && bodyCol >= 0)
                    apply(bodyCol % 2 == 0 ? "band1V" : "band2V");
                if (bandRow && bodyRow >= 0)
                    apply(bodyRow % 2 == 0 ? "band1H" : "band2H");
                if (firstCol && cell.col == 0)
                    apply("firstCol");
                if (lastCol && c1 == columnCount)
                    apply("lastCol");
                if (firstRow && cell.row == 0)
                    apply("firstRow");
                if (lastRow && r1 == rowCount)
                    apply("lastRow");
            }
            if (look.hasText || look.bold == 1)
                for (TextParagraph& para : cell.text.paragraphs)
                    for (TextRun& run : para.runs) {
                        if (look.hasText && run.color == defaultText)
                            run.color = look.text;
                        if (look.bold == 1)
                            run.flags |= Bold;
                    }
            PathItem bg;
            bg.fill = cell.hasFill ? cell.fill : look.hasFill ? look.fill : 0;
            bg.stroke = style && style->border ? style->border : rgba(160, 160, 160);
            bg.lineWidth = 0.75f;
            bg.subpaths.push_back({{box.x, box.y}, {box.x + box.w, box.y}, {box.x + box.w, box.y + box.h}, {box.x, box.y + box.h}});
            bg.closed.push_back(true);
            page.add(std::move(bg));
            if (cell.hasText) {
                cell.text.box = box;
                if (cell.text.anchor < 0)
                    cell.text.anchor = 0;
                page.add(std::move(cell.text));
            }
        }
    }

    bool addMedia(const std::string& part)
    {
        if (doc_->pages.media.count(part))
            return true;
        std::optional<std::string> bytes = zip_.read(part, std::size_t(64) << 20);
        if (!bytes)
            return false;
        doc_->pages.media[part] = std::move(*bytes);
        return true;
    }

    std::string notesText(const std::string& part)
    {
        const std::optional<std::string> xml = zip_.read(part);
        if (!xml)
            return {};
        std::string out;
        XmlReader x(*xml);
        bool inBody = false;
        int spDepth = -1;
        for (Event e; (e = x.next()) != Event::Eof && e != Event::Error;) {
            if (e == Event::End) {
                if (x.localName() == "sp" && x.depth() + 1 == spDepth) {
                    inBody = false;
                    spDepth = -1;
                } else if (x.localName() == "p" && inBody && !out.empty() && out.back() != '\n') {
                    out += '\n';
                }
                continue;
            }
            if (e != Event::Start)
                continue;
            if (x.localName() == "sp") {
                spDepth = x.depth();
            } else if (x.localName() == "ph" && spDepth > 0) {
                inBody = x.attribute("type") == "body";
            } else if (x.localName() == "t" && inBody) {
                out += x.elementText();
            }
        }
        while (!out.empty() && out.back() == '\n')
            out.pop_back();
        return out;
    }

    ooxml::ZipArchive& zip_;
    ViewerDocument* doc_ = nullptr;
    std::map<std::string, std::unique_ptr<Master>> masters_;
    std::map<std::string, std::unique_ptr<Template>> layouts_;
    std::string title_;
    float fontScale_ = 1.0f;
    int slideNumber_ = 0; // the slide being read, from 1 (slide-number fields)
    ooxml::Relationships presentationRels_;
    bool tableStylesRead_ = false;
    std::map<std::string, TableStyle> tableStyles_;
    int charts_ = 0, diagrams_ = 0, media_ = 0;
};

} // namespace

ViewerDocument readPptx(std::string bytes)
{
    ooxml::ZipArchive zip;
    std::string error;
    if (!zip.open(std::move(bytes), &error)) {
        ViewerDocument doc;
        doc.format = "PowerPoint presentation";
        doc.error = error;
        return doc;
    }
    return PptxReader(zip).read();
}

} // namespace enp::model::viewer
