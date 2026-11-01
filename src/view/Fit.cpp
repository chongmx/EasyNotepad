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
