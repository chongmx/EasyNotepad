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

// Text drawn with its style: bold as a second copy a pixel to the right and italic slanted (the UI
// font has neither face), underline and strike as lines.
void styledText(ImDrawList* dl, ImFont* font, float px, ImVec2 at, ImU32 color, std::uint8_t flags, const char* b, const char* e, float width)
{
    const int first = dl->VtxBuffer.Size;
    dl->AddText(font, px, at, color, b, e);
    if (flags & mv::Bold)
        dl->AddText(font, px, ImVec2(at.x + std::max(1.0f, px * 0.045f), at.y), color, b, e);
    if (flags & mv::Italic) { // about 12 degrees, about the baseline
        const float baseline = at.y + px * 0.8f;
        for (int i = first; i < dl->VtxBuffer.Size; ++i)
            dl->VtxBuffer[i].pos.x += (baseline - dl->VtxBuffer[i].pos.y) * 0.21f;
    }
    if (flags & mv::Underline)
        dl->AddLine(ImVec2(at.x, at.y + px * 1.02f), ImVec2(at.x + width, at.y + px * 1.02f), color, std::max(1.0f, px * 0.06f));
    if (flags & mv::Strike)
        dl->AddLine(ImVec2(at.x, at.y + px * 0.55f), ImVec2(at.x + width, at.y + px * 0.55f), color, std::max(1.0f, px * 0.06f));
}

// The vertices drawn since `first` turned by `degrees` clockwise about `c`.
void rotateVertices(ImDrawList* dl, int first, ImVec2 c, float degrees)
{
    if (std::fabs(degrees) < 0.01f)
        return;
    const float a = degrees * 3.14159265f / 180.0f, cs = std::cos(a), sn = std::sin(a);
    for (int i = first; i < dl->VtxBuffer.Size; ++i) {
        ImVec2& v = dl->VtxBuffer.Data[i].pos;
        const float dx = v.x - c.x, dy = v.y - c.y;
        v = ImVec2(c.x + dx * cs - dy * sn, c.y + dx * sn + dy * cs);
    }
}

// The vertices drawn since `first` stretched along x by `k` from `x0`.
void stretchVertices(ImDrawList* dl, int first, float x0, float k)
{
    if (std::fabs(k - 1.0f) < 0.005f)
        return;
    for (int i = first; i < dl->VtxBuffer.Size; ++i) {
        ImVec2& v = dl->VtxBuffer.Data[i].pos;
        v.x = x0 + (v.x - x0) * k;
    }
}

// The vertices drawn since `first` recoloured by `g` over the bounds [lo, hi]: each keeps its
// share of alpha (anti-aliased fringes stay transparent).
void shadeVertices(ImDrawList* dl, int first, const mv::Gradient& g, ImVec2 lo, ImVec2 hi, ImVec2 origin = ImVec2(0, 0), float scale = 1.0f)
{
    if (g.stops.empty() || first >= dl->VtxBuffer.Size)
        return;
    if (g.absolute) { // the axis in page points
        const ImVec2 p0(origin.x + g.from.x * scale, origin.y + g.from.y * scale), p1(origin.x + g.to.x * scale, origin.y + g.to.y * scale);
        const float ax = p1.x - p0.x, ay = p1.y - p0.y, len2 = std::max(1e-6f, ax * ax + ay * ay);
        const float r0 = g.r0 * scale, r1 = g.r1 * scale;
        for (int i = first; i < dl->VtxBuffer.Size; ++i) {
            ImDrawVert& v = dl->VtxBuffer.Data[i];
            float t;
            if (g.radial) { // the circle the point lies on, from the first to the second (concentric enough)
                const float d = std::sqrt((v.pos.x - p1.x) * (v.pos.x - p1.x) + (v.pos.y - p1.y) * (v.pos.y - p1.y));
                t = std::fabs(r1 - r0) > 0.01f ? (d - r0) / (r1 - r0) : 0.0f;
            } else {
                t = ((v.pos.x - p0.x) * ax + (v.pos.y - p0.y) * ay) / len2;
            }
            const std::uint32_t color = g.at(std::clamp(t, 0.0f, 1.0f));
            const float share = float((v.col >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
            v.col = (color & 0x00FFFFFFu) | (ImU32(float((color >> 24) & 0xFF) * share + 0.5f) << IM_COL32_A_SHIFT);
        }
        return;
    }
    const ImVec2 c((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f);
    const float a = g.angle * 3.14159265f / 180.0f, dx = std::cos(a), dy = std::sin(a);
    // Linear: the projection on the direction, over the bounds' extent along it.
    const float half = std::fabs((hi.x - lo.x) * 0.5f * dx) + std::fabs((hi.y - lo.y) * 0.5f * dy);
    // Radial: from its centre out to the farthest corner of the bounds.
    const ImVec2 rc(lo.x + (hi.x - lo.x) * g.cx, lo.y + (hi.y - lo.y) * g.cy);
    const float far = std::max(std::fabs(rc.x - lo.x), std::fabs(hi.x - rc.x)), farY = std::max(std::fabs(rc.y - lo.y), std::fabs(hi.y - rc.y));
    const float radius = std::max(1.0f, std::sqrt(far * far + farY * farY));
    for (int i = first; i < dl->VtxBuffer.Size; ++i) {
        ImDrawVert& v = dl->VtxBuffer.Data[i];
        float t;
        if (g.radial)
            t = std::sqrt((v.pos.x - rc.x) * (v.pos.x - rc.x) + (v.pos.y - rc.y) * (v.pos.y - rc.y)) / radius;
        else
            t = half > 0.0f ? 0.5f + ((v.pos.x - c.x) * dx + (v.pos.y - c.y) * dy) / (2.0f * half) : 0.0f;
        const std::uint32_t color = g.at(std::clamp(t, 0.0f, 1.0f));
        const float share = float((v.col >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
        const float alpha = float((color >> 24) & 0xFF) * share;
        v.col = (color & 0x00FFFFFFu) | (ImU32(alpha + 0.5f) << IM_COL32_A_SHIFT);
    }
}

// Whether a gradient varies inside a triangle in a way its corners' colours cannot give: radial
// ones, and linear ones of more than two stops.
bool needsFineMesh(const mv::Gradient& g)
{
    return g.radial || g.stops.size() > 2;
}

// Splits the triangles drawn since (firstVtx, firstIdx) into smaller ones, each edge in pieces of
// about `step` pixels (at most 24), so colours set per vertex afterwards vary inside them. Left as
// drawn when they span two draw commands. `tris` is the caller's scratch.
void subdivide(ImDrawList* dl, int firstVtx, int firstIdx, float step, std::vector<ImDrawVert>& tris)
{
    if (dl->CmdBuffer.empty())
        return;
    ImDrawCmd& cmd = dl->CmdBuffer.back();
    const int idxCount = dl->IdxBuffer.Size - firstIdx;
    if (idxCount <= 0 || idxCount % 3 != 0 || int(cmd.ElemCount) < idxCount || firstVtx < int(cmd.VtxOffset))
        return;
    tris.clear();
    for (int i = firstIdx; i < dl->IdxBuffer.Size; ++i)
        tris.push_back(dl->VtxBuffer[int(cmd.VtxOffset) + int(dl->IdxBuffer[i])]);
    // Take the triangles back out of the list, then draw their pieces in their place.
    dl->VtxBuffer.resize(firstVtx);
    dl->IdxBuffer.resize(firstIdx);
    dl->_VtxWritePtr = dl->VtxBuffer.Data + firstVtx;
    dl->_IdxWritePtr = dl->IdxBuffer.Data + firstIdx;
    dl->_VtxCurrentIdx = unsigned(firstVtx - int(cmd.VtxOffset));
    cmd.ElemCount -= unsigned(idxCount);
    auto mix = [](const ImDrawVert& a, const ImDrawVert& b, const ImDrawVert& c, float u, float v) {
        const float w = 1.0f - u - v;
        ImDrawVert out;
        out.pos = ImVec2(a.pos.x * w + b.pos.x * u + c.pos.x * v, a.pos.y * w + b.pos.y * u + c.pos.y * v);
        out.uv = ImVec2(a.uv.x * w + b.uv.x * u + c.uv.x * v, a.uv.y * w + b.uv.y * u + c.uv.y * v);
        ImU32 col = 0;
        for (int shift = 0; shift < 32; shift += 8) {
            const float ch = float((a.col >> shift) & 0xFF) * w + float((b.col >> shift) & 0xFF) * u + float((c.col >> shift) & 0xFF) * v;
            col |= ImU32(std::clamp(ch + 0.5f, 0.0f, 255.0f)) << shift;
        }
        out.col = col;
        return out;
    };
    auto length = [](ImVec2 p, ImVec2 q) { return std::sqrt((p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y)); };
    for (std::size_t t = 0; t + 2 < tris.size(); t += 3) {
        const ImDrawVert a = tris[t], b = tris[t + 1], c = tris[t + 2];
        const float edge = std::max({length(a.pos, b.pos), length(b.pos, c.pos), length(c.pos, a.pos)});
        const int k = std::clamp(int(std::ceil(edge / std::max(1.0f, step))), 1, 24);
        const int vertices = (k + 1) * (k + 2) / 2;
        dl->PrimReserve(k * k * 3, vertices);
        const unsigned base = dl->_VtxCurrentIdx; // after the reserve: it may have started a new command
        for (int j = 0; j <= k; ++j)
            for (int i = 0; i + j <= k; ++i) {
                const ImDrawVert v = mix(a, b, c, float(i) / float(k), float(j) / float(k));
                dl->PrimWriteVtx(v.pos, v.uv, v.col);
            }
        auto at = [&](int i, int j) { return ImDrawIdx(base + unsigned(j * (k + 1) - j * (j - 1) / 2 + i)); };
        for (int j = 0; j < k; ++j)
            for (int i = 0; i + j < k; ++i) {
                dl->PrimWriteIdx(at(i, j));
                dl->PrimWriteIdx(at(i + 1, j));
                dl->PrimWriteIdx(at(i, j + 1));
                if (i + j < k - 1) {
                    dl->PrimWriteIdx(at(i + 1, j));
                    dl->PrimWriteIdx(at(i + 1, j + 1));
                    dl->PrimWriteIdx(at(i, j + 1));
                }
            }
    }
}

// A line's end at `tip`, coming from `from`: a filled or open arrow, a diamond or a dot, `size`
// pixels long.
void lineEnd(ImDrawList* dl, mv::LineEnd kind, ImVec2 tip, ImVec2 from, float size, ImU32 color, float width)
{
    if (kind == mv::LineEnd::None)
        return;
    float dx = tip.x - from.x, dy = tip.y - from.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 0.01f)
        return;
    dx /= len;
    dy /= len;
    const ImVec2 back(tip.x - dx * size, tip.y - dy * size), side(-dy * size * 0.5f, dx * size * 0.5f);
    switch (kind) {
    case mv::LineEnd::Triangle:
        dl->AddTriangleFilled(tip, ImVec2(back.x + side.x, back.y + side.y), ImVec2(back.x - side.x, back.y - side.y), color);
        break;
    case mv::LineEnd::Stealth: {
        const ImVec2 notch(tip.x - dx * size * 0.6f, tip.y - dy * size * 0.6f);
        const ImVec2 pts[4] = {tip, ImVec2(back.x + side.x, back.y + side.y), notch, ImVec2(back.x - side.x, back.y - side.y)};
        dl->AddConcavePolyFilled(pts, 4, color);
        break;
    }
    case mv::LineEnd::Arrow: {
        const ImVec2 pts[3] = {ImVec2(back.x + side.x, back.y + side.y), tip, ImVec2(back.x - side.x, back.y - side.y)};
        dl->AddPolyline(pts, 3, color, width, ImDrawFlags_None);
        break;
    }
    case mv::LineEnd::Diamond: {
        const ImVec2 mid(tip.x - dx * size * 0.5f, tip.y - dy * size * 0.5f);
        const ImVec2 pts[4] = {tip, ImVec2(mid.x + side.x, mid.y + side.y), back, ImVec2(mid.x - side.x, mid.y - side.y)};
        dl->AddConvexPolyFilled(pts, 4, color);
        break;
    }
    case mv::LineEnd::Oval:
        dl->AddCircleFilled(ImVec2(tip.x - dx * size * 0.25f, tip.y - dy * size * 0.25f), size * 0.4f, color);
        break;
    case mv::LineEnd::None:
        break;
    }
}

} // namespace

void PagePainter::release()
{
    os::mem::release(points_);
    os::mem::release(tris_);
    peak_ = 0;
}

void PagePainter::draw(const mv::Page& page, ImVec2 origin, float scale, const Fonts& fonts, PageImages* images)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 end(origin.x + page.width * scale, origin.y + page.height * scale);
    dl->PushClipRect(origin, end, true);
    {
        const int first = dl->VtxBuffer.Size, firstIdx = dl->IdxBuffer.Size;
        dl->AddRectFilled(origin, end, page.backgroundGradient.stops.empty() ? page.background : page.backgroundGradient.stops.front().color);
        if (needsFineMesh(page.backgroundGradient))
            subdivide(dl, first, firstIdx, 24.0f, tris_);
        shadeVertices(dl, first, page.backgroundGradient, origin, end);
    }
    if (tris_.capacity() > 65536)
        os::mem::release(tris_);
    peak_ = 0;
    for (const mv::PageItem& item : page.items) {
        const bool clipped = item.clip >= 0 && std::size_t(item.clip) < page.clips.size();
        if (clipped) {
            const mv::Rect& c = page.clips[std::size_t(item.clip)];
            dl->PushClipRect(ImVec2(origin.x + c.x * scale, origin.y + c.y * scale), ImVec2(origin.x + (c.x + c.w) * scale, origin.y + (c.y + c.h) * scale),
                             true);
        }
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
            float u0 = img.crop.x, v0 = img.crop.y, u1 = img.crop.x + img.crop.w, v1 = img.crop.y + img.crop.h;
            if (img.flipH)
                std::swap(u0, u1);
            if (img.flipV)
                std::swap(v0, v1);
            const int first = dl->VtxBuffer.Size;
            if (image && image->texture)
                dl->AddImage(image->texture->GetTexRef(), a, b, ImVec2(u0, v0), ImVec2(u1, v1));
            else
                dl->AddRectFilled(a, b, IM_COL32(128, 128, 128, 40));
            rotateVertices(dl, first, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), img.rotation);
            break;
        }
        }
        if (clipped)
            dl->PopClipRect();
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
        const int firstFill = dl->VtxBuffer.Size, firstFillIdx = dl->IdxBuffer.Size;
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
            if (!p.gradient.stops.empty()) {
                ImVec2 lo = points_[0], hi = points_[0];
                for (const ImVec2& q : points_) {
                    lo = ImVec2(std::min(lo.x, q.x), std::min(lo.y, q.y));
                    hi = ImVec2(std::max(hi.x, q.x), std::max(hi.y, q.y));
                }
                if (needsFineMesh(p.gradient))
                    subdivide(dl, firstFill, firstFillIdx, 16.0f, tris_);
                shadeVertices(dl, firstFill, p.gradient, lo, hi, origin, scale);
            }
        }
        if (p.stroke) {
            const bool closed = s < p.closed.size() && p.closed[s];
            const float width = std::max(1.0f, p.lineWidth * scale);
            if (p.dash.empty()) {
                dl->AddPolyline(points_.data(), n, p.stroke, width, closed ? ImDrawFlags_Closed : ImDrawFlags_None);
            } else {
                // Dashes: along the outline, the pattern's lengths in turn (on, off, on, ...).
                std::size_t phase = 0;
                float left = std::max(0.5f, p.dash[0] * scale);
                const int segments = closed ? n : n - 1;
                for (int i = 0; i < segments; ++i) {
                    ImVec2 a = points_[size_t(i)];
                    const ImVec2 b = points_[size_t((i + 1) % n)];
                    float len = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
                    if (len < 0.01f)
                        continue;
                    const ImVec2 d((b.x - a.x) / len, (b.y - a.y) / len);
                    while (len > 0.0f) {
                        const float step = std::min(left, len);
                        const ImVec2 to(a.x + d.x * step, a.y + d.y * step);
                        if (phase % 2 == 0)
                            dl->AddLine(a, to, p.stroke, width);
                        a = to;
                        len -= step;
                        left -= step;
                        if (left <= 0.0f) {
                            phase = (phase + 1) % (p.dash.size() % 2 == 0 ? p.dash.size() : p.dash.size() * 2);
                            left = std::max(0.5f, p.dash[phase % p.dash.size()] * scale);
                        }
                    }
                }
            }
            if (!closed && n >= 2) {
                const float size = std::max(6.0f, width * 3.0f);
                lineEnd(dl, p.head, points_[0], points_[1], size, p.stroke, width);
                lineEnd(dl, p.tail, points_[size_t(n - 1)], points_[size_t(n - 2)], size, p.stroke, width);
            }
        }
    }
}

// A PDF's text is drawn in our font at its own positions: word by word, each stretched to the width
// the PDF gives it (its font's metrics, character spacing, horizontal scaling), so words keep their
// natural letter spacing and land where the PDF put them. A turned run is turned about its start.
void PagePainter::drawGlyphs(ImDrawList* dl, const mv::GlyphRun& g, ImVec2 origin, float scale, const Fonts& fonts)
{
    if ((g.color >> 24) == 0) // invisible text (over a scanned page): kept for copying, not drawn
        return;
    const float px = g.size * scale;
    if (px < 1.0f)
        return;
    ImFont* font = fontFor(fonts, g.flags);
    const float ascent = font->GetFontBaked(px)->Ascent;
    const ImVec2 runAt(origin.x + g.x * scale, origin.y + g.baseline * scale);
    const int firstVertex = dl->VtxBuffer.Size;
    const char* text = g.text.data();
    const char* end = text + g.text.size();
    std::size_t k = 0; // code point index
    auto offset = [&](std::size_t i) { return i < g.offsets.size() ? g.offsets[i] : g.width; };
    for (const char* c = text; c < end;) {
        if (*c == ' ') {
            ++c;
            ++k;
            continue;
        }
        // One word: up to the next space.
        const char* w = c;
        const std::size_t k0 = k;
        while (c < end && *c != ' ') {
            c = std::min(end, c + utf8Length(static_cast<unsigned char>(*c)));
            ++k;
        }
        const float x0 = runAt.x + offset(k0) * scale;
        const float target = (offset(k) - offset(k0)) * scale;
        const int wordVertex = dl->VtxBuffer.Size;
        styledText(dl, font, px, ImVec2(x0, runAt.y - ascent), g.color, g.flags & ~std::uint8_t(mv::Underline | mv::Strike), w, c, 0.0f);
        const float natural = measure(font, px, w, c);
        if (natural > 0.5f && target > 0.5f)
            stretchVertices(dl, wordVertex, x0, std::clamp(target / natural, 0.5f, 2.0f));
    }
    rotateVertices(dl, firstVertex, runAt, g.angle);
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
    const int firstVertex = dl->VtxBuffer.Size;
    layout(y, true);
    rotateVertices(dl, firstVertex, ImVec2(origin.x + (box.box.x + box.box.w * 0.5f) * scale, origin.y + (box.box.y + box.box.h * 0.5f) * scale),
                   box.rotation);
}

} // namespace enp::view
