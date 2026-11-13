#include "enp/model/viewer/Viewer.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace enp::model::viewer {

std::uint32_t Gradient::at(float t) const
{
    if (stops.empty())
        return kNone;
    if (t <= stops.front().position)
        return stops.front().color;
    for (std::size_t i = 1; i < stops.size(); ++i) {
        const GradientStop& a = stops[i - 1];
        const GradientStop& b = stops[i];
        if (t > b.position)
            continue;
        const float span = b.position - a.position;
        const float k = span > 1e-6f ? (t - a.position) / span : 1.0f;
        std::uint32_t out = 0;
        for (int shift = 0; shift < 32; shift += 8) {
            const float ca = float((a.color >> shift) & 0xFF), cb = float((b.color >> shift) & 0xFF);
            out |= std::uint32_t(ca + (cb - ca) * k + 0.5f) << shift;
        }
        return out;
    }
    return stops.back().color;
}

namespace {

struct Edge {
    float x0, y0, x1, y1; // y0 < y1
    int dir;              // +1 drawn downwards, -1 upwards
    float at(float y) const { return x0 + (x1 - x0) * (y - y0) / (y1 - y0); }
};

// One band of the sweep, [ya, yb], crossed by `active` edges: its spans by the fill rule. Edges
// that cross inside the band split it in two (a self-crossing outline), down to a small height.
void bandSpans(const std::vector<const Edge*>& active, float ya, float yb, bool evenOdd, int depth, std::vector<std::pair<float, const Edge*>>& order,
               std::vector<FillBand>& out)
{
    const float ym = (ya + yb) * 0.5f;
    order.clear();
    for (const Edge* e : active)
        order.emplace_back(e->at(ym), e);
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    if (depth < 10 && yb - ya > 0.02f)
        for (std::size_t i = 1; i < order.size(); ++i) {
            const Edge& a = *order[i - 1].second;
            const Edge& b = *order[i].second;
            if (a.at(ya) > b.at(ya) + 0.01f || a.at(yb) > b.at(yb) + 0.01f) {
                std::vector<std::pair<float, const Edge*>> inner;
                bandSpans(active, ya, ym, evenOdd, depth + 1, inner, out);
                bandSpans(active, ym, yb, evenOdd, depth + 1, inner, out);
                return;
            }
        }
    int winding = 0;
    const Edge* left = nullptr;
    for (const auto& [x, e] : order) {
        const bool before = evenOdd ? (winding & 1) != 0 : winding != 0;
        winding += evenOdd ? 1 : e->dir;
        const bool after = evenOdd ? (winding & 1) != 0 : winding != 0;
        if (!before && after) {
            left = e;
        } else if (before && !after && left) {
            out.push_back(FillBand{ya, yb, left->at(ya), e->at(ya), left->at(yb), e->at(yb)});
            left = nullptr;
        }
    }
}

// The outlines' fill as bands: a sweep down the page from vertex height to vertex height. Too
// big a path keeps no bands (each outline is filled on its own, as before).
void fillBands(PathItem& p)
{
    constexpr std::size_t kMaxEdges = 50000, kMaxBands = 200000;
    std::vector<Edge> edges;
    std::vector<float> ys;
    for (const std::vector<Point>& sub : p.subpaths) {
        if (sub.size() < 3)
            continue;
        for (std::size_t i = 0; i < sub.size(); ++i) {
            const Point& a = sub[i];
            const Point& b = sub[(i + 1) % sub.size()]; // closed for filling
            if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(b.x) || !std::isfinite(b.y) || a.y == b.y)
                continue;
            edges.push_back(a.y < b.y ? Edge{a.x, a.y, b.x, b.y, 1} : Edge{b.x, b.y, a.x, a.y, -1});
            ys.push_back(a.y);
        }
        if (edges.size() > kMaxEdges)
            return;
    }
    if (edges.empty())
        return;
    std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) { return a.y0 < b.y0; });
    std::sort(ys.begin(), ys.end());
    ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
    std::vector<FillBand> bands;
    std::vector<const Edge*> active;
    std::vector<std::pair<float, const Edge*>> order;
    std::size_t next = 0;
    for (std::size_t i = 0; i + 1 < ys.size(); ++i) {
        const float ya = ys[i], yb = ys[i + 1];
        active.erase(std::remove_if(active.begin(), active.end(), [ya](const Edge* e) { return e->y1 <= ya; }), active.end());
        while (next < edges.size() && edges[next].y0 <= ya)
            active.push_back(&edges[next++]);
        if (active.size() >= 2)
            bandSpans(active, ya, yb, p.evenOdd, 0, order, bands);
        if (bands.size() > kMaxBands)
            return;
    }
    bands.shrink_to_fit();
    p.bands = std::move(bands);
}

} // namespace

void Page::add(PathItem p)
{
    if (p.fill != kNone && p.subpaths.size() > 1)
        fillBands(p);
    items.push_back({PageItem::Kind::Path, std::uint32_t(paths.size()), clip});
    paths.push_back(std::move(p));
}

void Page::add(GlyphRun g)
{
    items.push_back({PageItem::Kind::Glyphs, std::uint32_t(glyphs.size()), clip});
    glyphs.push_back(std::move(g));
}

void Page::add(TextBox b)
{
    items.push_back({PageItem::Kind::Box, std::uint32_t(boxes.size()), clip});
    boxes.push_back(std::move(b));
}

void Page::add(ImageItem i)
{
    items.push_back({PageItem::Kind::Image, std::uint32_t(images.size()), clip});
    images.push_back(std::move(i));
}

std::string Page::plainText() const
{
    std::string out;
    // Glyph runs in reading order: by line (baseline, within half a size), then left to right.
    std::vector<const GlyphRun*> runs; // invisible text too (the words over a scanned page)
    for (const GlyphRun& g : glyphs)
        runs.push_back(&g);
    std::stable_sort(runs.begin(), runs.end(), [](const GlyphRun* a, const GlyphRun* b) {
        if (std::abs(a->baseline - b->baseline) > std::min(a->size, b->size) * 0.5f)
            return a->baseline < b->baseline;
        return a->x < b->x;
    });
    const GlyphRun* previous = nullptr;
    for (const GlyphRun* g : runs) {
        if (previous) {
            if (std::abs(g->baseline - previous->baseline) > std::min(g->size, previous->size) * 0.5f)
                out += '\n';
            else if (g->x > previous->x + previous->width + g->size * 0.15f && !out.empty() && out.back() != ' ')
                out += ' ';
        }
        out += g->text;
        previous = g;
    }
    for (const TextBox& b : boxes)
        for (const TextParagraph& p : b.paragraphs) {
            std::string line;
            for (const TextRun& r : p.runs)
                line += r.text == "\n" ? std::string(" ") : r.text;
            if (line.empty())
                continue;
            if (!out.empty())
                out += '\n';
            if (!p.bullet.empty())
                out += p.bullet + " ";
            out += line;
        }
    return out;
}

bool PageDocument::load(std::size_t index)
{
    if (index >= pages.size())
        return false;
    Page& page = pages[index];
    if (page.loaded)
        return true;
    if (!source)
        return false;
    page.loaded = true; // even when it fails: not tried again every frame
    const bool ok = source->load(index, page, media);
    page.scripts = scriptsIn(page.plainText());
    return ok;
}

std::uint8_t scriptsIn(std::string_view utf8)
{
    std::uint8_t scripts = 0;
    const auto* p = reinterpret_cast<const unsigned char*>(utf8.data());
    const auto* end = p + utf8.size();
    while (p < end) {
        const unsigned char b = *p;
        if (b < 0xE1) { // ASCII and two-byte letters (Latin, Greek, Cyrillic, ...): none of these
            p += b < 0x80 ? 1 : 2;
            continue;
        }
        if (b >= 0xF0) { // beyond the BMP: CJK extensions B and on (U+20000-U+3FFFF)
            if (b == 0xF0 && end - p >= 2 && p[1] >= 0xA0 && p[1] < 0xC0)
                scripts |= ScriptHan;
            p += 4;
            continue;
        }
        if (end - p < 3)
            break;
        const unsigned c = (unsigned(b & 0x0F) << 12) | (unsigned(p[1] & 0x3F) << 6) | unsigned(p[2] & 0x3F);
        p += 3;
        if ((c >= 0x1100 && c <= 0x11FF) || (c >= 0x3130 && c <= 0x318F) || (c >= 0xAC00 && c <= 0xD7AF))
            scripts |= ScriptHangul;
        else if ((c >= 0x2E80 && c <= 0x2FDF) || (c >= 0x3000 && c <= 0x312F) || (c >= 0x3190 && c <= 0x31FF) ||
                 (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) ||
                 (c >= 0xFF00 && c <= 0xFFEF))
            scripts |= ScriptHan;
        if (scripts == (ScriptHan | ScriptHangul))
            break;
    }
    return scripts;
}

void PageDocument::unload(std::size_t index)
{
    if (index >= pages.size() || !source || !pages[index].loaded)
        return;
    // Swapped with empty values: `= {}` and clear() keep the capacity (docs/MEMORY.md rule 5).
    Page& page = pages[index];
    std::vector<PageItem>().swap(page.items);
    std::vector<PathItem>().swap(page.paths);
    std::vector<GlyphRun>().swap(page.glyphs);
    std::vector<TextBox>().swap(page.boxes);
    std::vector<ImageItem>().swap(page.images);
    std::vector<Rect>().swap(page.clips);
    page.clip = -1;
    page.loaded = false;
}

namespace {

std::string extensionOf(std::string_view path)
{
    const std::size_t slash = path.find_last_of("/\\");
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash))
        return {};
    std::string ext(path.substr(dot + 1));
    for (char& c : ext)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

} // namespace

std::string viewerFormat(std::string_view path, std::string_view bytes)
{
    if (bytes.substr(0, 1024).find("%PDF-") != std::string_view::npos && (bytes.substr(0, 5) == "%PDF-" || extensionOf(path) == "pdf"))
        return "PDF document";
    if (bytes.substr(0, 4) == std::string_view("PK\x03\x04", 4)) {
        const std::string ext = extensionOf(path);
        if (ext == "docx" || ext == "docm" || ext == "dotx" || ext == "dotm")
            return "Word document";
        if (ext == "xlsx" || ext == "xlsm" || ext == "xltx" || ext == "xltm")
            return "Excel workbook";
        if (ext == "pptx" || ext == "pptm" || ext == "potx" || ext == "potm" || ext == "ppsx" || ext == "ppsm")
            return "PowerPoint presentation";
    }
    return {};
}

namespace {

// The Script bits of a document's text read so far (pages not read yet add theirs when read).
std::uint8_t scriptsOf(ViewerDocument& doc)
{
    std::uint8_t scripts = 0;
    auto spans = [&](const markdown::Spans& ss) {
        for (const markdown::Span& s : ss)
            scripts |= scriptsIn(s.text);
    };
    switch (doc.shape) {
    case Shape::Flow:
        for (const markdown::Block& b : doc.flow.blocks) {
            spans(b.spans);
            for (const auto& row : b.rows)
                for (const markdown::Spans& cell : row)
                    spans(cell);
        }
        break;
    case Shape::Sheet:
        for (const Sheet& s : doc.book.sheets) {
            scripts |= scriptsIn(s.name);
            for (const Row& r : s.rows)
                for (const Cell& c : r.cells)
                    scripts |= scriptsIn(c.text);
            for (const SheetDrawing& d : s.drawings)
                scripts |= scriptsIn(d.page.plainText());
        }
        break;
    case Shape::Pages:
        for (Page& p : doc.pages.pages)
            if (p.loaded) {
                p.scripts = scriptsIn(p.plainText());
                scripts |= p.scripts;
            }
        break;
    case Shape::None:
        break;
    }
    return scripts;
}

} // namespace

ViewerDocument readViewerDocument(std::string_view path, std::string bytes)
{
    const std::string format = viewerFormat(path, bytes);
    ViewerDocument doc;
    if (format == "PDF document")
        doc = readPdf(std::move(bytes));
    else if (format == "Word document")
        doc = readDocx(std::move(bytes));
    else if (format == "Excel workbook")
        doc = readXlsx(std::move(bytes));
    else if (format == "PowerPoint presentation")
        doc = readPptx(std::move(bytes));
    else
        doc.error = "not a document this viewer reads";
    doc.scripts = scriptsOf(doc);
    return doc;
}

std::string plainText(const ViewerDocument& doc)
{
    std::string out;
    auto line = [&](const std::string& s) {
        if (s.empty())
            return;
        if (!out.empty())
            out += '\n';
        out += s;
    };
    switch (doc.shape) {
    case Shape::Flow:
        for (const markdown::Block& b : doc.flow.blocks) {
            if (b.kind == markdown::BlockKind::Table) {
                for (const auto& row : b.rows) {
                    std::string r;
                    for (std::size_t i = 0; i < row.size(); ++i) {
                        if (i)
                            r += '\t';
                        r += markdown::plainText(row[i]);
                    }
                    line(r);
                }
            } else {
                std::string text = markdown::plainText(b.spans);
                if (!b.marker.empty())
                    text = b.marker + " " + text;
                line(text);
            }
        }
        break;
    case Shape::Sheet:
        for (const Sheet& s : doc.book.sheets) {
            line("# " + s.name);
            for (const Row& r : s.rows) {
                std::string text;
                std::uint32_t column = 0;
                for (const Cell& c : r.cells) {
                    for (; column < c.column; ++column)
                        text += '\t';
                    text += c.text;
                }
                line(text);
            }
        }
        break;
    case Shape::Pages:
        for (const Page& p : doc.pages.pages)
            line(p.plainText());
        break;
    case Shape::None:
        break;
    }
    return out;
}

} // namespace enp::model::viewer
