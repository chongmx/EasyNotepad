// Search in fixed-layout pages (Viewer.h): a page's text in reading order with where each letter
// is drawn, a matcher that folds case and spacing, and the boxes a match covers on the page.
#include "enp/model/viewer/Viewer.h"

#include <algorithm>
#include <cmath>

namespace enp::model::viewer {

namespace {

// The code point at `p` (advanced past it); a byte that starts none is U+FFFD.
char32_t decode(const unsigned char*& p, const unsigned char* end)
{
    const unsigned char b = *p;
    int length = b < 0x80 ? 1 : (b >> 5) == 0x6 ? 2 : (b >> 4) == 0xE ? 3 : (b >> 3) == 0x1E ? 4 : 0;
    if (length == 0 || end - p < length) {
        ++p;
        return 0xFFFD;
    }
    char32_t c = length == 1 ? b : length == 2 ? (b & 0x1F) : length == 3 ? (b & 0x0F) : (b & 0x07);
    for (int i = 1; i < length; ++i) {
        if ((p[i] & 0xC0) != 0x80) {
            ++p;
            return 0xFFFD;
        }
        c = (c << 6) | (p[i] & 0x3F);
    }
    p += length;
    return c;
}

std::size_t codePoints(std::string_view s)
{
    std::size_t n = 0;
    for (const char c : s)
        n += (static_cast<unsigned char>(c) & 0xC0) != 0x80 ? 1 : 0;
    return n;
}

bool isSpace(char32_t c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0xA0 || (c >= 0x2000 && c <= 0x200A) || c == 0x202F || c == 0x205F ||
           c == 0x3000;
}

// Lower case for the letters with a simple pair: Latin (Basic, Latin-1, Extended-A), Greek, Cyrillic.
char32_t fold(char32_t c)
{
    if (c < 0x80)
        return c >= 'A' && c <= 'Z' ? c + 32 : c;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7)
        return c + 32;
    if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177))
        return c | 1u;
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E))
        return (c & 1u) ? c + 1 : c;
    if (c == 0x178)
        return 0xFF;
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2)
        return c + 32;
    if (c == 0x3C2) // final sigma
        return 0x3C3;
    if (c >= 0x410 && c <= 0x42F)
        return c + 32;
    if (c >= 0x400 && c <= 0x40F)
        return c + 80;
    return c;
}

// Text as compared: case folded (unless matching it), ligatures as their letters, each run of
// spaces as one; with the code points of the original each comes from ([first, last)).
struct Normalized {
    std::u32string text;
    std::vector<std::pair<std::size_t, std::size_t>> from;
};

void normalize(std::string_view utf8, bool matchCase, Normalized& out)
{
    const auto* p = reinterpret_cast<const unsigned char*>(utf8.data());
    const auto* end = p + utf8.size();
    out.text.reserve(utf8.size());
    out.from.reserve(utf8.size());
    for (std::size_t i = 0; p < end; ++i) {
        const char32_t c = decode(p, end);
        auto add = [&out, i](char32_t letter) {
            out.text.push_back(letter);
            out.from.emplace_back(i, i + 1);
        };
        if (isSpace(c)) {
            if (!out.text.empty() && out.text.back() == U' ')
                out.from.back().second = i + 1;
            else
                add(U' ');
            continue;
        }
        if (c == 0xAD) // a soft hyphen: not there for a search
            continue;
        if (c >= 0xFB00 && c <= 0xFB06) {
            static constexpr const char* kLetters[] = {"ff", "fi", "fl", "ffi", "ffl", "st", "st"};
            for (const char* l = kLetters[c - 0xFB00]; *l; ++l)
                add(matchCase ? char32_t(*l) : fold(char32_t(*l)));
            continue;
        }
        add(matchCase ? c : fold(c));
    }
}

void addLetters(std::string_view text, TextAt at, std::vector<TextAt>* where, bool count)
{
    if (!where)
        return;
    const std::size_t n = count ? codePoints(text) : 1;
    for (std::size_t k = 0; k < n; ++k) {
        where->push_back(at);
        if (at.letter >= 0)
            ++at.letter;
    }
}

Point along(const GlyphRun& g, float distance, float up)
{
    const float a = g.angle * 3.14159265f / 180.0f, dx = std::cos(a), dy = std::sin(a);
    // Along the run (dx, dy), and up from its baseline (dy, -dx): y grows downwards.
    return Point{g.x + dx * distance + dy * up, g.baseline + dy * distance - dx * up};
}

TextHit::Quad runQuad(const GlyphRun& g, std::int32_t first, std::int32_t last)
{
    const std::size_t n = g.offsets.size();
    const float x0 = std::size_t(first) < n ? g.offsets[std::size_t(first)] : g.width;
    float x1 = std::size_t(last) + 1 < n ? g.offsets[std::size_t(last) + 1] : g.width;
    float lo = std::min(x0, x1), hi = std::max(x0, x1);
    if (hi - lo < g.size * 0.1f)
        hi = lo + g.size * 0.5f;
    const float above = g.size * 0.9f, below = g.size * 0.25f;
    return TextHit::Quad{{along(g, lo, above), along(g, hi, above), along(g, hi, -below), along(g, lo, -below)}};
}

TextHit::Quad boxQuad(const TextBox& b)
{
    const float cx = b.box.x + b.box.w * 0.5f, cy = b.box.y + b.box.h * 0.5f;
    const float a = b.rotation * 3.14159265f / 180.0f, c = std::cos(a), s = std::sin(a);
    auto at = [&](float x, float y) { return Point{cx + x * c - y * s, cy + x * s + y * c}; };
    const float hw = b.box.w * 0.5f, hh = b.box.h * 0.5f;
    return TextHit::Quad{{at(-hw, -hh), at(hw, -hh), at(hw, hh), at(-hw, hh)}};
}

} // namespace

std::string pageText(const Page& page, std::vector<TextAt>* where)
{
    std::string out;
    if (where)
        where->clear();
    // Glyph runs in reading order: by line (baseline, within half a size), then left to right.
    std::vector<const GlyphRun*> runs; // invisible text too (the words over a scanned page)
    runs.reserve(page.glyphs.size());
    for (const GlyphRun& g : page.glyphs)
        runs.push_back(&g);
    std::stable_sort(runs.begin(), runs.end(), [](const GlyphRun* a, const GlyphRun* b) {
        if (std::abs(a->baseline - b->baseline) > std::min(a->size, b->size) * 0.5f)
            return a->baseline < b->baseline;
        return a->x < b->x;
    });
    const GlyphRun* previous = nullptr;
    for (const GlyphRun* g : runs) {
        if (previous) {
            if (std::abs(g->baseline - previous->baseline) > std::min(g->size, previous->size) * 0.5f) {
                out += '\n';
                addLetters("\n", TextAt{}, where, false);
            } else if (g->x > previous->x + previous->width + g->size * 0.15f && !out.empty() && out.back() != ' ') {
                out += ' ';
                addLetters(" ", TextAt{}, where, false);
            }
        }
        out += g->text;
        addLetters(g->text, TextAt{std::int32_t(g - page.glyphs.data()), 0, -1}, where, true);
        previous = g;
    }
    for (std::size_t bi = 0; bi < page.boxes.size(); ++bi) {
        const TextAt box{-1, -1, std::int32_t(bi)};
        for (const TextParagraph& p : page.boxes[bi].paragraphs) {
            std::string line;
            for (const TextRun& r : p.runs)
                line += r.text == "\n" ? std::string(" ") : r.text;
            if (line.empty())
                continue;
            if (!out.empty()) {
                out += '\n';
                addLetters("\n", TextAt{}, where, false);
            }
            if (!p.bullet.empty()) {
                out += p.bullet + " ";
                addLetters(p.bullet + " ", box, where, true);
            }
            out += line;
            addLetters(line, box, where, true);
        }
    }
    return out;
}

std::vector<std::pair<std::size_t, std::size_t>> findText(std::string_view text, std::string_view query, bool matchCase)
{
    std::vector<std::pair<std::size_t, std::size_t>> found;
    Normalized needle, hay;
    normalize(query, matchCase, needle);
    if (needle.text.find_first_not_of(U' ') == std::u32string::npos)
        return found;
    normalize(text, matchCase, hay);
    const std::size_t m = needle.text.size();
    for (std::size_t i = hay.text.find(needle.text); i != std::u32string::npos; i = hay.text.find(needle.text, i + m))
        found.emplace_back(hay.from[i].first, hay.from[i + m - 1].second);
    return found;
}

std::vector<TextHit> findOnPage(const Page& page, std::string_view query, bool matchCase)
{
    std::vector<TextHit> hits;
    std::vector<TextAt> where;
    const std::string text = pageText(page, &where);
    const auto found = findText(text, query, matchCase);
    hits.reserve(found.size());
    for (const auto& [first, last] : found) {
        TextHit hit;
        std::int32_t run = -1, from = 0, to = 0, box = -1;
        auto flush = [&]() {
            if (run >= 0)
                hit.quads.push_back(runQuad(page.glyphs[std::size_t(run)], from, to));
            run = -1;
        };
        for (std::size_t i = first; i < last && i < where.size(); ++i) {
            const TextAt& at = where[i];
            if (at.box >= 0) {
                flush();
                if (at.box != box) {
                    box = at.box;
                    hit.quads.push_back(boxQuad(page.boxes[std::size_t(at.box)]));
                }
                continue;
            }
            if (at.run < 0)
                continue; // a space or line break between runs
            if (at.run == run && at.letter == to + 1) {
                to = at.letter;
                continue;
            }
            flush();
            run = at.run;
            from = to = at.letter;
        }
        flush();
        hits.push_back(std::move(hit));
    }
    return hits;
}

Rect TextHit::bounds() const
{
    if (quads.empty())
        return Rect{};
    float x0 = quads[0].corners[0].x, y0 = quads[0].corners[0].y, x1 = x0, y1 = y0;
    for (const Quad& q : quads)
        for (const Point& p : q.corners) {
            x0 = std::min(x0, p.x);
            y0 = std::min(y0, p.y);
            x1 = std::max(x1, p.x);
            y1 = std::max(y1, p.y);
        }
    return Rect{x0, y0, x1 - x0, y1 - y0};
}

} // namespace enp::model::viewer
