#include "enp/view/PanelFrame.h"

#include <cmath>
#include <vector>

namespace enp::view::panel {

namespace {

struct Frame {
    ImVec2 min, max;
    float rounding;
};

std::vector<Frame>& frames()
{
    static std::vector<Frame> stack;
    return stack;
}

bool near(float a, float b) { return std::fabs(a - b) < 1.5f; }

} // namespace

void push(ImVec2 min, ImVec2 max, float rounding)
{
    frames().push_back({min, max, rounding});
}

void pop()
{
    if (!frames().empty())
        frames().pop_back();
}

ImDrawFlags cornersOf(ImVec2 min, ImVec2 max)
{
    if (frames().empty())
        return ImDrawFlags_RoundNone;
    const Frame& f = frames().back();
    const bool left = near(min.x, f.min.x), right = near(max.x, f.max.x);
    const bool top = near(min.y, f.min.y), bottom = near(max.y, f.max.y);
    ImDrawFlags flags = 0;
    if (top && left)
        flags |= ImDrawFlags_RoundTopLeft;
    if (top && right)
        flags |= ImDrawFlags_RoundTopRight;
    if (bottom && left)
        flags |= ImDrawFlags_RoundBottomLeft;
    if (bottom && right)
        flags |= ImDrawFlags_RoundBottomRight;
    return flags ? flags : ImDrawFlags_RoundNone;
}

float rounding()
{
    return frames().empty() ? 0.0f : frames().back().rounding;
}

void fill(ImDrawList* drawList, ImVec2 min, ImVec2 max, ImU32 color)
{
    const ImDrawFlags corners = cornersOf(min, max);
    drawList->AddRectFilled(min, max, color, corners == ImDrawFlags_RoundNone ? 0.0f : rounding(), corners);
}

bool beginChild(const char* id, ImVec2 size, ImU32 background, ImGuiChildFlags childFlags, ImGuiWindowFlags flags)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
    const bool visible = ImGui::BeginChild(id, size, childFlags, flags);
    ImGui::PopStyleColor();
    const ImVec2 p = ImGui::GetWindowPos();
    const ImVec2 s = ImGui::GetWindowSize();
    // The child's own draw list is drawn after its parent's, so the fill must be clipped to the
    // child only: its clip rect already is.
    fill(ImGui::GetWindowDrawList(), p, ImVec2(p.x + s.x, p.y + s.y), background);
    return visible;
}

} // namespace enp::view::panel
