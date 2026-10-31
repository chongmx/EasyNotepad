#include "enp/view/LayersView.h"

#include "enp/view/UiAutomation.h"

#include "IconsFontAwesome6.h"

#include <algorithm>
#include <cmath>

namespace enp::view {

using viewmodel::LayerDensity;
using viewmodel::LayerKind;
using viewmodel::LayerNode;
using viewmodel::LayerScope;

namespace {

// Layout metrics (unscaled). One margin everywhere keeps the edges of every row aligned.
constexpr float kMargin = 8.0f;
constexpr float kControlRow = 30.0f;
constexpr float kCrumbRow = 26.0f;
constexpr float kCardPad = 8.0f;
constexpr float kCardGap = 6.0f;
constexpr float kIconColumn = 18.0f;
constexpr float kChevronColumn = 20.0f;
constexpr float kTitleSize = 0.92f;   // relative to Fonts::kUiSize
constexpr float kTextSize = 0.84f;
constexpr float kSmallSize = 0.76f;
constexpr double kAnimSeconds = 0.18;

ImU32 alphaOf(ImU32 c, int a)
{
    return (c & ~IM_COL32_A_MASK) | (ImU32(a) << IM_COL32_A_SHIFT);
}

// Text cut to `width` with an ellipsis, in the current font. The code font has no "…" glyph, so
// callers drawing monospace text pass `ascii`.
std::string ellipsize(const std::string& text, float width, bool ascii = false)
{
    if (ImGui::CalcTextSize(text.c_str()).x <= width)
        return text;
    const char* ellipsis = ascii ? "..." : "\xE2\x80\xA6";
    const float ew = ImGui::CalcTextSize(ellipsis).x;
    std::string out = text;
    while (!out.empty() && ImGui::CalcTextSize(out.c_str()).x + ew > width) {
        out.pop_back();
        while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80) // whole code points
            out.pop_back();
    }
    return out + ellipsis;
}

// Word-wrapped text limited to `maxLines` lines (the last one ellipsized), in the current font.
std::vector<std::string> wrapLines(const std::string& text, float width, int maxLines)
{
    std::vector<std::string> lines;
    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize();
    const char* p = text.c_str();
    const char* end = p + text.size();
    while (p < end && int(lines.size()) < maxLines) {
        const char* wrap = font->CalcWordWrapPosition(size, p, end, width);
        if (wrap == p) // a single word wider than the line
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

float easeOut(float t)
{
    t = 1.0f - t;
    return 1.0f - t * t * t;
}

} // namespace

LayersView::LayersView(viewmodel::NotepadViewModel& vm, const Fonts& fonts, const Palette& palette, float uiScale)
    : vm_(vm), fonts_(fonts), palette_(palette), s_(uiScale)
{
}

const char* LayersView::kindIcon(LayerKind kind)
{
    switch (kind) {
    case LayerKind::Project: return ICON_FA_DIAGRAM_PROJECT;
    case LayerKind::Folder: return ICON_FA_FOLDER;
    case LayerKind::File: return ICON_FA_FILE_CODE;
    case LayerKind::Namespace: return ICON_FA_CUBES;
    case LayerKind::Type: return ICON_FA_CUBE;
    case LayerKind::Function: return ICON_FA_CODE;
    case LayerKind::Block: return ICON_FA_LIST_UL;
    case LayerKind::Section: return ICON_FA_HASHTAG;
    case LayerKind::Step: return ICON_FA_SHOE_PRINTS;
    case LayerKind::Module: return ICON_FA_LAYER_GROUP;
    case LayerKind::Component: return ICON_FA_PUZZLE_PIECE;
    case LayerKind::Feature: return ICON_FA_ROUTE;
    }
    return ICON_FA_CIRCLE;
}

ImU32 LayersView::kindColor(LayerKind kind) const
{
    switch (kind) {
    case LayerKind::Project: return palette_.modeAgent;
    case LayerKind::Folder: return palette_.iconOrange;
    case LayerKind::File: return palette_.iconBlue;
    case LayerKind::Namespace: return palette_.iconGray;
    case LayerKind::Type: return palette_.iconOrange;
    case LayerKind::Function: return palette_.iconBlue;
    case LayerKind::Block: return palette_.iconTeal;
    case LayerKind::Section: return palette_.iconPurple;
    case LayerKind::Step: return palette_.modeAgent;
    case LayerKind::Module: return palette_.iconPurple;
    case LayerKind::Component: return palette_.iconTeal;
    case LayerKind::Feature: return palette_.iconGreen;
    }
    return palette_.iconGray;
}

std::string LayersView::meta(const LayerNode& node) const
{
    if (node.startLine >= 0)
        return "L" + std::to_string(node.startLine + 1) + "\xE2\x80\x93" + std::to_string(node.endLine + 1);
    if (node.kind == LayerKind::File)
        return node.size > 0 ? std::to_string(node.size) + " lines" : std::string();
    if (node.kind == LayerKind::Module || node.kind == LayerKind::Component || node.kind == LayerKind::Feature ||
        (node.kind == LayerKind::Project && vm_.layers().scope.get() == LayerScope::Logic))
        return std::to_string(node.size) + (node.size == 1 ? " part" : " parts");
    return std::to_string(node.size) + (node.size == 1 ? " file" : " files");
}

void LayersView::segmented(const char* id, const char* const* labels, const char* const* tooltips, int count, int selected,
                           ImVec2 pos, float height, float& width, const std::function<void(int)>& onSelect)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float inner = 2.0f * s_;
    std::vector<float> widths(static_cast<size_t>(count), 0.0f);
    width = inner;
    for (int i = 0; i < count; ++i) {
        widths[size_t(i)] = ImGui::CalcTextSize(labels[i]).x + 16.0f * s_;
        width += widths[size_t(i)];
    }
    width += inner;
    dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), palette_.editorBg, 6.0f * s_);
    int clicked = -1;
    float x = pos.x + inner;
    ImGui::PushID(id);
    for (int i = 0; i < count; ++i) {
        const ImVec2 a(x, pos.y + inner), b(x + widths[size_t(i)], pos.y + height - inner);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##segment", ImVec2(b.x - a.x, b.y - a.y)))
            clicked = i;
        ImGui::PopID();
        const bool hovered = ImGui::IsItemHovered();
        if (i == selected)
            dl->AddRectFilled(a, b, palette_.tabActiveBg, 5.0f * s_);
        else if (hovered)
            dl->AddRectFilled(a, b, palette_.hover, 5.0f * s_);
        const ImVec2 ts = ImGui::CalcTextSize(labels[i]);
        dl->AddText(ImVec2(std::floor(a.x + (b.x - a.x - ts.x) * 0.5f), std::floor(a.y + (b.y - a.y - ts.y) * 0.5f)),
                    i == selected || hovered ? palette_.text : palette_.textDim, labels[i]);
        if (tooltips)
            ImGui::SetItemTooltip("%s", tooltips[i]);
        registerLastItem(std::string(id) + ":" + (tooltips ? tooltips[i] : labels[i]), "button", tooltips ? tooltips[i] : labels[i],
                         [onSelect, i]() { onSelect(i); });
        x += widths[size_t(i)];
    }
    ImGui::PopID();
    if (clicked >= 0)
        onSelect(clicked);
}

// ---- Frame ----------------------------------------------------------------------------------------

void LayersView::render(ImVec2 size)
{
    ImGui::PushFont(fonts_.ui, Fonts::kUiSize * kTextSize);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    renderControls(size.x);
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + kControlRow * s_));
    renderBreadcrumb(size.x);
    const float top = (kControlRow + kCrumbRow) * s_;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(origin.x, origin.y + top - 1.0f), ImVec2(origin.x + size.x, origin.y + top - 1.0f),
                                        palette_.border);
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + top));
    renderCards(ImVec2(size.x, std::max(1.0f, size.y - top)));
    ImGui::PopFont();
}

void LayersView::renderControls(float width)
{
    viewmodel::LayersViewModel& layers = vm_.layers();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = kControlRow * s_ - 8.0f * s_;
    const float y = p.y + 5.0f * s_;

    static const char* const scopeLabels[] = {ICON_FA_FILE_CODE " File", ICON_FA_FOLDER_TREE " Project", ICON_FA_BRAIN " Logic"};
    static const char* const scopeTips[] = {"File", "Project", "Logic"};
    float scopeW = 0.0f;
    segmented("layers-scope", scopeLabels, scopeTips, 3, int(layers.scope.get()), ImVec2(p.x + kMargin * s_, y), h, scopeW,
              [&layers](int i) { layers.scope.set(LayerScope(i)); });

    static const char* const densityLabels[] = {ICON_FA_GRIP_LINES, ICON_FA_ALIGN_LEFT, ICON_FA_TABLE_LIST};
    static const char* const densityTips[] = {"Compact", "Summary", "Detailed"};
    // Measure first to right-align.
    float densityW = 4.0f * s_;
    for (const char* label : densityLabels)
        densityW += ImGui::CalcTextSize(label).x + 16.0f * s_;
    const float x = std::max(p.x + kMargin * s_ + scopeW + 6.0f * s_, p.x + width - kMargin * s_ - densityW);
    segmented("layers-density", densityLabels, densityTips, 3, int(layers.density.get()), ImVec2(x, y), h, densityW,
              [&layers](int i) { layers.density.set(LayerDensity(i)); });
}

void LayersView::renderBreadcrumb(float width)
{
    viewmodel::LayersViewModel& layers = vm_.layers();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float rowH = kCrumbRow * s_;
    const std::vector<int> crumbs = layers.breadcrumb();
    const auto& nodes = layers.nodes();

    // Back button, aligned with the margin.
    const float backW = rowH - 6.0f * s_;
    const ImVec2 b0(p.x + kMargin * s_, p.y + 2.0f * s_);
    ImGui::SetCursorScreenPos(b0);
    const bool canBack = crumbs.size() > 1;
    if (ImGui::InvisibleButton("##layersBack", ImVec2(backW, backW)) && canBack)
        layers.zoomOut();
    if (canBack && ImGui::IsItemHovered())
        dl->AddRectFilled(b0, ImVec2(b0.x + backW, b0.y + backW), palette_.hover, 5.0f * s_);
    ImGui::SetItemTooltip("Zoom out (mouse back)");
    registerLastItem("layers-zoom-out", "button", "Zoom out", [&layers]() { layers.zoomOut(); }, canBack);
    const ImVec2 arrow = ImGui::CalcTextSize(ICON_FA_ARROW_LEFT);
    dl->AddText(ImVec2(std::floor(b0.x + (backW - arrow.x) * 0.5f), std::floor(b0.y + (backW - arrow.y) * 0.5f)),
                canBack ? palette_.text : palette_.textFaint, ICON_FA_ARROW_LEFT);

    if (crumbs.empty())
        return;
    // Fit from the end: the deepest layers matter most; earlier ones collapse into "...".
    const char* separator = " " ICON_FA_CHEVRON_RIGHT " ";
    ImGui::PushFont(nullptr, Fonts::kUiSize * kSmallSize);
    const float sepW = ImGui::CalcTextSize(separator).x;
    ImGui::PopFont();
    const float left = b0.x + backW + 6.0f * s_;
    const float avail = p.x + width - kMargin * s_ - left;
    std::vector<float> widths;
    for (int i : crumbs)
        widths.push_back(ImGui::CalcTextSize(nodes[size_t(i)].title.c_str()).x);
    size_t first = 0;
    const float ellipsisW = ImGui::CalcTextSize("\xE2\x80\xA6").x + sepW;
    auto total = [&](size_t from) {
        float w = from > 0 ? ellipsisW : 0.0f;
        for (size_t k = from; k < widths.size(); ++k)
            w += widths[k] + (k + 1 < widths.size() ? sepW : 0.0f);
        return w;
    };
    while (first + 1 < crumbs.size() && total(first) > avail)
        ++first;

    float x = left;
    const float textY = std::floor(p.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f);
    dl->PushClipRect(ImVec2(left, p.y), ImVec2(p.x + width - kMargin * s_, p.y + rowH), true);
    if (first > 0) {
        // The collapsed levels: clicking zooms out to the deepest hidden one.
        const float ew = ImGui::CalcTextSize("\xE2\x80\xA6").x;
        ImGui::SetCursorScreenPos(ImVec2(x, p.y + 3.0f * s_));
        const int hidden = crumbs[first - 1];
        if (ImGui::InvisibleButton("##crumbEllipsis", ImVec2(ew, rowH - 6.0f * s_)))
            layers.zoomTo(hidden);
        ImGui::SetItemTooltip("%s", nodes[size_t(hidden)].title.c_str());
        dl->AddText(ImVec2(x, textY), ImGui::IsItemHovered() ? palette_.accent : palette_.textFaint, "\xE2\x80\xA6");
        x += ew;
    }
    for (size_t k = first; k < crumbs.size(); ++k) {
        if (k > first || first > 0) {
            ImGui::PushFont(nullptr, Fonts::kUiSize * kSmallSize);
            dl->AddText(ImVec2(x, textY + 1.0f * s_), palette_.textFaint, separator);
            ImGui::PopFont();
            x += sepW;
        }
        const LayerNode& n = nodes[size_t(crumbs[k])];
        const bool last = k + 1 == crumbs.size();
        ImGui::SetCursorScreenPos(ImVec2(x, p.y + 3.0f * s_));
        ImGui::PushID(int(k));
        const int index = crumbs[k];
        if (ImGui::InvisibleButton("##crumb", ImVec2(std::max(1.0f, widths[k]), rowH - 6.0f * s_)) && !last)
            layers.zoomTo(index);
        ImGui::PopID();
        const bool hovered = ImGui::IsItemHovered() && !last;
        registerLastItem("layer-crumb:" + n.id, "button", n.title, [&layers, index]() { layers.zoomTo(index); });
        dl->AddText(ImVec2(x, textY), last ? palette_.text : hovered ? palette_.accent : palette_.textDim, n.title.c_str());
        x += widths[k];
    }
    dl->PopClipRect();
}

// ---- Cards ----------------------------------------------------------------------------------------

float LayersView::aboutCard(const LayerNode& node, float x, float y, float width, bool draw)
{
    // The zoomed-in layer itself: icon, title, meta, and its full summary.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float pad = kCardPad * s_;
    ImGui::PushFont(nullptr, Fonts::kUiSize * kTitleSize);
    const float titleH = ImGui::GetTextLineHeight();
    const std::string metaText = meta(node);
    ImGui::PushFont(nullptr, Fonts::kUiSize * kSmallSize);
    const float metaW = ImGui::CalcTextSize(metaText.c_str()).x;
    ImGui::PopFont();
    const float textX = x + pad + kIconColumn * s_;
    const std::string title = ellipsize(node.title, x + width - pad - metaW - 8.0f * s_ - textX);
    ImGui::PopFont();
    const float wrapW = x + width - pad - textX;
    const std::vector<std::string> summary = wrapLines(node.summary, wrapW, 6);
    const float lineH = ImGui::GetTextLineHeight();
    const float h = pad + titleH + (summary.empty() ? 0.0f : 4.0f * s_ + float(summary.size()) * lineH) + pad;
    if (!draw)
        return h;

    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + width, y + h), alphaOf(kindColor(node.kind), 22), 7.0f * s_);
    ImGui::PushFont(nullptr, Fonts::kUiSize * kTitleSize);
    dl->AddText(ImVec2(x + pad, y + pad), ImGui::GetColorU32(kindColor(node.kind)), kindIcon(node.kind));
    dl->AddText(ImVec2(textX, y + pad), ImGui::GetColorU32(palette_.text), title.c_str());
    ImGui::PopFont();
    ImGui::PushFont(nullptr, Fonts::kUiSize * kSmallSize);
    const float metaY = y + pad + (titleH - ImGui::GetTextLineHeight()) * 0.5f;
    dl->AddText(ImVec2(x + width - pad - metaW, metaY), ImGui::GetColorU32(palette_.textFaint), metaText.c_str());
    ImGui::PopFont();
    float ty = y + pad + titleH + 4.0f * s_;
    for (const std::string& line : summary) {
        dl->AddText(ImVec2(textX, ty), ImGui::GetColorU32(palette_.textDim), line.c_str());
        ty += lineH;
    }
    return h;
}

float LayersView::card(int index, int maxSize, bool current, float x, float y, float width, bool draw)
{
    viewmodel::LayersViewModel& layers = vm_.layers();
    const LayerNode& node = layers.nodes()[size_t(index)];
    const LayerDensity density = layers.density.get();
    const bool hasChildren = !node.children.empty() || node.expandable;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float pad = kCardPad * s_;
    const float textX = x + pad + kIconColumn * s_;
    const float chevronX = x + width - pad - kChevronColumn * s_;
    const float right = hasChildren ? chevronX - 4.0f * s_ : x + width - pad;

    ImGui::PushFont(nullptr, Fonts::kUiSize * kTitleSize);
    const float titleH = ImGui::GetTextLineHeight();
    ImGui::PopFont();
    const std::string metaText = meta(node);
    ImGui::PushFont(nullptr, Fonts::kUiSize * kSmallSize);
    const float metaW = ImGui::CalcTextSize(metaText.c_str()).x;
    const float smallH = ImGui::GetTextLineHeight();
    ImGui::PopFont();

    // Detailed: the full signature under the title, when it says more than the short name.
    const bool signature = density == LayerDensity::Detailed && !node.signature.empty() && node.signature != node.title;
    std::string signatureText;
    if (signature) {
        ImGui::PushFont(fonts_.mono, Fonts::kUiSize * kSmallSize);
        signatureText = ellipsize(node.signature, right - textX, true);
        ImGui::PopFont();
    }
    std::vector<std::string> summary;
    if (density != LayerDensity::Compact)
        summary = wrapLines(node.summary, right - textX, density == LayerDensity::Detailed ? 4 : 2);
    const float lineH = ImGui::GetTextLineHeight();
    const float chipH = smallH + 4.0f * s_;
    const bool chips = density == LayerDensity::Detailed && !node.children.empty();
    const float barH = density == LayerDensity::Compact ? 0.0f : 3.0f * s_;
    const float h = pad + titleH + (signature ? 3.0f * s_ + smallH : 0.0f) +
                    (summary.empty() ? 0.0f : 3.0f * s_ + float(summary.size()) * lineH) +
                    (chips ? 6.0f * s_ + chipH : 0.0f) + (barH > 0.0f ? 7.0f * s_ + barH : 0.0f) + pad;
    if (!draw)
        return h;

    // Interaction: the whole card opens the layer; the chevron zooms in.
    ImGui::PushID(node.id.c_str());
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::SetNextItemAllowOverlap();
    const bool clicked = ImGui::InvisibleButton("##card", ImVec2(width, h));
    const bool hovered = ImGui::IsItemHovered();
    const bool doubleClicked = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    registerLastItem("layer:" + node.id, "region", node.title, [&layers, index]() { layers.open(index); });
    if (density == LayerDensity::Compact && hovered && !node.summary.empty())
        ImGui::SetItemTooltip("%s", node.summary.c_str());
    bool zoom = false;
    bool chevronHovered = false;
    const float chevronSize = kChevronColumn * s_;
    const ImVec2 c0(chevronX, y + (h - chevronSize) * 0.5f);
    if (hasChildren) {
        ImGui::SetCursorScreenPos(c0);
        zoom = ImGui::InvisibleButton("##zoom", ImVec2(chevronSize, chevronSize));
        chevronHovered = ImGui::IsItemHovered();
        if (node.children.empty())
            ImGui::SetItemTooltip("Zoom in: load this file's parts");
        else
            ImGui::SetItemTooltip("Zoom in: %d part%s", int(node.children.size()), node.children.size() == 1 ? "" : "s");
        registerLastItem("layer-zoom:" + node.id, "button", "Zoom into " + node.title,
                         [&layers, index]() { layers.zoomIn(index); });
    }
    ImGui::PopID();

    const ImU32 kind = kindColor(node.kind);
    const ImVec2 a(x, y), b(x + width, y + h);
    dl->AddRectFilled(a, b, ImGui::GetColorU32(hovered ? palette_.tabActiveBg : palette_.panelHeaderBg), 7.0f * s_);
    if (current)
        dl->AddRect(a, b, ImGui::GetColorU32(alphaOf(palette_.accent, 170)), 7.0f * s_, 1.0f * s_);
    // Kind strip on the left edge.
    dl->AddRectFilled(ImVec2(x, y + 6.0f * s_), ImVec2(x + 3.0f * s_, b.y - 6.0f * s_), ImGui::GetColorU32(kind), 2.0f * s_);

    ImGui::PushFont(nullptr, Fonts::kUiSize * kTitleSize);
    const float iconW = ImGui::CalcTextSize(kindIcon(node.kind)).x;
    dl->AddText(ImVec2(x + pad + (kIconColumn * s_ - iconW) * 0.5f - 2.0f * s_, y + pad), ImGui::GetColorU32(kind),
                kindIcon(node.kind));
    const std::string title = ellipsize(node.title, right - metaW - 8.0f * s_ - textX);
    dl->AddText(ImVec2(textX, y + pad), ImGui::GetColorU32(palette_.text), title.c_str());
    ImGui::PopFont();
    ImGui::PushFont(nullptr, Fonts::kUiSize * kSmallSize);
    dl->AddText(ImVec2(right - metaW, y + pad + (titleH - smallH) * 0.5f), ImGui::GetColorU32(palette_.textFaint), metaText.c_str());
    ImGui::PopFont();

    float ty = y + pad + titleH + 3.0f * s_;
    if (signature) {
        ImGui::PushFont(fonts_.mono, Fonts::kUiSize * kSmallSize);
        dl->AddText(ImVec2(textX, ty), ImGui::GetColorU32(palette_.synType), signatureText.c_str());
        ImGui::PopFont();
        ty += smallH + 3.0f * s_;
    }
    for (const std::string& line : summary) {
        dl->AddText(ImVec2(textX, ty), ImGui::GetColorU32(palette_.textDim), line.c_str());
        ty += lineH;
    }
    if (chips) {
        // The first sub-layers as small chips, then "+N".
        ty += 6.0f * s_ - 3.0f * s_;
        float cx = textX;
        ImGui::PushFont(nullptr, Fonts::kUiSize * kSmallSize);
        const auto& nodes = layers.nodes();
        // Chips for the first sub-layers; whatever does not fit is counted in a final "+N" chip.
        auto chip = [&](const std::string& label) {
            const float w = ImGui::CalcTextSize(label.c_str()).x + 10.0f * s_;
            dl->AddRectFilled(ImVec2(cx, ty), ImVec2(cx + w, ty + chipH), ImGui::GetColorU32(palette_.hover), chipH * 0.5f);
            dl->AddText(ImVec2(cx + 5.0f * s_, ty + 2.0f * s_), ImGui::GetColorU32(palette_.textDim), label.c_str());
            cx += w + 4.0f * s_;
        };
        const size_t total = node.children.size();
        for (size_t k = 0; k < total; ++k) {
            const std::string label = ellipsize(nodes[size_t(node.children[k])].title, 110.0f * s_);
            const std::string rest = "+" + std::to_string(total - k);
            const float restW = k + 1 < total ? ImGui::CalcTextSize(("+" + std::to_string(total - k - 1)).c_str()).x + 14.0f * s_ : 0.0f;
            if (k == 3 || cx + ImGui::CalcTextSize(label.c_str()).x + 10.0f * s_ + restW > right) {
                chip(rest);
                break;
            }
            chip(label);
        }
        ImGui::PopFont();
        ty += chipH;
    }
    if (barH > 0.0f) {
        // Size relative to the largest sibling: a sense of scale without a to-scale layout.
        const float by = b.y - pad - barH;
        const float full = right - textX;
        const float fraction = maxSize > 0 ? std::clamp(float(node.size) / float(maxSize), 0.04f, 1.0f) : 0.0f;
        dl->AddRectFilled(ImVec2(textX, by), ImVec2(textX + full, by + barH), ImGui::GetColorU32(palette_.hover), barH);
        dl->AddRectFilled(ImVec2(textX, by), ImVec2(textX + full * fraction, by + barH),
                          ImGui::GetColorU32(alphaOf(kind, 150)), barH);
    }
    if (hasChildren) {
        if (chevronHovered)
            dl->AddRectFilled(c0, ImVec2(c0.x + chevronSize, c0.y + chevronSize), ImGui::GetColorU32(palette_.hover), 5.0f * s_);
        ImGui::PushFont(nullptr, Fonts::kUiSize * kSmallSize);
        const ImVec2 cs = ImGui::CalcTextSize(ICON_FA_CHEVRON_RIGHT);
        dl->AddText(ImVec2(std::floor(c0.x + (chevronSize - cs.x) * 0.5f), std::floor(c0.y + (chevronSize - cs.y) * 0.5f)),
                    ImGui::GetColorU32(chevronHovered ? palette_.text : palette_.textDim), ICON_FA_CHEVRON_RIGHT);
        ImGui::PopFont();
    }

    if (zoom || (doubleClicked && hasChildren))
        layers.zoomIn(index);
    else if (clicked && !doubleClicked)
        layers.open(index);
    return h;
}

void LayersView::renderCards(ImVec2 size)
{
    viewmodel::LayersViewModel& layers = vm_.layers();
    ImGuiIO& io = ImGui::GetIO();
    const bool ctrlWheel = io.KeyCtrl && ImGui::IsMouseHoveringRect(ImGui::GetCursorScreenPos(),
                                                                    ImVec2(ImGui::GetCursorScreenPos().x + size.x,
                                                                           ImGui::GetCursorScreenPos().y + size.y));
    ImGui::BeginChild("##layerCards", size, ImGuiChildFlags_None, ctrlWheel ? ImGuiWindowFlags_NoScrollWithMouse : 0);
    const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
    if (hovered && ctrlWheel && io.MouseWheel != 0.0f) // Ctrl+wheel: density, like zooming a map
        layers.density.set(LayerDensity(std::clamp(int(layers.density.get()) + (io.MouseWheel > 0.0f ? 1 : -1), 0, 2)));
    if (hovered && ImGui::IsMouseClicked(3)) // mouse back button
        layers.zoomOut();

    const int focus = layers.focus();
    if (focus < 0) {
        ImGui::SetCursorPos(ImVec2(kMargin * s_, kMargin * s_));
        ImGui::TextDisabled(layers.scope.get() == LayerScope::File ? "Open a file to see its layers." : "No project open.");
        ImGui::EndChild();
        return;
    }

    // Slide in from the side we zoom towards.
    if (layers.zoomSerial() != animSerial_) {
        if (animSerial_ >= 0)
            animStart_ = ImGui::GetTime();
        animSerial_ = layers.zoomSerial();
    }
    const float t = float(std::clamp((ImGui::GetTime() - animStart_) / kAnimSeconds, 0.0, 1.0));
    const float e = easeOut(t);
    const float slide = float(layers.zoomDirection()) * 18.0f * s_ * (1.0f - e);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.2f + 0.8f * e);

    const float x = ImGui::GetCursorScreenPos().x + kMargin * s_ + slide;
    const float width = ImGui::GetContentRegionAvail().x - 2.0f * kMargin * s_;
    float y = ImGui::GetCursorScreenPos().y + kMargin * s_;
    const LayerNode& focused = layers.nodes()[size_t(focus)];
    if (layers.density.get() != LayerDensity::Compact)
        y += aboutCard(focused, x, y, width, true) + 10.0f * s_;

    // Section label.
    const size_t count = focused.children.size();
    ImGui::PushFont(nullptr, Fonts::kUiSize * kSmallSize);
    const bool folder = focused.kind == LayerKind::Project || focused.kind == LayerKind::Folder;
    const char* noun = folder ? (count == 1 ? " ITEM" : " ITEMS") : (count == 1 ? " PART" : " PARTS");
    const std::string label = count == 0 ? "NO SMALLER PARTS" : std::to_string(count) + noun;
    ImGui::GetWindowDrawList()->AddText(ImVec2(x + 2.0f * s_, y), ImGui::GetColorU32(palette_.textFaint), label.c_str());
    y += ImGui::GetTextLineHeight() + 5.0f * s_;
    ImGui::PopFont();

    int maxSize = 0;
    for (int child : focused.children)
        maxSize = std::max(maxSize, layers.nodes()[size_t(child)].size);
    const int current = layers.current();
    // Copy: a click can rebuild the tree (opening a file) while we iterate.
    const std::vector<int> children = focused.children;
    for (int child : children) {
        if (child >= int(layers.nodes().size()))
            break;
        y += card(child, maxSize, child == current, x, y, width, true) + kCardGap * s_;
    }
    ImGui::PopStyleVar();
    ImGui::SetCursorScreenPos(ImVec2(x - slide, y));
    ImGui::Dummy(ImVec2(1.0f, kMargin * s_)); // scroll extent
    ImGui::EndChild();
}

} // namespace enp::view
