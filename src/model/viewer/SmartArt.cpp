// SmartArt laid out from its data model by the kind of its layout.
#include "SmartArt.h"

#include "Shapes.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <map>

namespace enp::model::viewer::smartart {

namespace {

using ooxml::XmlReader;
using Event = XmlReader::Event;

constexpr float kPi = 3.14159265f;

// A shape of the diagram and its text (a node's, then its children's as bullets when `bullets`).
struct Item {
    Rect box;
    std::string geometry = "rect"; // a preset, or "" for `polygon`
    std::vector<Point> polygon;
    Rect textBox;                    // where its text goes (its box unless set)
    bool hasTextBox = false;
    std::uint32_t fill = 0, stroke = 0, ink = 0;
    std::vector<std::pair<std::string, bool>> text; // (text, a child's bullet)
    bool top = false; // the text from its top (else centred)
};

std::size_t letters(std::string_view s)
{
    std::size_t n = 0;
    for (unsigned char c : s)
        n += (c & 0xC0) != 0x80;
    return n;
}

// Whether `item`'s text fits its box at `size` points (children at four fifths): an estimate from
// the letters (about half an em each), each paragraph wrapped, its longest word on one line.
bool fits(const Item& item, float size)
{
    const Rect b = item.hasTextBox ? item.textBox : item.box;
    const float w = b.w - 7.2f - 4.0f, h = b.h - 7.2f;
    if (w <= 4.0f || h <= 4.0f)
        return false;
    float height = 0.0f;
    for (const auto& [text, child] : item.text) {
        const float s = child ? size * 0.8f : size;
        const float indent = child ? 20.0f : 0.0f;
        std::size_t word = 0, longest = 0;
        for (unsigned char c : text) {
            if (c == ' ' || c == '\n')
                word = 0;
            else if ((c & 0xC0) != 0x80)
                longest = std::max(longest, ++word);
        }
        if (float(longest) * 0.55f * s > w - indent)
            return false;
        height += std::max(1.0f, std::ceil(float(letters(text)) * 0.52f * s / (w - indent))) * s * 1.2f;
    }
    return height <= h;
}

// The node's text, then its descendants' as bullets.
void textOf(const Tree& tree, int node, Item& item, bool withChildren)
{
    if (!tree.nodes[size_t(node)].text.empty())
        item.text.emplace_back(tree.nodes[size_t(node)].text, false);
    if (!withChildren)
        return;
    std::function<void(int, int)> below = [&](int n, int depth) {
        if (depth > 6)
            return;
        for (int child : tree.nodes[size_t(n)].children) {
            if (!tree.nodes[size_t(child)].text.empty() && item.text.size() < 64)
                item.text.emplace_back(tree.nodes[size_t(child)].text, true);
            below(child, depth + 1);
        }
    };
    below(node, 0);
}

PathItem arrowLine(const std::vector<Point>& pts, std::uint32_t color, float width)
{
    PathItem p;
    p.stroke = color;
    p.lineWidth = width;
    p.subpaths.push_back(pts);
    p.closed.push_back(false);
    p.tail = LineEnd::Triangle;
    return p;
}

} // namespace

bool readTree(std::string_view xml, Tree& out)
{
    struct Pt {
        std::string type, text;
        std::vector<std::pair<int, std::string>> children; // (order, id)
    };
    std::map<std::string, Pt> pts;
    std::string root;
    XmlReader d(xml);
    for (Event e; (e = d.next()) != Event::Eof && e != Event::Error;) {
        if (e != Event::Start)
            continue;
        const std::string_view n = d.localName();
        if (n == "pt") {
            const std::string id = d.attribute("modelId");
            Pt& pt = pts[id];
            pt.type = d.hasAttribute("type") ? d.attribute("type") : "node";
            if (pt.type == "doc")
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
                    if (d.localName() == "p" && !pt.text.empty())
                        pt.text += '\n';
                    else if (d.localName() == "t")
                        pt.text += d.elementText();
                }
            }
            while (!pt.text.empty() && pt.text.back() == '\n')
                pt.text.pop_back();
        } else if (n == "cxn") {
            const std::string type = d.hasAttribute("type") ? d.attribute("type") : "parOf";
            if (type == "parOf")
                pts[d.attribute("srcId")].children.push_back({std::atoi(d.attribute("srcOrd").c_str()), d.attribute("destId")});
        }
    }
    out.nodes.clear();
    if (root.empty())
        return false;
    out.nodes.emplace_back();
    std::function<void(const std::string&, int, int)> add = [&](const std::string& id, int index, int depth) {
        if (depth > 8 || out.nodes.size() > 500)
            return;
        const auto it = pts.find(id);
        if (it == pts.end())
            return;
        std::vector<std::pair<int, std::string>> children = it->second.children;
        std::sort(children.begin(), children.end());
        for (const auto& [order, child] : children) {
            const auto c = pts.find(child);
            if (c == pts.end() || c->second.type != "node")
                continue;
            const int ci = int(out.nodes.size());
            out.nodes.push_back(Node{c->second.text, {}});
            out.nodes[size_t(index)].children.push_back(ci);
            add(child, ci, depth + 1);
        }
    };
    add(root, 0, 0);
    return !out.nodes[0].children.empty();
}

Kind kindOf(std::string_view layoutId)
{
    const std::size_t slash = layoutId.rfind('/');
    std::string k(slash == std::string_view::npos ? layoutId : layoutId.substr(slash + 1));
    for (char& c : k)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    auto has = [&](const char* s) { return k.find(s) != std::string::npos; };
    if (has("pyramid"))
        return Kind::Pyramid;
    if (has("orgchart") || has("hierarchy"))
        return Kind::Hierarchy;
    if (has("cycle") || has("radial"))
        return Kind::Cycle;
    if (has("chevron"))
        return Kind::Chevrons;
    if (k == "process2" || has("vprocess"))
        return Kind::VerticalProcess;
    if (has("process") || has("arrow"))
        return Kind::Process;
    if (has("hlist"))
        return Kind::HorizontalList;
    if (has("list"))
        return Kind::List;
    return Kind::Blocks;
}

bool draw(const Tree& tree, Kind kind, const ooxml::Theme& theme, Rect r, Page& page)
{
    if (tree.nodes.empty() || tree.nodes[0].children.empty() || r.w < 8.0f || r.h < 8.0f)
        return false;
    const std::vector<int>& top = tree.nodes[0].children;
    const int n = int(std::min<std::size_t>(top.size(), 64));
    const std::uint32_t accent = theme.color("accent1", rgba(0x44, 0x72, 0xC4)), light = theme.color("lt1", kWhite);
    const std::uint32_t dark = theme.color("dk1", kBlack), pale = ooxml::applyTint(accent, 0.8), soft = ooxml::applyTint(accent, 0.4);
    std::vector<Item> items;
    std::vector<PathItem> lines; // arrows and connectors, under the shapes
    auto shape = [&](Rect box, const char* geometry, int node, bool withChildren) -> Item& {
        Item item;
        item.box = box;
        item.geometry = geometry;
        item.fill = accent;
        item.stroke = light;
        item.ink = light;
        if (node >= 0)
            textOf(tree, node, item, withChildren);
        items.push_back(std::move(item));
        return items.back();
    };
    // The children of a node, as bullets in a box of their own (lists).
    auto childrenBox = [&](Rect box, int node, std::uint32_t fill) {
        Item item;
        item.box = box;
        item.fill = fill;
        item.ink = dark;
        item.top = true;
        for (int child : tree.nodes[size_t(node)].children)
            textOf(tree, child, item, true);
        for (auto& t : item.text)
            t.second = true;
        if (!item.text.empty())
            items.push_back(std::move(item));
    };
    switch (kind) {
    case Kind::Blocks: { // a grid of blocks 10:6, the last row centred
        int cols = 1;
        float unit = 0.0f;
        for (int c = 1; c <= n; ++c) {
            const int rows = (n + c - 1) / c;
            const float u = std::min(r.w / (float(c) + float(c - 1) * 0.1f), r.h / (float(rows) * 0.6f + float(rows - 1) * 0.1f));
            if (u > unit)
                unit = u, cols = c;
        }
        const int rows = (n + cols - 1) / cols;
        const float bw = unit, bh = unit * 0.6f, gap = unit * 0.1f;
        const float y0 = r.y + (r.h - (float(rows) * bh + float(rows - 1) * gap)) * 0.5f;
        for (int i = 0; i < n; ++i) {
            const int row = i / cols, inRow = row == rows - 1 ? n - row * cols : cols;
            const float x0 = r.x + (r.w - (float(inRow) * bw + float(inRow - 1) * gap)) * 0.5f;
            shape(Rect{x0 + float(i % cols) * (bw + gap), y0 + float(row) * (bh + gap), bw, bh}, "rect", top[size_t(i)], true);
        }
        break;
    }
    case Kind::List: { // bars down the frame, each node's children as bullets under its bar
        float units = 0.0f;
        for (int i = 0; i < n; ++i)
            units += 1.0f + 0.8f * float(tree.nodes[size_t(top[size_t(i)])].children.size()) + (i ? 0.15f : 0.0f);
        const float unit = std::min(r.h / units, r.w * 0.25f);
        float y = r.y + (r.h - unit * units) * 0.5f;
        for (int i = 0; i < n; ++i) {
            const int node = top[size_t(i)];
            if (i)
                y += unit * 0.15f;
            shape(Rect{r.x, y, r.w, unit}, "roundRect", node, false);
            y += unit;
            const float below = 0.8f * float(tree.nodes[size_t(node)].children.size()) * unit;
            if (below > 0.0f)
                childrenBox(Rect{r.x, y, r.w, below}, node, 0);
            y += below;
        }
        break;
    }
    case Kind::HorizontalList: { // columns: a header over its children
        const float gap = r.w * 0.03f, cw = (r.w - gap * float(n - 1)) / float(n);
        const float hh = std::min(r.h * 0.3f, cw * 0.5f);
        for (int i = 0; i < n; ++i) {
            const float x = r.x + float(i) * (cw + gap);
            shape(Rect{x, r.y, cw, hh}, "roundRect", top[size_t(i)], false);
            childrenBox(Rect{x, r.y + hh, cw, r.h - hh}, top[size_t(i)], pale);
        }
        break;
    }
    case Kind::Process:
    case Kind::VerticalProcess: { // boxes in a row (or a column) with arrows between them
        const bool down = kind == Kind::VerticalProcess;
        const float length = down ? r.h : r.w, across = down ? r.w : r.h;
        float bl = length / (float(n) + float(n - 1) * 0.6f);
        float ba = std::min(across, bl * (down ? 1.6f : 0.6f));
        if (down)
            bl = std::min(bl, ba * 0.6f);
        const float start = (length - (float(n) * bl + float(n - 1) * 0.6f * bl)) * 0.5f;
        for (int i = 0; i < n; ++i) {
            const float at = start + float(i) * 1.6f * bl;
            const Rect box = down ? Rect{r.x + (r.w - ba) * 0.5f, r.y + at, ba, bl} : Rect{r.x + at, r.y + (r.h - ba) * 0.5f, bl, ba};
            shape(box, "roundRect", top[size_t(i)], true);
            if (i + 1 < n) {
                Item arrow;
                arrow.geometry = down ? "downArrow" : "rightArrow";
                arrow.fill = soft;
                arrow.box = down ? Rect{r.x + (r.w - ba * 0.3f) * 0.5f, r.y + at + bl * 1.15f, ba * 0.3f, bl * 0.3f}
                                 : Rect{r.x + at + bl * 1.15f, r.y + (r.h - ba * 0.35f) * 0.5f, bl * 0.3f, ba * 0.35f};
                items.push_back(std::move(arrow));
            }
        }
        break;
    }
    case Kind::Chevrons: { // chevrons side by side, slightly apart
        const float cw = r.w / (float(n) * 0.92f + 0.08f), h = std::min(r.h, cw * 0.4f);
        for (int i = 0; i < n; ++i) {
            Item& item = shape(Rect{r.x + float(i) * cw * 0.92f, r.y + (r.h - h) * 0.5f, cw, h}, i == 0 ? "homePlate" : "chevron", top[size_t(i)], true);
            item.textBox = Rect{item.box.x + cw * (i == 0 ? 0.08f : 0.25f), item.box.y, cw * (i == 0 ? 0.7f : 0.55f), h};
            item.hasTextBox = true;
        }
        break;
    }
    case Kind::Cycle: { // circles round a circle, arrows from each to the next
        const float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f, side = std::min(r.w, r.h);
        const float d = std::min(side * 0.3f, n > 1 ? 2.0f * kPi * side * 0.35f / float(n) * 0.7f : side * 0.6f);
        const float radius = n > 1 ? (side - d) * 0.5f : 0.0f;
        for (int i = 0; i < n; ++i) {
            const float a = -kPi * 0.5f + 2.0f * kPi * float(i) / float(n);
            shape(Rect{cx + std::cos(a) * radius - d * 0.5f, cy + std::sin(a) * radius - d * 0.5f, d, d}, "ellipse", top[size_t(i)], true);
            if (n > 1) {
                const float skip = (d * 0.5f) / radius + 0.08f, b = a + 2.0f * kPi / float(n);
                std::vector<Point> arc;
                for (int k = 0; k <= 12; ++k) {
                    const float t = a + skip + (b - a - 2.0f * skip) * float(k) / 12.0f;
                    arc.push_back({cx + std::cos(t) * radius, cy + std::sin(t) * radius});
                }
                if (b - a > 2.0f * skip)
                    lines.push_back(arrowLine(arc, soft, std::max(1.0f, d * 0.06f)));
            }
        }
        break;
    }
    case Kind::Hierarchy: { // a tree, top down: each node over the middle of its leaves, joined by elbows
        std::vector<int> leaves(tree.nodes.size(), 1), depth(tree.nodes.size(), 0);
        int levels = 1;
        std::function<int(int, int)> count = [&](int node, int level) {
            depth[size_t(node)] = level;
            levels = std::max(levels, level + 1);
            int sum = 0;
            for (int child : tree.nodes[size_t(node)].children)
                sum += count(child, level + 1);
            return leaves[size_t(node)] = std::max(1, sum);
        };
        int total = 0;
        for (int i = 0; i < n; ++i)
            total += count(top[size_t(i)], 0);
        const float unit = r.w / (float(total) + float(total - 1) * 0.2f);
        const float bh = std::min(unit * 0.6f, r.h / (float(levels) + float(levels - 1) * 0.6f));
        const float vgap = levels > 1 ? std::min(bh, (r.h - float(levels) * bh) / float(levels - 1)) : 0.0f;
        const float y0 = r.y + (r.h - (float(levels) * bh + float(levels - 1) * vgap)) * 0.5f;
        auto slot = [&](float leaf) { return r.x + leaf * unit * 1.2f + unit * 0.5f; };
        std::function<float(int, int)> place = [&](int node, int first) {
            const int span = leaves[size_t(node)];
            const float x = (slot(float(first)) + slot(float(first + span - 1))) * 0.5f;
            const float y = y0 + float(depth[size_t(node)]) * (bh + vgap);
            shape(Rect{x - unit * 0.5f, y, unit, bh}, "rect", node, false);
            int at = first;
            for (int child : tree.nodes[size_t(node)].children) {
                const float childX = place(child, at);
                at += leaves[size_t(child)];
                PathItem elbow;
                elbow.stroke = accent;
                elbow.lineWidth = 1.5f;
                const float mid = y + bh + vgap * 0.5f;
                elbow.subpaths.push_back({{x, y + bh}, {x, mid}, {childX, mid}, {childX, y + bh + vgap}});
                elbow.closed.push_back(false);
                lines.push_back(std::move(elbow));
            }
            return x;
        };
        int at = 0;
        for (int i = 0; i < n; ++i) {
            place(top[size_t(i)], at);
            at += leaves[size_t(top[size_t(i)])];
        }
        break;
    }
    case Kind::Pyramid: { // a triangle cut into bands, the first node at the top
        const float h = r.h, w = std::min(r.w, h * 1.2f), cx = r.x + r.w * 0.5f, gap = h * 0.015f;
        auto half = [&](float y) { return (y - r.y) / h * w * 0.5f; };
        for (int i = 0; i < n; ++i) {
            const float y0 = r.y + h * float(i) / float(n) + (i ? gap * 0.5f : 0.0f);
            const float y1 = r.y + h * float(i + 1) / float(n) - (i + 1 < n ? gap * 0.5f : 0.0f);
            Item& item = shape(Rect{cx - half(y1), y0, half(y1) * 2.0f, y1 - y0}, "", top[size_t(i)], true);
            item.polygon = {{cx - half(y0), y0}, {cx + half(y0), y0}, {cx + half(y1), y1}, {cx - half(y1), y1}};
            // The text in the band's lower half, three quarters of its base wide (the top band is a
            // triangle: its tip has no room).
            const float ty = y0 + (y1 - y0) * 0.5f, tw = half(y1) * 2.0f * 0.75f;
            item.textBox = Rect{cx - tw * 0.5f, ty, tw, y1 - ty};
            item.hasTextBox = true;
        }
        break;
    }
    }
    // One text size for all of it (SmartArt's), the largest at which every shape's text fits.
    float size = 48.0f;
    for (const Item& item : items)
        if (!item.text.empty())
            while (size > 7.0f && !fits(item, size))
                size -= 0.5f;
    for (PathItem& line : lines)
        page.add(std::move(line));
    for (Item& item : items) {
        PathItem path;
        path.fill = item.fill;
        path.stroke = item.stroke;
        path.lineWidth = 1.5f;
        if (item.geometry.empty()) {
            path.subpaths.push_back(item.polygon);
        } else {
            std::vector<Point> pts;
            dml::presetOutline(item.geometry, item.box, pts);
            path.subpaths.push_back(std::move(pts));
        }
        path.closed.push_back(true);
        if (path.fill || path.stroke)
            page.add(std::move(path));
        if (item.text.empty())
            continue;
        TextBox box;
        box.box = item.hasTextBox ? item.textBox : item.box;
        box.anchor = item.top ? 0 : 1;
        for (const auto& [text, child] : item.text) {
            TextParagraph para;
            para.align = child ? 0 : 1;
            if (child)
                para.bullet = "\xE2\x80\xA2";
            TextRun run;
            run.text = text;
            run.size = child ? size * 0.8f : size;
            run.color = item.ink;
            run.font = theme.minorFont;
            para.runs.push_back(std::move(run));
            box.paragraphs.push_back(std::move(para));
        }
        page.add(std::move(box));
    }
    return true;
}

} // namespace enp::model::viewer::smartart
