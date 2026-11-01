# Fit helpers: nothing overlaps on narrow screens (Dear ImGui)

Small helpers that turn the rules in SKILL.md §4 into one-liners, the per-frame window clamp, and
the patterns that replace fixed layouts.

## Fit.h

```cpp
// Making things fit narrow spaces (a phone, a narrow panel): text cut short with an ellipsis
// instead of running into its neighbour, button rows that wrap, dialogs no larger than the screen.
#pragma once

#include "imgui.h"

#include <string>

namespace enp::view::fit {

// Draws `text` at `pos` in the current font, cut short with "..." so it ends before `maxX`.
// Returns the x where the drawn text ends.
float text(ImDrawList* dl, ImVec2 pos, float maxX, ImU32 color, const char* text, const char* end = nullptr);
inline float text(ImDrawList* dl, ImVec2 pos, float maxX, ImU32 color, const std::string& s)
{
    return text(dl, pos, maxX, color, s.c_str(), s.c_str() + s.size());
}

// The width of a Button / SmallButton showing `label` (the part before "##").
float buttonWidth(const char* label, bool small = false);

// Keeps the next item on this line when one `width` wide still fits; otherwise it starts the next
// line. Call after an item, instead of SameLine().
void sameLineIfFits(float width, float spacing = -1.0f);
inline void sameLineIfFitsButton(const char* label, bool small = false)
{
    sameLineIfFits(buttonWidth(label, small));
}

// A width for the next item: `wanted`, or less when the line has less room (never below `least`).
float itemWidth(float wanted, float least);

// A window size: `wanted`, but never larger than the screen minus a margin (dialogs on a phone).
ImVec2 windowSize(ImVec2 wanted);
// Before Begin / BeginPopupModal: the next window may not grow larger than the screen, also when
// it sizes itself to its content.
void constrainNextWindow();

// The room is narrow (a phone, a thin panel): `width` is less than `threshold` (unscaled dp * scale).
inline bool narrow(float width, float threshold) { return width < threshold; }

} // namespace enp::view::fit
```

## Fit.cpp

```cpp
#include "enp/view/Fit.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cstring>

namespace enp::view::fit {

float text(ImDrawList* dl, ImVec2 pos, float maxX, ImU32 color, const char* text, const char* end)
{
    if (!end)
        end = text + std::strlen(text);
    if (maxX <= pos.x)
        return pos.x;
    const ImVec2 size = ImGui::CalcTextSize(text, end, true);
    if (pos.x + size.x <= maxX) {
        dl->AddText(pos, color, text, end);
        return pos.x + size.x;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::RenderTextEllipsis(dl, pos, ImVec2(maxX, pos.y + size.y), maxX, text, end, &size);
    ImGui::PopStyleColor();
    return maxX;
}

float buttonWidth(const char* label, bool small)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    return ImGui::CalcTextSize(label, nullptr, true).x + (small ? style.FramePadding.x : style.FramePadding.x * 2.0f);
}

void sameLineIfFits(float width, float spacing)
{
    const float gap = spacing >= 0.0f ? spacing : ImGui::GetStyle().ItemSpacing.x;
    // After an item the cursor sits at the start of the next line: that line's width is the room.
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    if (ImGui::GetItemRectMax().x + gap + width <= right)
        ImGui::SameLine(0.0f, gap);
}

float itemWidth(float wanted, float least)
{
    return std::max(least, std::min(wanted, ImGui::GetContentRegionAvail().x));
}

ImVec2 windowSize(ImVec2 wanted)
{
    const ImVec2 screen = ImGui::GetMainViewport()->WorkSize;
    const float margin = std::min(24.0f, screen.x * 0.03f);
    return ImVec2(std::min(wanted.x, screen.x - margin * 2.0f), std::min(wanted.y, screen.y - margin * 2.0f));
}

void constrainNextWindow()
{
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), windowSize(ImVec2(FLT_MAX, FLT_MAX)));
}

} // namespace enp::view::fit
```

## Keep floating windows on screen (phone layouts, every frame, after the UI)

```cpp
void MainView::keepWindowsOnScreen()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float margin = 6.0f * s_;
    const ImVec2 min(viewport->WorkPos.x + safeArea_.x + margin, viewport->WorkPos.y + safeArea_.y + margin);
    const ImVec2 max(viewport->WorkPos.x + viewport->WorkSize.x - safeArea_.z - margin,
                     viewport->WorkPos.y + viewport->WorkSize.y - safeArea_.w - margin);
    const ImVec2 room(std::max(1.0f, max.x - min.x), std::max(1.0f, max.y - min.y));
    for (ImGuiWindow* w : GImGui->Windows) {
        // Root floating windows only: not the app itself, not children, not tooltips (ImGui keeps
        // those on screen itself).
        if (!w->Active || (w->Flags & (ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_Tooltip)) || std::strcmp(w->Name, "##main") == 0)
            continue;
        ImVec2 size = w->Size;
        if (!(w->Flags & ImGuiWindowFlags_AlwaysAutoResize) && (size.x > room.x || size.y > room.y)) {
            size = ImVec2(std::min(size.x, room.x), std::min(size.y, room.y));
            ImGui::SetWindowSize(w, size, ImGuiCond_Always); // from the next frame
        }
        const ImVec2 pos(std::clamp(w->Pos.x, min.x, std::max(min.x, max.x - size.x)),
                         std::clamp(w->Pos.y, min.y, std::max(min.y, max.y - size.y)));
        if (pos.x != w->Pos.x || pos.y != w->Pos.y)
            ImGui::SetWindowPos(w, pos, ImGuiCond_Always);
    }
}

MainView::Layout MainView::chooseLayout(float widthDp, float heightDp)
{
    if (widthDp < 600.0f) // a phone, or a narrow window
        return widthDp >= heightDp ? Layout::Landscape : Layout::Portrait;
    if (heightDp < 500.0f) // a phone turned sideways
```

This is the safety net. It cannot shrink `AlwaysAutoResize` windows, and a size set every frame
by the dialog overrides it, so also:

```cpp
// before:  ImGui::SetNextWindowSize(ImVec2(440.0f * s, 0.0f));
ImGui::SetNextWindowSize(fit::windowSize(ImVec2(440.0f * s, 0.0f)));
fit::constrainNextWindow();                 // before every BeginPopupModal / Begin of a dialog
if (!ImGui::BeginPopupModal("Stash changes", nullptr, ImGuiWindowFlags_NoResize))
```

A one-off script can convert a codebase: regex `SetNextWindowSize\(ImVec2\(([^()]*)\)` →
`SetNextWindowSize(fit::windowSize(ImVec2(\1))`, and insert `fit::constrainNextWindow();` before
each `BeginPopupModal(` line.

## Row with priority columns (instead of fixed offsets from the right)

```cpp
const float right = ImGui::GetItemRectMax().x - 4 * s;
const float hashW = 70 * s, whenW = 100 * s, authorW = 120 * s, messageMin = 140 * s;
const float room = right - x;
const bool showWhen = room - whenW >= messageMin * 0.7f;
const bool showHash = showWhen && room - whenW - hashW >= messageMin;
const bool showAuthor = showHash && room - whenW - hashW - authorW >= messageMin + 60 * s;
// positions from the right; the message ends before the first visible column:
fit::text(dl, {x, y}, messageEnd, textColor, message);
// badges before the message: cut the last one that fits, count the rest ("+2")
```

## Controls at the right of a heading, or under it when narrow

```cpp
auto controlsAtRight = [&](float width) {
    const float headingEnd = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + ImGui::GetScrollX();
    if (headingEnd + 12 * s + width <= contentMaxX())
        ImGui::SameLine(contentMaxX() - width);
    // else: the next item starts on its own line
};
```

## Other patterns

- Wrap messages: `PushTextWrapPos(0.0f)` … `PopTextWrapPos()` (0 = the window/cell edge).
- A row of buttons: `fit::sameLineIfFitsButton(label)` before each button except the first.
- An input with buttons after it: `SetNextItemWidth(clamp(avail - buttonsW, minW, wantedW))`.
- A bar that wraps: `BeginChild(id, {0, 0}, ImGuiChildFlags_AutoResizeY | …)` instead of a fixed height.
- A tab row too wide: put it in its own child with `HorizontalScrollbar`, a 3 px scrollbar, and
  `SetScrollHereX(0.5f)` once when the selected tab changes.
- Tables: decide `narrow` **before** `BeginTable`; disable columns with
  `ImGuiTableColumnFlags_Disabled`; stretch a fixed column instead of reserving 200 px.
- Side-by-side → stacked: `stacked = avail.x < 640 * s`; first child `{0, max(140*s, avail.y*0.5f)}`,
  then the second without `SameLine`.
