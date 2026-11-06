#include "enp/view/viewer/PagePainter.h"

#include "enp/os/Memory.h"
#include "enp/view/MarkdownImages.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace enp::view {
namespace {

namespace mv = model::viewer;

std::size_t utf8Length(unsigned char c)
{
    return c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
}

ImFont* fontFor(const Fonts& fonts, std::uint8_t flags)
{
    return (flags & mv::Mono) && fonts.mono ? fonts.mono : fonts.ui;
}

float measure(ImFont* font, float px, const char* b, const char* e)
{
    return b >= e ? 0.0f : font->CalcTextSizeA(px, FLT_MAX, 0.0f, b, e).x;
}

// Text drawn with its style: bold as a second copy a pixel to the right (the UI font has no bold
// face), underline and strike as lines.
void styledText(ImDrawList* dl, ImFont* font, float px, ImVec2 at, ImU32 color, std::uint8_t flags, const char* b, const char* e, float width)
{
    dl->AddText(font, px, at, color, b, e);
    if (flags & mv::Bold)
        dl->AddText(font, px, ImVec2(at.x + std::max(1.0f, px * 0.045f), at.y), color, b, e);
    if (flags & mv::Underline)
        dl->AddLine(ImVec2(at.x, at.y + px * 1.02f), ImVec2(at.x + width, at.y + px * 1.02f), color, std::max(1.0f, px * 0.06f));
    if (flags & mv::Strike)
        dl->AddLine(ImVec2(at.x, at.y + px * 0.55f), ImVec2(at.x + width, at.y + px * 0.55f), color, std::max(1.0f, px * 0.06f));
}

} // namespace

void PagePainter::release()
{
    os::mem::release(points_);
    peak_ = 0;
}

void PagePainter::draw(const mv::Page& page, ImVec2 origin, float scale, const Fonts& fonts, PageImages* images)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 end(origin.x + page.width * scale, origin.y + page.height * scale);
    dl->AddRectFilled(origin, end, page.background);
    dl->PushClipRect(origin, end, true);
    peak_ = 0;
    for (const mv::PageItem& item : page.items) {
        switch (item.kind) {
        case mv::PageItem::Kind::Path:
            drawPath(dl, page.paths[item.index], origin, scale);
            break;
        case mv::PageItem::Kind::Glyphs:
            drawGlyphs(dl, page.glyphs[item.index], origin, scale, fonts);
            break;
        case mv::PageItem::Kind::Box:
            drawBox(dl, page.boxes[item.index], origin, scale, fonts);
            break;
        case mv::PageItem::Kind::Image: {
            const mv::ImageItem& img = page.images[item.index];
            const ImVec2 a(origin.x + img.box.x * scale, origin.y + img.box.y * scale);
            const ImVec2 b(a.x + img.box.w * scale, a.y + img.box.h * scale);
            const MarkdownImage* image = images ? images->image(img.media) : nullptr;
            if (image && image->texture)
                dl->AddImage(image->texture->GetTexRef(), a, b);
            else
                dl->AddRectFilled(a, b, IM_COL32(128, 128, 128, 40));
            break;
        }
        }
    }
    dl->PopClipRect();
    // The point buffer keeps a big path's room only while such paths are drawn.
    if (points_.capacity() > 4 * peak_ + 1024)
        os::mem::release(points_);
}

void PagePainter::drawPath(ImDrawList* dl, const mv::PathItem& p, ImVec2 origin, float scale)
{
    for (std::size_t s = 0; s < p.subpaths.size(); ++s) {
        const std::vector<mv::Point>& sub = p.subpaths[s];
        if (sub.size() < 2)
            continue;
        points_.resize(sub.size());
        peak_ = std::max(peak_, sub.size());
        for (std::size_t i = 0; i < sub.size(); ++i)
            points_[i] = ImVec2(origin.x + sub[i].x * scale, origin.y + sub[i].y * scale);
        const int n = int(points_.size());
        if (p.fill && n >= 3) {
            const bool rect = n == 4 && ((points_[0].x == points_[1].x && points_[1].y == points_[2].y && points_[2].x == points_[3].x) ||
                                         (points_[0].y == points_[1].y && points_[1].x == points_[2].x && points_[2].y == points_[3].y));
            if (rect) {
                const ImVec2 a(std::min({points_[0].x, points_[1].x, points_[2].x}), std::min({points_[0].y, points_[1].y, points_[2].y}));
                const ImVec2 b(std::max({points_[0].x, points_[1].x, points_[2].x}), std::max({points_[0].y, points_[1].y, points_[2].y}));
                dl->AddRectFilled(a, b, p.fill);
            } else {
                dl->AddConcavePolyFilled(points_.data(), n, p.fill);
            }
        }
        if (p.stroke) {
            const bool closed = s < p.closed.size() && p.closed[s];
            dl->AddPolyline(points_.data(), n, p.stroke, std::max(1.0f, p.lineWidth * scale), closed ? ImDrawFlags_Closed : ImDrawFlags_None);
        }
    }
}

void PagePainter::drawGlyphs(ImDrawList* dl, const mv::GlyphRun& g, ImVec2 origin, float scale, const Fonts& fonts)
{
    if ((g.color >> 24) == 0) // invisible text (over a scanned page): kept for copying, not drawn
        return;
    const float px = g.size * scale;
    if (px < 1.0f)
        return;
    ImFont* font = fontFor(fonts, g.flags);
    const float ascent = font->GetFontBaked(px)->Ascent;
    const float top = origin.y + g.baseline * scale - ascent;
    const char* text = g.text.data();
    const char* end = text + g.text.size();
    std::size_t k = 0;
    for (const char* c = text; c < end; ++k) {
        const char* next = std::min(end, c + utf8Length(static_cast<unsigned char>(*c)));
        if (*c != ' ') {
            const float x = origin.x + (g.x + (k < g.offsets.size() ? g.offsets[k] : 0.0f)) * scale;
            styledText(dl, font, px, ImVec2(x, top), g.color, g.flags & ~std::uint8_t(mv::Underline | mv::Strike), c, next, 0.0f);
        }
        c = next;
    }
}

// Wraps and draws a slide's text box: paragraphs of runs, line by line, without copying the text.
// Laid out twice when it is not anchored at the top: once to measure, once to draw.
void PagePainter::drawBox(ImDrawList* dl, const mv::TextBox& box, ImVec2 origin, float scale, const Fonts& fonts)
{
    const float left = origin.x + (box.box.x + box.insetL) * scale;
    const float right = origin.x + (box.box.x + box.box.w - box.insetR) * scale;
    const float top = origin.y + (box.box.y + box.insetT) * scale;
    const float bottom = origin.y + (box.box.y + box.box.h - box.insetB) * scale;
    struct Pos {
        std::size_t run = 0, at = 0;
    };
    auto layout = [&](float y0, bool draw) {
        float y = y0;
        for (std::size_t pi = 0; pi < box.paragraphs.size(); ++pi) {
            const mv::TextParagraph& para = box.paragraphs[pi];
            if (pi > 0)
                y += para.spaceBefore * scale;
            const float indent = float(para.level) * 27.0f * scale + (para.bullet.empty() ? 0.0f : 20.0f * scale);
            const float lineLeft = left + indent;
            const float avail = std::max(8.0f, right - lineLeft);
            const auto& runs = para.runs;
            Pos cur;
            bool firstLine = true;
            while (cur.run < runs.size()) {
                // Gather one line: whole words until the width is used.
                Pos p = cur, breakAt = cur;
                float width = 0.0f, maxSize = 0.0f;
                bool any = false, forced = false;
                while (p.run < runs.size()) {
                    const mv::TextRun& r = runs[p.run];
                    if (p.at >= r.text.size()) {
                        if (r.text.empty())
                            maxSize = std::max(maxSize, r.size);
                        p = Pos{p.run + 1, 0};
                        breakAt = p;
                        continue;
                    }
                    ImFont* font = fontFor(fonts, r.flags);
                    const float px = r.size * scale;
                    maxSize = std::max(maxSize, r.size);
                    const char* s = r.text.data();
                    if (s[p.at] == '\n') {
                        p.at += 1;
                        breakAt = p;
                        forced = true;
                        break;
                    }
                    std::size_t e = p.at;
                    while (e < r.text.size() && s[e] != ' ' && s[e] != '\n')
                        ++e;
                    while (e < r.text.size() && s[e] == ' ')
                        ++e;
                    const float w = measure(font, px, s + p.at, s + e);
                    if (any && box.wrap && width + w > avail + 0.5f && width > 0.0f) {
                        // The trailing spaces of the word may stay on this line.
                        break;
                    }
                    width += w;
                    any = true;
                    p.at = e;
                    breakAt = p;
                }
                if (!any && !forced && p.run >= runs.size() && maxSize == 0.0f)
                    break;
                const float lineH = std::max(1.0f, maxSize) * 1.2f * para.lineSpacing * scale;
                if (draw) {
                    float x = lineLeft;
                    if (para.align == 1)
                        x += std::max(0.0f, (avail - width) * 0.5f);
                    else if (para.align == 2)
                        x += std::max(0.0f, avail - width);
                    if (firstLine && !para.bullet.empty() && !runs.empty()) {
                        const mv::TextRun& r0 = runs[0];
                        ImFont* font = fontFor(fonts, r0.flags);
                        const float px = r0.size * scale;
                        const float bw = measure(font, px, para.bullet.data(), para.bullet.data() + para.bullet.size());
                        dl->AddText(font, px, ImVec2(lineLeft - bw - 6.0f * scale, y + (lineH - px) * 0.5f), r0.color, para.bullet.data(),
                                    para.bullet.data() + para.bullet.size());
                    }
                    // The line's pieces, run by run.
                    Pos q = cur;
                    while (q.run < runs.size() && (q.run < breakAt.run || (q.run == breakAt.run && q.at < breakAt.at))) {
                        const mv::TextRun& r = runs[q.run];
                        const std::size_t stop = q.run == breakAt.run ? breakAt.at : r.text.size();
                        if (q.at < stop) {
                            std::size_t e = stop;
                            if (e > q.at && r.text[e - 1] == '\n')
                                --e;
                            ImFont* font = fontFor(fonts, r.flags);
                            const float px = r.size * scale;
                            const char* b = r.text.data() + q.at;
                            const char* en = r.text.data() + e;
                            const float w = measure(font, px, b, en);
                            if (px >= 1.0f && en > b)
                                styledText(dl, font, px, ImVec2(x, y + (lineH - px * 1.2f) * 0.5f + px * 0.1f), r.color, r.flags, b, en, w);
                            x += w;
                        }
                        q = Pos{q.run + 1, 0};
                    }
                }
                y += lineH;
                firstLine = false;
                cur = breakAt;
                if (!any && !forced) // nothing fitted and nothing moved: done with this paragraph
                    break;
            }
        }
        return y - y0;
    };
    float y = top;
    if (box.anchor != 0) {
        const float h = layout(top, false);
        const float room = bottom - top;
        y = box.anchor == 1 ? top + (room - h) * 0.5f : bottom - h;
    }
    layout(y, true);
}

} // namespace enp::view
