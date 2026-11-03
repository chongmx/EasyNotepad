#include "enp/view/MapView.h"

#include "enp/os/Memory.h"
#include "enp/view/PanelFrame.h"

#include "enp/view/UiAutomation.h"

#include "IconsFontAwesome6.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace enp::view {

using model::LogicalKind;
using model::LogicalNode;
using viewmodel::MapLens;

namespace {

// World-space metrics (multiplied by the UI scale and the map zoom when drawn).
constexpr float kBoxW = 230.0f;
constexpr float kBoxH = 96.0f;
constexpr float kFlowW = 380.0f;
constexpr float kFlowH = 78.0f;
constexpr float kGapX = 44.0f;
constexpr float kGapY = 56.0f;
constexpr float kHeader = 34.0f;
constexpr double kAnimSeconds = 0.22;

const char* kindIcon(LogicalKind kind)
{
    switch (kind) {
    case LogicalKind::System: return ICON_FA_DIAGRAM_PROJECT;
    case LogicalKind::Module: return ICON_FA_LAYER_GROUP;
    case LogicalKind::Component: return ICON_FA_PUZZLE_PIECE;
    case LogicalKind::Function: return ICON_FA_CODE;
    case LogicalKind::Step: return ICON_FA_SHOE_PRINTS;
    case LogicalKind::Feature: return ICON_FA_ROUTE;
    }
    return ICON_FA_CIRCLE;
}

ImU32 withAlpha(ImU32 c, float a)
{
    ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
    v.w *= a;
    return ImGui::ColorConvertFloat4ToU32(v);
}

std::string ellipsize(const std::string& text, float width)
{
    if (ImGui::CalcTextSize(text.c_str()).x <= width)
        return text;
    std::string out = text;
    const float ew = ImGui::CalcTextSize("\xE2\x80\xA6").x;
    while (!out.empty() && ImGui::CalcTextSize(out.c_str()).x + ew > width) {
        out.pop_back();
        while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80)
            out.pop_back();
    }
    return out + "\xE2\x80\xA6";
}

std::vector<std::string> wrapLines(const std::string& text, float width, int maxLines)
{
    std::vector<std::string> lines;
    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize();
    const char* p = text.c_str();
    const char* end = p + text.size();
    while (p < end && int(lines.size()) < maxLines) {
        const char* wrap = font->CalcWordWrapPosition(size, p, end, width);
        if (wrap == p)
            wrap = p + 1;
        std::string line(p, wrap);
        while (!line.empty() && line.back() == ' ')
            line.pop_back();
        p = wrap;
        while (p < end && (*p == ' ' || *p == '\n'))
            ++p;
        if (int(lines.size()) == maxLines - 1 && p < end)
            line = ellipsize(line + " " + std::string(p, end), width);
        lines.push_back(line);
    }
    return lines;
}

} // namespace

MapView::MapView(viewmodel::NotepadViewModel& vm, const Fonts& fonts, const Palette& palette, float uiScale)
    : vm_(vm), fonts_(fonts), palette_(palette), s_(uiScale)
{
}

// ---- Layout -------------------------------------------------------------------------------------

void MapView::layout()
{
    viewmodel::LogicalMapViewModel& logic = vm_.logicalMap();
    const model::LogicalMap& map = logic.map();
    const std::string focus = logic.focus();
    const std::string key = focus + "|" + std::to_string(map.nodes.size()) + "|" + map.source;
    if (key == layoutKey_)
        return;
    layoutKey_ = key;
    needsFit_ = true;
    boxes_.clear();
    arrows_.clear();
    const LogicalNode* f = map.find(focus);
    if (!f) {
        releaseLayout(); // nothing to lay out: no room kept for the last map either
        layoutKey_ = key;
        return;
    }

    std::vector<const LogicalNode*> children;
    for (const std::string& id : f->children)
        if (const LogicalNode* c = map.find(id))
            children.push_back(c);
    flow_ = f->kind == LogicalKind::Function || f->kind == LogicalKind::Feature ||
            (!children.empty() && children.front()->kind == LogicalKind::Step);

    if (flow_) {
        // A flow reads top to bottom, wrapping into columns of eight steps.
        for (size_t i = 0; i < children.size(); ++i) {
            const int column = int(i / 8), row = int(i % 8);
            boxes_.push_back({children[i]->id, ImVec2(float(column) * (kFlowW + kGapX * 1.5f), float(row) * (kFlowH + 34.0f)),
                              ImVec2(kFlowW, kFlowH)});
            if (i > 0)
                arrows_.push_back({int(i) - 1, int(i), 1, std::string()});
        }
        return;
    }

    // Dependency layers: a part sits below everything that uses it (users on top, foundations below).
    std::map<std::string, int> index;
    for (size_t i = 0; i < children.size(); ++i)
        index[children[i]->id] = int(i);
    for (const model::LogicalEdge& e : map.edgesAmongChildren(focus)) {
        const auto a = index.find(e.from), b = index.find(e.to);
        if (a != index.end() && b != index.end())
            arrows_.push_back({a->second, b->second, e.weight, e.label});
    }
    // Ranks by longest path over the relations without their cycles (a header and its users often
    // include each other): a depth-first pass from the biggest users drops the back edges.
    const size_t count = children.size();
    std::vector<std::vector<int>> out(count);
    std::vector<int> fanIn(count, 0);
    for (const Arrow& a : arrows_)
        if (a.from != a.to) {
            out[size_t(a.from)].push_back(a.to);
            ++fanIn[size_t(a.to)];
        }
    std::vector<int> starts(count);
    for (size_t i = 0; i < count; ++i)
        starts[i] = int(i);
    std::stable_sort(starts.begin(), starts.end(), [&](int a, int b) {
        return int(out[size_t(a)].size()) - fanIn[size_t(a)] > int(out[size_t(b)].size()) - fanIn[size_t(b)];
    });
    std::vector<int> state(count, 0), order; // 0 new, 1 on the stack, 2 done; order: post-order
    std::vector<std::pair<int, int>> forward;
    auto visit = [&](auto&& self, int v) -> void {
        state[size_t(v)] = 1;
        for (int w : out[size_t(v)]) {
            if (state[size_t(w)] == 1)
                continue; // back edge
            forward.emplace_back(v, w);
            if (state[size_t(w)] == 0)
                self(self, w);
        }
        state[size_t(v)] = 2;
        order.push_back(v);
    };
    for (int v : starts)
        if (state[size_t(v)] == 0)
            visit(visit, v);
    std::vector<int> rank(count, 0);
    for (auto it = order.rbegin(); it != order.rend(); ++it) // topological order
        for (const auto& [from, to] : forward)
            if (from == *it)
                rank[size_t(to)] = std::max(rank[size_t(to)], rank[size_t(from)] + 1);
    if (arrows_.empty()) {
        // No relations: a balanced grid.
        const int columns = std::max(1, std::min(4, int(std::ceil(std::sqrt(double(count))))));
        for (size_t i = 0; i < count; ++i)
            rank[i] = int(i) / columns;
    }
    std::map<int, std::vector<int>> rows;
    for (size_t i = 0; i < count; ++i)
        rows[rank[i]].push_back(int(i));
    // Order within a rank by the average position of the parts that use them (fewer crossings),
    // and wrap a crowded rank onto several lines.
    constexpr size_t kPerLine = 4;
    const float boxH = f->kind == LogicalKind::Component ? kBoxH * 0.7f : kBoxH; // functions: name + one-line summary
    std::vector<float> x(count, 0.0f);
    float widest = 0.0f;
    for (auto& [r, members] : rows)
        widest = std::max(widest, float(std::min(members.size(), kPerLine)) * (kBoxW + kGapX) - kGapX);
    boxes_.resize(count);
    int line = 0;
    for (auto& [r, members] : rows) {
        if (r > 0)
            std::stable_sort(members.begin(), members.end(), [&](int a, int b) {
                auto center = [&](int v) {
                    float sum = 0.0f;
                    int n = 0;
                    for (const Arrow& arrow : arrows_)
                        if (arrow.to == v && rank[size_t(arrow.from)] < r) {
                            sum += x[size_t(arrow.from)];
                            ++n;
                        }
                    return n ? sum / float(n) : 1e9f;
                };
                return center(a) < center(b);
            });
        for (size_t first = 0; first < members.size(); first += kPerLine, ++line) {
            const size_t n = std::min(kPerLine, members.size() - first);
            const float rowW = float(n) * (kBoxW + kGapX) - kGapX;
            float cx = (widest - rowW) * 0.5f;
            // Wrapped lines of one rank sit closer together than ranks do.
            const float y = float(line) * (boxH + kGapY) - float(first / kPerLine) * kGapY * 0.5f;
            for (size_t k = first; k < first + n; ++k) {
                const int m = members[k];
                boxes_[size_t(m)] = {children[size_t(m)]->id, ImVec2(cx, y), ImVec2(kBoxW, boxH)};
                x[size_t(m)] = cx + kBoxW * 0.5f;
                cx += kBoxW + kGapX;
            }
        }
    }
}

void MapView::fit(ImVec2 canvas)
{
    if (boxes_.empty()) {
        pan_ = ImVec2(0.0f, 0.0f);
        zoom_ = 1.0f;
        return;
    }
    ImVec2 lo(1e9f, 1e9f), hi(-1e9f, -1e9f);
    for (const Box& b : boxes_) {
        lo = ImVec2(std::min(lo.x, b.pos.x), std::min(lo.y, b.pos.y));
        hi = ImVec2(std::max(hi.x, b.pos.x + b.size.x), std::max(hi.y, b.pos.y + b.size.y));
    }
    const float margin = 40.0f;
    const float w = (hi.x - lo.x + 2.0f * margin) * s_, h = (hi.y - lo.y + 2.0f * margin) * s_;
    zoom_ = std::clamp(std::min(canvas.x / w, canvas.y / h), 0.8f, 1.0f); // legible first; pan for the rest
    pan_ = ImVec2(std::max(margin * s_ * zoom_, (canvas.x - (hi.x - lo.x) * s_ * zoom_) * 0.5f) - lo.x * s_ * zoom_,
                  std::max(margin * s_ * zoom_, (canvas.y - (hi.y - lo.y) * s_ * zoom_) * 0.5f) - lo.y * s_ * zoom_);
}

void MapView::releaseLayout()
{
    os::mem::release(boxes_);
    os::mem::release(arrows_);
    os::mem::release(layoutKey_);
    needsFit_ = true;
}

void MapView::releaseIfMapGone()
{
    if ((!boxes_.empty() || boxes_.capacity() || arrows_.capacity() || !layoutKey_.empty()) &&
        !vm_.logicalMap().built()) // not map(): that would build one
        releaseLayout();
}

size_t MapView::layoutBytes() const
{
    size_t n = boxes_.capacity() * sizeof(Box) + arrows_.capacity() * sizeof(Arrow);
    const size_t inlineRoom = std::string().capacity();
    auto heap = [inlineRoom](const std::string& t) { return t.capacity() > inlineRoom ? t.capacity() + 1 : size_t(0); };
    for (const Box& b : boxes_)
        n += heap(b.id);
    for (const Arrow& a : arrows_)
        n += heap(a.label);
    return n + heap(layoutKey_);
}

// ---- Frame --------------------------------------------------------------------------------------

void MapView::render(ImVec2 size)
{
    viewmodel::LogicalMapViewModel& logic = vm_.logicalMap();
    ImGui::PushFont(fonts_.ui, Fonts::kUiSize * 0.9f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    panel::beginChild("##logicalMap", size, palette_.editorBg, ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    const ImVec2 p = ImGui::GetWindowPos();
    const float w = ImGui::GetWindowWidth(), h = ImGui::GetWindowHeight();
    renderHeader(w);
    layout();

    const ImVec2 canvasMin(p.x, p.y + kHeader * s_);
    const ImVec2 canvasSize(w, h - kHeader * s_);
    if (needsFit_) {
        fit(canvasSize);
        needsFit_ = false;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Dot grid, moving with the pan for a sense of place.
    dl->PushClipRect(canvasMin, ImVec2(p.x + w, p.y + h), true);
    const float step = 24.0f * s_ * zoom_;
    for (float gx = std::fmod(pan_.x, step); gx < w; gx += step)
        for (float gy = std::fmod(pan_.y, step); gy < canvasSize.y; gy += step)
            dl->AddCircleFilled(ImVec2(canvasMin.x + gx, canvasMin.y + gy), 1.0f * s_, palette_.indentGuide);

    // Interaction on the empty canvas: drag to pan, wheel to zoom around the cursor.
    ImGui::SetCursorScreenPos(canvasMin);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##mapCanvas", canvasSize, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    const bool canvasHovered = ImGui::IsItemHovered();
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 2.0f))) {
        pan_.x += io.MouseDelta.x;
        pan_.y += io.MouseDelta.y;
        panning_ = true;
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseDown(ImGuiMouseButton_Middle))
        panning_ = false;
    const bool overCanvas = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                            ImGui::IsMouseHoveringRect(canvasMin, ImVec2(p.x + w, p.y + h));
    if (overCanvas && io.MouseWheel != 0.0f) {
        const float old = zoom_;
        zoom_ = std::clamp(zoom_ * (io.MouseWheel > 0.0f ? 1.12f : 1.0f / 1.12f), 0.35f, 1.8f);
        const ImVec2 m(io.MousePos.x - canvasMin.x, io.MousePos.y - canvasMin.y);
        pan_ = ImVec2(m.x - (m.x - pan_.x) * zoom_ / old, m.y - (m.y - pan_.y) * zoom_ / old);
    }
    if (overCanvas && ImGui::IsMouseClicked(3))
        logic.zoomOut();

    // Zoom transition: the new level grows in (or settles back when zooming out).
    if (logic.zoomSerial() != animSerial_) {
        if (animSerial_ >= 0)
            animStart_ = ImGui::GetTime();
        animSerial_ = logic.zoomSerial();
    }
    const float t = float(std::clamp((ImGui::GetTime() - animStart_) / kAnimSeconds, 0.0, 1.0));
    const float e = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
    const float animScale = 1.0f + float(logic.zoomDirection()) * -0.12f * (1.0f - e);
    const float scale = zoom_ * animScale * s_;
    const ImVec2 center(canvasMin.x + canvasSize.x * 0.5f, canvasMin.y + canvasSize.y * 0.5f);
    auto toScreen = [&](ImVec2 world) {
        const ImVec2 base(canvasMin.x + pan_.x + world.x * zoom_ * s_, canvasMin.y + pan_.y + world.y * zoom_ * s_);
        return ImVec2(center.x + (base.x - center.x) * animScale, center.y + (base.y - center.y) * animScale);
    };

    // Which box is under the mouse.
    int hovered = -1;
    for (int i = 0; i < int(boxes_.size()); ++i) {
        const ImVec2 a = toScreen(boxes_[size_t(i)].pos);
        const ImVec2 b(a.x + boxes_[size_t(i)].size.x * scale, a.y + boxes_[size_t(i)].size.y * scale);
        if (overCanvas && !panning_ && io.MousePos.x >= a.x && io.MousePos.x < b.x && io.MousePos.y >= a.y && io.MousePos.y < b.y)
            hovered = i;
    }

    // Arrows below the boxes.
    for (const Arrow& arrow : arrows_) {
        const Box& from = boxes_[size_t(arrow.from)];
        const Box& to = boxes_[size_t(arrow.to)];
        const bool lit = hovered == arrow.from || hovered == arrow.to;
        const ImU32 color = lit ? palette_.accent : withAlpha(palette_.textFaint, 0.75f);
        const float thickness = std::min(3.5f, 1.0f + 0.35f * float(arrow.weight)) * s_ * zoom_;
        ImVec2 a, b, c1, c2;
        if (flow_) {
            const bool sameColumn = std::fabs(from.pos.x - to.pos.x) < 1.0f;
            a = toScreen(sameColumn ? ImVec2(from.pos.x + from.size.x * 0.5f, from.pos.y + from.size.y)
                                    : ImVec2(from.pos.x + from.size.x, from.pos.y + from.size.y * 0.5f));
            b = toScreen(sameColumn ? ImVec2(to.pos.x + to.size.x * 0.5f, to.pos.y) : ImVec2(to.pos.x, to.pos.y + to.size.y * 0.5f));
            c1 = sameColumn ? ImVec2(a.x, a.y + (b.y - a.y) * 0.5f) : ImVec2(a.x + 40.0f * scale, a.y);
            c2 = sameColumn ? ImVec2(b.x, b.y - (b.y - a.y) * 0.5f) : ImVec2(b.x - 40.0f * scale, b.y);
        } else if (std::fabs(to.pos.y - from.pos.y) < 1.0f) {
            // Same line: an arc over the top.
            a = toScreen(ImVec2(from.pos.x + from.size.x * 0.5f, from.pos.y));
            b = toScreen(ImVec2(to.pos.x + to.size.x * 0.5f, to.pos.y));
            const float lift = std::min(60.0f, 18.0f + std::fabs(to.pos.x - from.pos.x) * 0.12f) * scale;
            c1 = ImVec2(a.x, a.y - lift);
            c2 = ImVec2(b.x, b.y - lift);
        } else {
            const bool down = to.pos.y > from.pos.y + 1.0f;
            a = toScreen(down ? ImVec2(from.pos.x + from.size.x * 0.5f, from.pos.y + from.size.y)
                              : ImVec2(from.pos.x + from.size.x * 0.5f, from.pos.y));
            b = toScreen(down ? ImVec2(to.pos.x + to.size.x * 0.5f, to.pos.y) : ImVec2(to.pos.x + to.size.x * 0.5f, to.pos.y + to.size.y));
            const float bend = (b.y - a.y) * 0.5f;
            c1 = ImVec2(a.x, a.y + bend);
            c2 = ImVec2(b.x, b.y - bend);
        }
        dl->AddBezierCubic(a, c1, c2, b, color, thickness);
        // Arrowhead along the final tangent.
        ImVec2 dir(b.x - c2.x, b.y - c2.y);
        const float len = std::max(0.001f, std::sqrt(dir.x * dir.x + dir.y * dir.y));
        dir = ImVec2(dir.x / len, dir.y / len);
        const float head = 7.0f * s_ * zoom_;
        const ImVec2 left(b.x - dir.x * head - dir.y * head * 0.6f, b.y - dir.y * head + dir.x * head * 0.6f);
        const ImVec2 right(b.x - dir.x * head + dir.y * head * 0.6f, b.y - dir.y * head - dir.x * head * 0.6f);
        dl->AddTriangleFilled(b, left, right, color);
        if (arrow.weight > 1 && lit) {
            const std::string label = (arrow.label.empty() ? std::string("uses") : arrow.label) + " \xC3\x97" + std::to_string(arrow.weight);
            const ImVec2 mid(0.125f * a.x + 0.375f * c1.x + 0.375f * c2.x + 0.125f * b.x,
                             0.125f * a.y + 0.375f * c1.y + 0.375f * c2.y + 0.125f * b.y);
            const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
            dl->AddRectFilled(ImVec2(mid.x - ts.x * 0.5f - 4.0f, mid.y - ts.y * 0.5f - 1.0f),
                              ImVec2(mid.x + ts.x * 0.5f + 4.0f, mid.y + ts.y * 0.5f + 1.0f), palette_.panelHeaderBg, 4.0f);
            dl->AddText(ImVec2(mid.x - ts.x * 0.5f, mid.y - ts.y * 0.5f), palette_.accent, label.c_str());
        }
    }

    // Boxes.
    const model::LogicalMap& map = logic.map();
    for (int i = 0; i < int(boxes_.size()); ++i) {
        const Box& box = boxes_[size_t(i)];
        const ImVec2 a = toScreen(box.pos);
        const ImVec2 b(a.x + box.size.x * scale, a.y + box.size.y * scale);
        const LogicalNode* n = map.find(box.id);
        UiAutomation::instance().add({"map-node:" + box.id, "region", n ? n->name : box.id, a, b, true,
                                      [&logic, id = box.id, hasChildren = n && !n->children.empty()]() {
                                          if (hasChildren)
                                              logic.zoomIn(id);
                                          else
                                              logic.open(id);
                                      }});
        drawBox(box, a, i == hovered, 0.25f + 0.75f * e);
    }
    dl->PopClipRect();

    // Clicks on a box (released without dragging): zoom in, or open the code at the bottom level.
    if (hovered >= 0 && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !panning_ &&
        ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).x * ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).x +
                ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).y * ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).y < 16.0f) {
        const LogicalNode* n = map.find(boxes_[size_t(hovered)].id);
        if (n && (!n->children.empty() || !n->target.empty()))
            logic.zoomIn(n->id);
        else if (n)
            logic.open(n->id);
    }
    if (hovered >= 0) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (const LogicalNode* n = map.find(boxes_[size_t(hovered)].id); n && ImGui::BeginTooltip()) {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
            ImGui::TextDisabled("%s", model::logicalKindName(n->kind));
            ImGui::SameLine();
            ImGui::TextUnformatted(n->name.c_str());
            if (!n->summary.empty())
                ImGui::TextWrapped("%s", n->summary.c_str());
            for (const std::string& r : n->responsibilities)
                ImGui::BulletText("%s", r.c_str());
            for (size_t k = 0; k < n->code.size() && k < 4; ++k)
                ImGui::TextDisabled(ICON_FA_FILE_CODE " %s%s", n->code[k].path.c_str(),
                                    n->code[k].startLine >= 0 ? (":" + std::to_string(n->code[k].startLine + 1)).c_str() : "");
            ImGui::TextDisabled(!n->children.empty() ? "Click to zoom in." : n->target.empty() ? "Click to open the code." : "Click to see the component.");
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }
    if (boxes_.empty()) {
        const char* text = logic.lens.get() == MapLens::Features
                               ? "No features yet: use Dissect to let the agent find the program's features and flows."
                               : "Nothing to show at this level.";
        const ImVec2 ts = ImGui::CalcTextSize(text);
        dl->AddText(ImVec2(p.x + (w - ts.x) * 0.5f, canvasMin.y + canvasSize.y * 0.4f), palette_.textFaint, text);
    }
    (void)canvasHovered;
    ImGui::EndChild();
    ImGui::PopFont();
}

void MapView::drawBox(const Box& box, ImVec2 a, bool hovered, float alpha)
{
    viewmodel::LogicalMapViewModel& logic = vm_.logicalMap();
    const LogicalNode* n = logic.map().find(box.id);
    if (!n)
        return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float scale = zoom_ * s_;
    const ImVec2 b(a.x + box.size.x * scale, a.y + box.size.y * scale);
    const ImU32 kind = n->kind == LogicalKind::Module      ? palette_.iconPurple
                       : n->kind == LogicalKind::Component ? palette_.iconTeal
                       : n->kind == LogicalKind::Function  ? palette_.iconBlue
                       : n->kind == LogicalKind::Feature   ? palette_.iconGreen
                                                           : palette_.modeAgent;
    const bool highlighted = n->id == logic.highlight();
    const float r = 9.0f * scale;
    dl->AddRectFilled(ImVec2(a.x + 3.0f * scale, a.y + 4.0f * scale), ImVec2(b.x + 3.0f * scale, b.y + 4.0f * scale),
                      withAlpha(IM_COL32(0, 0, 0, 255), 0.18f * alpha), r);
    dl->AddRectFilled(a, b, withAlpha(hovered ? palette_.tabActiveBg : palette_.panelHeaderBg, alpha), r);
    dl->AddRectFilled(a, ImVec2(b.x, a.y + 4.0f * scale), withAlpha(kind, alpha), r, ImDrawFlags_RoundTop);
    dl->AddRect(a, b, withAlpha(highlighted ? palette_.accent : hovered ? kind : palette_.border, alpha), r,
                (highlighted || hovered ? 2.0f : 1.0f) * scale, 0);
    if (highlighted) {
        const char* here = ICON_FA_LOCATION_DOT " you are here";
        ImGui::PushFont(nullptr, Fonts::kUiSize * 0.7f * zoom_);
        dl->AddText(ImVec2(a.x, a.y - ImGui::GetFontSize() - 3.0f * scale), withAlpha(palette_.accent, alpha), here);
        ImGui::PopFont();
    }

    // Contents scale with the zoom, so the map stays legible as a picture.
    const float pad = 10.0f * scale;
    ImGui::PushFont(nullptr, Fonts::kUiSize * 0.95f * zoom_);
    const float titleH = ImGui::GetFontSize();
    const char* icon = kindIcon(n->kind);
    dl->AddText(ImVec2(a.x + pad, a.y + pad + 2.0f * scale), withAlpha(kind, alpha), icon);
    const float textX = a.x + pad + ImGui::CalcTextSize(icon).x + 6.0f * scale;
    std::string meta;
    if (!n->children.empty())
        meta = std::to_string(n->children.size()) + (n->kind == LogicalKind::Function || n->kind == LogicalKind::Feature ? " steps" : " parts");
    else if (!n->code.empty() && n->code.front().startLine >= 0)
        meta = "L" + std::to_string(n->code.front().startLine + 1);
    ImGui::PushFont(nullptr, Fonts::kUiSize * 0.75f * zoom_);
    const float metaW = ImGui::CalcTextSize(meta.c_str()).x;
    dl->AddText(ImVec2(b.x - pad - metaW, a.y + pad + 2.0f * scale + (titleH - ImGui::GetFontSize()) * 0.5f),
                withAlpha(palette_.textFaint, alpha), meta.c_str());
    ImGui::PopFont();
    dl->AddText(ImVec2(textX, a.y + pad + 2.0f * scale), withAlpha(palette_.text, alpha),
                ellipsize(n->name, b.x - pad - metaW - 8.0f * scale - textX).c_str());
    ImGui::PopFont();

    ImGui::PushFont(nullptr, Fonts::kUiSize * 0.8f * zoom_);
    const float lineH = ImGui::GetFontSize() * 1.15f;
    const float textTop = a.y + pad + 2.0f * scale + titleH + 6.0f * scale;
    const int maxLines = std::max(1, int((b.y - pad - textTop) / lineH));
    float y = textTop;
    const std::string summary = n->summary.empty() ? std::string(model::logicalKindName(n->kind)) : n->summary;
    for (const std::string& line : wrapLines(summary, b.x - pad - (a.x + pad), maxLines)) {
        dl->AddText(ImVec2(a.x + pad, y), withAlpha(n->summary.empty() ? palette_.textFaint : palette_.textDim, alpha), line.c_str());
        y += lineH;
    }
    ImGui::PopFont();
    // Zoom affordance.
    if (!n->children.empty() && hovered) {
        ImGui::PushFont(nullptr, Fonts::kUiSize * 0.8f * zoom_);
        dl->AddText(ImVec2(b.x - pad - ImGui::CalcTextSize(ICON_FA_MAGNIFYING_GLASS_PLUS).x, b.y - pad - ImGui::GetFontSize()),
                    withAlpha(palette_.accent, alpha), ICON_FA_MAGNIFYING_GLASS_PLUS);
        ImGui::PopFont();
    }
}

// ---- Header -------------------------------------------------------------------------------------

void MapView::renderHeader(float width)
{
    viewmodel::LogicalMapViewModel& logic = vm_.logicalMap();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetWindowPos();
    const float h = kHeader * s_;
    panel::fill(dl, p, ImVec2(p.x + width, p.y + h), palette_.panelBg);
    dl->AddLine(ImVec2(p.x, p.y + h - 1.0f), ImVec2(p.x + width, p.y + h - 1.0f), palette_.border);
    const float margin = 8.0f * s_;
    const float pillH = h - 10.0f * s_;
    const float y = p.y + 5.0f * s_;
    float x = p.x + margin;

    // Back to the code.
    {
        const char* label = ICON_FA_CODE "  Code";
        const float tw = ImGui::CalcTextSize(label).x + 18.0f * s_;
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        if (ImGui::InvisibleButton("##toCode", ImVec2(tw, pillH)))
            vm_.execute(viewmodel::CommandId::ShowCodeView);
        const bool hovered = ImGui::IsItemHovered();
        ImGui::SetItemTooltip("Back to the code (the map stays where you left it)");
        registerLastItem("map-to-code", "button", "Code", [this]() { vm_.execute(viewmodel::CommandId::ShowCodeView); });
        dl->AddRect(ImVec2(x, y), ImVec2(x + tw, y + pillH), hovered ? palette_.accent : palette_.border, 5.0f * s_);
        const ImVec2 ts = ImGui::CalcTextSize(label);
        dl->AddText(ImVec2(std::floor(x + 9.0f * s_), std::floor(y + (pillH - ts.y) * 0.5f)), hovered ? palette_.text : palette_.textDim, label);
        x += tw + 10.0f * s_;
    }

    // Lens switch.
    const bool features = logic.hasFeatures();
    struct Lens {
        const char* label;
        MapLens lens;
    };
    const Lens lenses[] = {{ICON_FA_LAYER_GROUP "  Architecture", MapLens::Architecture}, {ICON_FA_ROUTE "  Features", MapLens::Features}};
    const float trackStart = x;
    float trackW = 4.0f * s_;
    for (const Lens& l : lenses)
        trackW += ImGui::CalcTextSize(l.label).x + 18.0f * s_;
    dl->AddRectFilled(ImVec2(trackStart, y), ImVec2(trackStart + trackW, y + pillH), palette_.editorBg, 6.0f * s_);
    x += 2.0f * s_;
    for (const Lens& l : lenses) {
        const float tw = ImGui::CalcTextSize(l.label).x + 18.0f * s_;
        const ImVec2 a(x, y + 2.0f * s_), b(x + tw, y + pillH - 2.0f * s_);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(int(l.lens));
        if (ImGui::InvisibleButton("##lens", ImVec2(tw, b.y - a.y)))
            logic.lens.set(l.lens);
        ImGui::PopID();
        const bool active = logic.lens.get() == l.lens;
        const bool hovered = ImGui::IsItemHovered();
        if (l.lens == MapLens::Features && !features)
            ImGui::SetItemTooltip("No features yet: Dissect asks the agent to find them");
        registerLastItem(std::string("map-lens:") + (l.lens == MapLens::Features ? "Features" : "Architecture"), "tab", l.label,
                         [&logic, lens = l.lens]() { logic.lens.set(lens); });
        if (active)
            dl->AddRectFilled(a, b, palette_.tabActiveBg, 5.0f * s_);
        else if (hovered)
            dl->AddRectFilled(a, b, palette_.hover, 5.0f * s_);
        const ImVec2 ts = ImGui::CalcTextSize(l.label);
        dl->AddText(ImVec2(std::floor(a.x + 9.0f * s_), std::floor(a.y + (b.y - a.y - ts.y) * 0.5f)),
                    active || hovered ? palette_.text : (l.lens == MapLens::Features && !features ? palette_.textFaint : palette_.textDim),
                    l.label);
        x += tw;
    }
    x = trackStart + trackW + 12.0f * s_;

    // Back + breadcrumb.
    const std::vector<std::string> crumbs = logic.breadcrumb();
    const float backW = pillH;
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    const bool canBack = crumbs.size() > 1;
    if (ImGui::InvisibleButton("##mapBack", ImVec2(backW, pillH)) && canBack)
        logic.zoomOut();
    if (canBack && ImGui::IsItemHovered())
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + backW, y + pillH), palette_.hover, 5.0f * s_);
    registerLastItem("map-zoom-out", "button", "Zoom out", [&logic]() { logic.zoomOut(); }, canBack);
    const ImVec2 as = ImGui::CalcTextSize(ICON_FA_ARROW_LEFT);
    dl->AddText(ImVec2(std::floor(x + (backW - as.x) * 0.5f), std::floor(y + (pillH - as.y) * 0.5f)),
                canBack ? palette_.text : palette_.textFaint, ICON_FA_ARROW_LEFT);
    x += backW + 6.0f * s_;

    // Right side first (to know how much room the breadcrumb has): source badge, dissect, fit.
    const bool busy = logic.analyzing.get();
    const char* dissect = busy ? ICON_FA_SPINNER "  Dissecting..." : ICON_FA_BRAIN "  Dissect";
    const char* fitLabel = ICON_FA_EXPAND;
    const std::string source = std::string(logic.map().source == "agent" ? ICON_FA_WAND_MAGIC_SPARKLES " " : ICON_FA_MICROCHIP " ") +
                               logic.sourceName();
    const float dissectW = ImGui::CalcTextSize(dissect).x + 18.0f * s_;
    const float fitW = pillH;
    const float sourceW = ImGui::CalcTextSize(source.c_str()).x;
    float rx = p.x + width - margin - fitW;
    ImGui::SetCursorScreenPos(ImVec2(rx, y));
    if (ImGui::InvisibleButton("##mapFit", ImVec2(fitW, pillH)))
        needsFit_ = true;
    ImGui::SetItemTooltip("Fit to view");
    if (ImGui::IsItemHovered())
        dl->AddRectFilled(ImVec2(rx, y), ImVec2(rx + fitW, y + pillH), palette_.hover, 5.0f * s_);
    const ImVec2 fs = ImGui::CalcTextSize(fitLabel);
    dl->AddText(ImVec2(std::floor(rx + (fitW - fs.x) * 0.5f), std::floor(y + (pillH - fs.y) * 0.5f)), palette_.textDim, fitLabel);
    rx -= dissectW + 6.0f * s_;
    ImGui::SetCursorScreenPos(ImVec2(rx, y));
    if (ImGui::InvisibleButton("##dissect", ImVec2(dissectW, pillH)) && !busy)
        vm_.execute(viewmodel::CommandId::DissectProject);
    const bool dissectHovered = ImGui::IsItemHovered();
    ImGui::SetItemTooltip("Ask the agent (%s) to dissect the project into features and flows, and refine every summary",
                          logic.agentCommand().c_str());
    registerLastItem("command:DissectProject", "button", "Dissect", [this]() { vm_.execute(viewmodel::CommandId::DissectProject); }, !busy);
    dl->AddRectFilled(ImVec2(rx, y), ImVec2(rx + dissectW, y + pillH),
                      withAlpha(palette_.modeAgent, busy ? 0.25f : dissectHovered ? 0.45f : 0.3f), 5.0f * s_);
    const ImVec2 ds = ImGui::CalcTextSize(dissect);
    dl->AddText(ImVec2(std::floor(rx + 9.0f * s_), std::floor(y + (pillH - ds.y) * 0.5f)), palette_.text, dissect);
    rx -= sourceW + 12.0f * s_;
    dl->AddText(ImVec2(rx, std::floor(y + (pillH - ImGui::GetTextLineHeight()) * 0.5f)), palette_.textFaint, source.c_str());

    // Breadcrumb in the space left.
    const model::LogicalMap& map = logic.map();
    const float right = rx - 12.0f * s_;
    const char* sep = "  " ICON_FA_CHEVRON_RIGHT "  ";
    dl->PushClipRect(ImVec2(x, p.y), ImVec2(right, p.y + h), true);
    size_t first = 0;
    auto total = [&](size_t from) {
        float t = 0.0f;
        for (size_t k = from; k < crumbs.size(); ++k)
            if (const LogicalNode* n = map.find(crumbs[k]))
                t += ImGui::CalcTextSize(n->name.c_str()).x + ImGui::CalcTextSize(sep).x;
        return t;
    };
    while (first + 1 < crumbs.size() && total(first) > right - x)
        ++first;
    for (size_t k = first; k < crumbs.size(); ++k) {
        const LogicalNode* n = map.find(crumbs[k]);
        if (!n)
            continue;
        if (k > first) {
            ImGui::PushFont(nullptr, Fonts::kUiSize * 0.65f);
            dl->AddText(ImVec2(x, std::floor(y + (pillH - ImGui::GetFontSize()) * 0.5f)), palette_.textFaint, sep);
            x += ImGui::CalcTextSize(sep).x;
            ImGui::PopFont();
        }
        const bool last = k + 1 == crumbs.size();
        const float tw = ImGui::CalcTextSize(n->name.c_str()).x;
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        ImGui::PushID(int(k));
        if (ImGui::InvisibleButton("##crumb", ImVec2(std::max(1.0f, tw), pillH)) && !last)
            logic.zoomTo(n->id);
        ImGui::PopID();
        const bool hovered = ImGui::IsItemHovered() && !last;
        registerLastItem("map-crumb:" + n->id, "button", n->name, [&logic, id = n->id]() { logic.zoomTo(id); });
        dl->AddText(ImVec2(x, std::floor(y + (pillH - ImGui::GetTextLineHeight()) * 0.5f)),
                    last ? palette_.text : hovered ? palette_.accent : palette_.textDim, n->name.c_str());
        x += tw;
    }
    dl->PopClipRect();
}

} // namespace enp::view
