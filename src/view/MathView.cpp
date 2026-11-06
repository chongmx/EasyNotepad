#include "enp/view/MathView.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace enp::view {

namespace {

using model::math::Node;

struct Ctx {
    ImFont* font;
    float base;    // body size
    float s;       // UI scale
    ImU32 color;
    bool display;
    int level;     // 0 text, 1 script, 2 scriptscript
    float size() const { return base * (level == 0 ? 1.0f : level == 1 ? 0.72f : 0.55f); }
    Ctx script() const
    {
        Ctx c = *this;
        c.level = std::min(2, level + 1);
        c.display = false;
        return c;
    }
};

float ascentOf(ImFont* font, float size)
{
    ImFontBaked* baked = font->GetFontBaked(size);
    return baked ? baked->Ascent : size * 0.8f;
}
float descentOf(ImFont* font, float size)
{
    ImFontBaked* baked = font->GetFontBaked(size);
    return baked ? -baked->Descent : size * 0.2f;
}

MathBox textBox(const Ctx& c, const std::string& text, float size)
{
    MathBox b;
    b.width = c.font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x;
    b.ascent = ascentOf(c.font, size);
    b.descent = descentOf(c.font, size);
    return b;
}

bool empty(const Node& n) { return n.kind == Node::Kind::Row && n.children.empty(); }

MathBox measure(const Node& n, const Ctx& c);
void draw(ImDrawList* dl, const Node& n, ImVec2 at, const Ctx& c); // at: left end of the baseline

// Layout parameters, as fractions of the current size.
constexpr float kAxis = 0.25f;       // the math axis (fraction rule height) above the baseline
constexpr float kFracGap = 0.12f;
constexpr float kSupShift = 0.42f;
constexpr float kSubShift = 0.18f;
constexpr float kBinaryPad = 0.18f;
constexpr float kOpScale = 1.35f;

MathBox measureText(const Node& n, const Ctx& c)
{
    const float size = c.size();
    MathBox b = textBox(c, n.text, size);
    if (n.binary)
        b.width += 2.0f * kBinaryPad * size;
    if (n.bold)
        b.width += std::max(0.5f, size / 24.0f);
    return b;
}

MathBox measureRow(const Node& n, const Ctx& c)
{
    MathBox b;
    for (const Node& child : n.children) {
        const MathBox cb = measure(child, c);
        b.width += cb.width;
        b.ascent = std::max(b.ascent, cb.ascent);
        b.descent = std::max(b.descent, cb.descent);
    }
    if (n.children.empty()) {
        b.ascent = ascentOf(c.font, c.size()) * 0.6f;
        b.descent = 0.0f;
    }
    return b;
}

MathBox measureFrac(const Node& n, const Ctx& c)
{
    const Ctx inner = c.display ? c : c.script();
    const MathBox num = measure(n.children[0], inner), den = measure(n.children[1], inner);
    const float size = c.size();
    const float gap = kFracGap * size, axis = kAxis * size, pad = 0.12f * size;
    MathBox b;
    b.width = std::max(num.width, den.width) + pad * 2.0f;
    b.ascent = axis + gap + num.descent + num.ascent;
    b.descent = std::max(0.0f, den.ascent + den.descent + gap - axis);
    return b;
}

MathBox measureScripts(const Node& n, const Ctx& c)
{
    const Node& baseNode = n.children[0];
    const bool hasSup = !empty(n.children[1]), hasSub = !empty(n.children[2]);
    const Ctx sc = c.script();
    const MathBox base = measure(baseNode, c);
    const MathBox sup = hasSup ? measure(n.children[1], sc) : MathBox{};
    const MathBox sub = hasSub ? measure(n.children[2], sc) : MathBox{};
    const float size = c.size();
    MathBox b = base;
    if (baseNode.kind == Node::Kind::Op && baseNode.limitsAbove && c.display) { // limits above and below
        const float gap = 0.1f * size;
        b.width = std::max({base.width, sup.width, sub.width});
        if (hasSup)
            b.ascent = base.ascent + gap + sup.descent + sup.ascent;
        if (hasSub)
            b.descent = base.descent + gap + sub.ascent + sub.descent;
        return b;
    }
    const float supShift = std::max(kSupShift * size, base.ascent - 0.55f * sc.size());
    const float subShift = std::max(kSubShift * size, base.descent + 0.1f * size);
    b.width = base.width + std::max(sup.width, sub.width) + (hasSup || hasSub ? 0.04f * size : 0.0f);
    if (hasSup)
        b.ascent = std::max(b.ascent, supShift + sup.ascent);
    if (hasSub)
        b.descent = std::max(b.descent, subShift + sub.descent);
    return b;
}

MathBox measureSqrt(const Node& n, const Ctx& c)
{
    const MathBox rad = measure(n.children[0], c);
    const float size = c.size();
    const float gap = 0.1f * size, rule = std::max(1.0f, size / 14.0f);
    MathBox b;
    b.width = 0.75f * size + rad.width + 0.15f * size;
    b.ascent = rad.ascent + gap + rule + gap;
    b.descent = rad.descent;
    if (!empty(n.children[1])) {
        const MathBox index = measure(n.children[1], c.script().script());
        b.width += std::max(0.0f, index.width - 0.35f * size);
        b.ascent = std::max(b.ascent, 0.55f * size + index.height());
    }
    return b;
}

MathBox measureOp(const Node& n, const Ctx& c)
{
    const bool glyph = n.text.size() <= 4 && static_cast<unsigned char>(n.text[0]) >= 0x80; // ∑ ∫: a symbol, enlarged
    const float size = c.size() * (glyph && c.display ? kOpScale : 1.0f);
    MathBox b = textBox(c, n.text, size);
    b.width += 0.1f * c.size();
    return b;
}

MathBox measureAccent(const Node& n, const Ctx& c)
{
    const MathBox base = measure(n.children[0], c);
    MathBox b = base;
    const float size = c.size();
    b.ascent = std::max(base.ascent, 0.72f * size) + 0.22f * size;
    return b;
}

// Half the height of the delimiters around `content`: centred on the axis, to its farther edge.
float fenceHalf(const MathBox& content, const Ctx& c)
{
    const float axis = kAxis * c.size();
    return std::max({content.ascent - axis, content.descent + axis, 0.5f * c.size()});
}

MathBox measureFenced(const Node& n, const Ctx& c)
{
    const MathBox content = measure(n.children[0], c);
    MathBox b = content;
    const float size = c.size();
    if (!n.text.empty())
        b.width += 0.45f * size + 0.05f * size;
    if (!n.text2.empty())
        b.width += 0.45f * size + 0.05f * size;
    const float half = fenceHalf(content, c), axis = kAxis * size;
    b.ascent = std::max(b.ascent, axis + half);
    b.descent = std::max(b.descent, half - axis);
    return b;
}

// Tables: column widths kept for this many columns (a wider table puts the rest in the last).
constexpr int kMaxColumns = 64;
constexpr float kRowGap = 0.3f;    // between rows
constexpr float kColumnGap = 1.0f; // between columns (between pairs in align, and in cases)
constexpr float kMatrixGap = 0.8f; // between columns of a matrix or an array

Ctx cellContext(const Node& table, const Ctx& c)
{
    Ctx k = c;
    k.display = table.display && c.display;
    return k;
}

int columnOf(size_t j) { return int(std::min<size_t>(j, kMaxColumns - 1)); }

char alignOf(const Node& table, size_t j)
{
    if (table.pairs)
        return j % 2 == 0 ? 'r' : 'l';
    if (table.text.empty())
        return 'c';
    return table.text[std::min(j, table.text.size() - 1)];
}

float gapAfter(const Node& table, size_t j, float size)
{
    if (table.pairs)
        return j % 2 == 1 ? kColumnGap * size : 0.0f;
    return (table.text == "l" ? kColumnGap : kMatrixGap) * size;
}

// A row's box, at least a line of text high so rows are evenly spaced.
MathBox rowBox(const Node& cells, const Ctx& k)
{
    MathBox b;
    b.ascent = 0.8f * k.size();
    b.descent = 0.3f * k.size();
    for (const Node& cell : cells.children) {
        const MathBox cb = measure(cell, k);
        b.ascent = std::max(b.ascent, cb.ascent);
        b.descent = std::max(b.descent, cb.descent);
    }
    return b;
}

// Fills `widths`; returns the number of columns.
int columnWidths(const Node& table, const Ctx& k, float (&widths)[kMaxColumns])
{
    int columns = 0;
    for (const Node& cells : table.children)
        for (size_t j = 0; j < cells.children.size(); ++j) {
            const int col = columnOf(j);
            widths[col] = std::max(widths[col], measure(cells.children[j], k).width);
            columns = std::max(columns, col + 1);
        }
    return columns;
}

// Centred on the math axis, as LaTeX centres a matrix or an aligned block.
MathBox measureTable(const Node& n, const Ctx& c)
{
    const Ctx k = cellContext(n, c);
    const float size = c.size();
    float widths[kMaxColumns] = {};
    const int columns = columnWidths(n, k, widths);
    MathBox b;
    for (int j = 0; j < columns; ++j)
        b.width += widths[j] + (j + 1 < columns ? gapAfter(n, size_t(j), size) : 0.0f);
    float height = 0.0f;
    for (size_t r = 0; r < n.children.size(); ++r)
        height += rowBox(n.children[r], k).height() + (r > 0 ? kRowGap * size : 0.0f);
    b.ascent = height * 0.5f + kAxis * size;
    b.descent = height * 0.5f - kAxis * size;
    return b;
}

MathBox measure(const Node& n, const Ctx& c)
{
    switch (n.kind) {
    case Node::Kind::Row: return measureRow(n, c);
    case Node::Kind::Text: return measureText(n, c);
    case Node::Kind::Frac: return measureFrac(n, c);
    case Node::Kind::Scripts: return measureScripts(n, c);
    case Node::Kind::Sqrt: return measureSqrt(n, c);
    case Node::Kind::Op: return measureOp(n, c);
    case Node::Kind::Accent: return measureAccent(n, c);
    case Node::Kind::Space: {
        MathBox b;
        b.width = n.space * c.size();
        return b;
    }
    case Node::Kind::Fenced: return measureFenced(n, c);
    case Node::Kind::Table: return measureTable(n, c);
    }
    return {};
}

void drawText(ImDrawList* dl, const Ctx& c, const std::string& text, float size, ImVec2 baselineLeft, bool bold = false)
{
    const ImVec2 pos(baselineLeft.x, baselineLeft.y - ascentOf(c.font, size));
    dl->AddText(c.font, size, pos, c.color, text.c_str());
    if (bold)
        dl->AddText(c.font, size, ImVec2(pos.x + std::max(0.5f, size / 24.0f), pos.y), c.color, text.c_str());
}

// A glyph stretched vertically to `height` (delimiters): drawn at a larger size and centred on
// the content's axis.
void drawStretched(ImDrawList* dl, const Ctx& c, const std::string& glyph, float x, float axisY, float height, float slot)
{
    const float normal = c.size();
    const MathBox g = textBox(c, glyph, normal);
    const float factor = std::clamp(height / std::max(1.0f, g.height() * 0.8f), 1.0f, 3.0f);
    const float size = normal * factor;
    const float w = c.font->CalcTextSizeA(size, FLT_MAX, 0.0f, glyph.c_str()).x;
    const float a = ascentOf(c.font, size), d = descentOf(c.font, size);
    // Centre the glyph's body (ascent..descent) on the axis.
    const float top = axisY - (a + d) * 0.5f + (a - d) * 0.0f;
    dl->AddText(c.font, size, ImVec2(x + (slot - w) * 0.5f, top - 0.1f * size), c.color, glyph.c_str());
}

// A delimiter from `top` to `bottom` in a slot `slot` wide at `x`, drawn as lines: ( ) [ ] { } | ‖ ⟨ ⟩
// taller than a glyph stretches well (a matrix, cases). False (nothing drawn) for others and short ones,
// which come from the font.
bool drawDelimiter(ImDrawList* dl, const Ctx& c, const std::string& glyph, float x, float top, float bottom, float slot)
{
    const float size = c.size(), height = bottom - top;
    const bool left = glyph == "(" || glyph == "[" || glyph == "{" || glyph == "⟨";
    const bool right = glyph == ")" || glyph == "]" || glyph == "}" || glyph == "⟩";
    const bool bar = glyph == "|" || glyph == "‖";
    if (height < 1.8f * size || !(left || right || bar))
        return false;
    const float t = std::max(1.0f, c.s * size / 14.0f);
    // Shapes drawn as the left one; the right one mirrored. u across the slot (0 outer, 1 inner), v down.
    auto p = [&](float u, float v) {
        const float ux = 0.2f + 0.6f * (left ? u : 1.0f - u);
        return ImVec2(x + slot * ux, top + height * v);
    };
    const char k = bar ? '|' : glyph == "(" || glyph == ")" ? '(' : glyph == "[" || glyph == "]" ? '[' : glyph == "{" || glyph == "}" ? '{' : '<';
    switch (k) {
    case '|':
        if (glyph == "|") {
            dl->AddLine(ImVec2(x + slot * 0.5f, top), ImVec2(x + slot * 0.5f, bottom), c.color, t);
        } else {
            dl->AddLine(ImVec2(x + slot * 0.35f, top), ImVec2(x + slot * 0.35f, bottom), c.color, t);
            dl->AddLine(ImVec2(x + slot * 0.65f, top), ImVec2(x + slot * 0.65f, bottom), c.color, t);
        }
        break;
    case '(': {
        constexpr int kSteps = 24;
        for (int i = 0; i <= kSteps; ++i) {
            const float v = float(i) / kSteps;
            dl->PathLineTo(p(1.0f - std::sin(v * 3.14159265f), v));
        }
        dl->PathStroke(c.color, t * 1.2f);
        break;
    }
    case '[':
        dl->PathLineTo(p(0.9f, 0.0f));
        dl->PathLineTo(p(0.15f, 0.0f));
        dl->PathLineTo(p(0.15f, 1.0f));
        dl->PathLineTo(p(0.9f, 1.0f));
        dl->PathStroke(c.color, t);
        break;
    case '{': {
        const float r = std::min(0.12f, 0.25f * size / height); // the curls, as a fraction of the height
        dl->PathLineTo(p(1.0f, 0.0f));
        dl->PathBezierQuadraticCurveTo(p(0.5f, 0.0f), p(0.5f, r), 0);
        dl->PathLineTo(p(0.5f, 0.5f - r));
        dl->PathBezierQuadraticCurveTo(p(0.5f, 0.5f), p(0.0f, 0.5f), 0);
        dl->PathBezierQuadraticCurveTo(p(0.5f, 0.5f), p(0.5f, 0.5f + r), 0);
        dl->PathLineTo(p(0.5f, 1.0f - r));
        dl->PathBezierQuadraticCurveTo(p(0.5f, 1.0f), p(1.0f, 1.0f), 0);
        dl->PathStroke(c.color, t);
        break;
    }
    default: // ⟨ ⟩
        dl->PathLineTo(p(1.0f, 0.0f));
        dl->PathLineTo(p(0.0f, 0.5f));
        dl->PathLineTo(p(1.0f, 1.0f));
        dl->PathStroke(c.color, t);
        break;
    }
    return true;
}

void draw(ImDrawList* dl, const Node& n, ImVec2 at, const Ctx& c)
{
    const float size = c.size();
    switch (n.kind) {
    case Node::Kind::Row: {
        float x = at.x;
        for (const Node& child : n.children) {
            draw(dl, child, ImVec2(x, at.y), c);
            x += measure(child, c).width;
        }
        break;
    }
    case Node::Kind::Text: {
        const float pad = n.binary ? kBinaryPad * size : 0.0f;
        drawText(dl, c, n.text, size, ImVec2(at.x + pad, at.y), n.bold);
        break;
    }
    case Node::Kind::Frac: {
        const Ctx inner = c.display ? c : c.script();
        const MathBox num = measure(n.children[0], inner), den = measure(n.children[1], inner);
        const MathBox whole = measureFrac(n, c);
        const float gap = kFracGap * size, axis = kAxis * size;
        const float ruleY = at.y - axis;
        const float thickness = std::max(1.0f, c.s * size / 16.0f);
        dl->AddLine(ImVec2(at.x + 0.06f * size, ruleY), ImVec2(at.x + whole.width - 0.06f * size, ruleY), c.color, thickness);
        draw(dl, n.children[0], ImVec2(at.x + (whole.width - num.width) * 0.5f, ruleY - gap - num.descent), inner);
        draw(dl, n.children[1], ImVec2(at.x + (whole.width - den.width) * 0.5f, ruleY + gap + den.ascent), inner);
        break;
    }
    case Node::Kind::Scripts: {
        const Node& baseNode = n.children[0];
        const bool hasSup = !empty(n.children[1]), hasSub = !empty(n.children[2]);
        const Ctx sc = c.script();
        const MathBox base = measure(baseNode, c);
        const MathBox sup = hasSup ? measure(n.children[1], sc) : MathBox{};
        const MathBox sub = hasSub ? measure(n.children[2], sc) : MathBox{};
        if (baseNode.kind == Node::Kind::Op && baseNode.limitsAbove && c.display) {
            const MathBox whole = measureScripts(n, c);
            const float gap = 0.1f * size;
            draw(dl, baseNode, ImVec2(at.x + (whole.width - base.width) * 0.5f, at.y), c);
            if (hasSup)
                draw(dl, n.children[1], ImVec2(at.x + (whole.width - sup.width) * 0.5f, at.y - base.ascent - gap - sup.descent), sc);
            if (hasSub)
                draw(dl, n.children[2], ImVec2(at.x + (whole.width - sub.width) * 0.5f, at.y + base.descent + gap + sub.ascent), sc);
            break;
        }
        draw(dl, baseNode, at, c);
        const float x = at.x + base.width + 0.04f * size;
        const float supShift = std::max(kSupShift * size, base.ascent - 0.55f * sc.size());
        const float subShift = std::max(kSubShift * size, base.descent + 0.1f * size);
        if (hasSup)
            draw(dl, n.children[1], ImVec2(x, at.y - supShift), sc);
        if (hasSub)
            draw(dl, n.children[2], ImVec2(x, at.y + subShift), sc);
        break;
    }
    case Node::Kind::Sqrt: {
        const MathBox rad = measure(n.children[0], c);
        const MathBox whole = measureSqrt(n, c);
        const float rule = std::max(1.0f, c.s * size / 14.0f);
        const float top = at.y - whole.ascent + rule * 0.5f;
        const float bottom = at.y + rad.descent;
        const float signW = 0.75f * size;
        const float x0 = at.x + (whole.width - signW - rad.width - 0.15f * size); // room for an index, if any
        // The radical: a short down stroke, a long up stroke, then the bar over the radicand.
        const ImVec2 p0(x0 + 0.05f * size, at.y - 0.25f * size);
        const ImVec2 p1(x0 + 0.3f * size, bottom);
        const ImVec2 p2(x0 + signW, top);
        const ImVec2 p3(at.x + whole.width - 0.05f * size, top);
        dl->AddLine(p0, p1, c.color, rule);
        dl->AddLine(p1, p2, c.color, rule);
        dl->AddLine(p2, p3, c.color, rule);
        draw(dl, n.children[0], ImVec2(x0 + signW + 0.05f * size, at.y), c);
        if (!empty(n.children[1])) {
            const Ctx ic = c.script().script();
            const MathBox index = measure(n.children[1], ic);
            draw(dl, n.children[1], ImVec2(x0 + 0.35f * size - index.width, at.y - 0.55f * size), ic);
        }
        break;
    }
    case Node::Kind::Op: {
        const bool glyph = n.text.size() <= 4 && static_cast<unsigned char>(n.text[0]) >= 0x80;
        const float opSize = size * (glyph && c.display ? kOpScale : 1.0f);
        drawText(dl, c, n.text, opSize, ImVec2(at.x + 0.05f * size, at.y + (glyph && c.display ? 0.12f * size : 0.0f)));
        break;
    }
    case Node::Kind::Accent: {
        const MathBox base = measure(n.children[0], c);
        draw(dl, n.children[0], at, c);
        const float top = at.y - std::max(base.ascent, 0.72f * size);
        if (n.text == "‾") {
            dl->AddLine(ImVec2(at.x, top - 0.1f * size), ImVec2(at.x + base.width, top - 0.1f * size), c.color, std::max(1.0f, c.s));
            break;
        }
        const float aSize = size * 0.8f;
        const float w = c.font->CalcTextSizeA(aSize, FLT_MAX, 0.0f, n.text.c_str()).x;
        // Accents are tall glyphs with the mark at their top: lift so the mark sits on the base.
        dl->AddText(c.font, aSize, ImVec2(at.x + (base.width - w) * 0.5f, top - 0.62f * aSize), c.color, n.text.c_str());
        break;
    }
    case Node::Kind::Space:
        break;
    case Node::Kind::Fenced: {
        const MathBox content = measure(n.children[0], c);
        const float slot = 0.45f * size;
        const float axisY = at.y - kAxis * size;
        const float height = std::max(content.height(), size);
        const float half = fenceHalf(content, c);
        auto delimiter = [&](const std::string& glyph, float dx) {
            if (!drawDelimiter(dl, c, glyph, dx, axisY - half, axisY + half, slot))
                drawStretched(dl, c, glyph, dx, axisY, height, slot);
        };
        float x = at.x;
        if (!n.text.empty()) {
            delimiter(n.text, x);
            x += slot + 0.05f * size;
        }
        draw(dl, n.children[0], ImVec2(x, at.y), c);
        x += content.width;
        if (!n.text2.empty())
            delimiter(n.text2, x + 0.05f * size);
        break;
    }
    case Node::Kind::Table: {
        const Ctx k = cellContext(n, c);
        float widths[kMaxColumns] = {};
        columnWidths(n, k, widths);
        float y = at.y - measureTable(n, c).ascent; // the top of the first row
        for (const Node& cells : n.children) {
            const MathBox rb = rowBox(cells, k);
            float x = at.x;
            for (size_t j = 0; j < cells.children.size(); ++j) {
                const Node& cell = cells.children[j];
                const float w = widths[columnOf(j)], cw = measure(cell, k).width;
                const char a = alignOf(n, j);
                const float cx = a == 'l' ? x : a == 'r' ? x + w - cw : x + (w - cw) * 0.5f;
                draw(dl, cell, ImVec2(cx, y + rb.ascent), k);
                x += w + gapAfter(n, j, size);
            }
            y += rb.height() + kRowGap * size;
        }
        break;
    }
    }
}

// The lines of a long row broken to fit `maxWidth`: visit(begin, end, indent) for each, the
// items [begin, end). A line breaks before a binary operator or relation, the latest that fits;
// the later lines are indented to the first relation (continuing "= a + b" under it), or by 1 em
// when that is more than half the width.
template <typename Visit>
void forEachLine(const Node& n, const Ctx& c, float maxWidth, Visit&& visit)
{
    const std::vector<Node>& items = n.children;
    float indent = c.size(), x = 0.0f;
    for (const Node& item : items) {
        if (item.kind == Node::Kind::Text && item.relation) {
            if (x > 0.0f && x <= maxWidth * 0.5f)
                indent = x;
            break;
        }
        x += measure(item, c).width;
    }
    size_t start = 0, lastBreak = 0;
    float lineIndent = 0.0f, atBreak = 0.0f;
    x = 0.0f;
    for (size_t i = 0; i < items.size(); ++i) {
        const Node& item = items[i];
        if (i > start && item.kind == Node::Kind::Text && item.binary) {
            lastBreak = i;
            atBreak = x;
        }
        const float w = measure(item, c).width;
        if (x + w > maxWidth && lastBreak > start) {
            visit(start, lastBreak, lineIndent);
            x = indent + (x - atBreak);
            start = lastBreak;
            lineIndent = indent;
        }
        x += w;
    }
    visit(start, items.size(), lineIndent);
}

// A box of the items [begin, end) of a row.
MathBox lineBox(const Node& n, size_t begin, size_t end, const Ctx& c)
{
    MathBox b;
    b.ascent = 0.8f * c.size();
    b.descent = 0.3f * c.size();
    for (size_t i = begin; i < end; ++i) {
        const MathBox ib = measure(n.children[i], c);
        b.width += ib.width;
        b.ascent = std::max(b.ascent, ib.ascent);
        b.descent = std::max(b.descent, ib.descent);
    }
    return b;
}

bool breaks(const Node& n, const Ctx& c, float maxWidth)
{
    return n.kind == Node::Kind::Row && maxWidth > 0.0f && measure(n, c).width > maxWidth;
}

Ctx context(const MathStyle& style)
{
    Ctx c{style.font, style.size, style.scale, style.color, style.display, 0};
    return c;
}

} // namespace

MathBox measureMath(const Node& node, const MathStyle& style)
{
    if (!style.font)
        return {};
    return measure(node, context(style));
}

void drawMath(ImDrawList* dl, const Node& node, ImVec2 baseline, const MathStyle& style)
{
    if (!style.font || !dl)
        return;
    draw(dl, node, baseline, context(style));
}

MathBox measureMath(const Node& node, const MathStyle& style, float maxWidth)
{
    if (!style.font)
        return {};
    const Ctx c = context(style);
    if (!breaks(node, c, maxWidth))
        return measure(node, c);
    MathBox b;
    bool first = true;
    float previousDescent = 0.0f;
    forEachLine(node, c, maxWidth, [&](size_t begin, size_t end, float indent) {
        const MathBox lb = lineBox(node, begin, end, c);
        b.width = std::max(b.width, indent + lb.width);
        if (first)
            b.ascent = lb.ascent;
        else
            b.descent += previousDescent + kRowGap * c.size() + lb.ascent;
        previousDescent = lb.descent;
        first = false;
    });
    b.descent += previousDescent;
    return b;
}

void drawMath(ImDrawList* dl, const Node& node, ImVec2 baseline, const MathStyle& style, float maxWidth)
{
    if (!style.font || !dl)
        return;
    const Ctx c = context(style);
    if (!breaks(node, c, maxWidth)) {
        draw(dl, node, baseline, c);
        return;
    }
    float y = baseline.y, previousDescent = 0.0f;
    bool first = true;
    forEachLine(node, c, maxWidth, [&](size_t begin, size_t end, float indent) {
        const MathBox lb = lineBox(node, begin, end, c);
        if (!first)
            y += previousDescent + kRowGap * c.size() + lb.ascent;
        float x = baseline.x + indent;
        for (size_t i = begin; i < end; ++i) {
            draw(dl, node.children[i], ImVec2(x, y), c);
            x += measure(node.children[i], c).width;
        }
        previousDescent = lb.descent;
        first = false;
    });
}

} // namespace enp::view
