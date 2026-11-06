#include "enp/model/viewer/Viewer.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace enp::model::viewer {

void Page::add(PathItem p)
{
    items.push_back({PageItem::Kind::Path, std::uint32_t(paths.size())});
    paths.push_back(std::move(p));
}

void Page::add(GlyphRun g)
{
    items.push_back({PageItem::Kind::Glyphs, std::uint32_t(glyphs.size())});
    glyphs.push_back(std::move(g));
}

void Page::add(TextBox b)
{
    items.push_back({PageItem::Kind::Box, std::uint32_t(boxes.size())});
    boxes.push_back(std::move(b));
}

void Page::add(ImageItem i)
{
    items.push_back({PageItem::Kind::Image, std::uint32_t(images.size())});
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
    return source->load(index, page, media);
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

ViewerDocument readViewerDocument(std::string_view path, std::string bytes)
{
    const std::string format = viewerFormat(path, bytes);
    if (format == "PDF document")
        return readPdf(std::move(bytes));
    if (format == "Word document")
        return readDocx(std::move(bytes));
    if (format == "Excel workbook")
        return readXlsx(std::move(bytes));
    if (format == "PowerPoint presentation")
        return readPptx(std::move(bytes));
    ViewerDocument doc;
    doc.error = "not a document this viewer reads";
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
