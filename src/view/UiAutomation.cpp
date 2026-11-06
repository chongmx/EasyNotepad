#include "enp/view/UiAutomation.h"

#include "enp/os/Memory.h"
#include "enp/view/UiStyle.h"

#include <algorithm>
#include <cfloat>

namespace enp::view {

UiAutomation& UiAutomation::instance()
{
    static UiAutomation automation;
    return automation;
}

void UiAutomation::beginFrame()
{
    building_.clear();
}

void UiAutomation::endFrame()
{
    current_.swap(building_);
    // Both buffers keep the capacity of the busiest frame (a large explorer tree, a long git log):
    // once the screen holds far fewer elements, the room goes back (docs/MEMORY.md rule 5).
    const size_t n = current_.size();
    if (current_.capacity() > 4 * n + 64)
        current_.shrink_to_fit();
    if (building_.capacity() > 4 * n + 64)
        os::mem::release(building_);
}

void UiAutomation::add(UiElement element)
{
    // Later registrations of the same id replace earlier ones (e.g. a control drawn twice).
    for (UiElement& e : building_)
        if (e.id == element.id) {
            e = std::move(element);
            return;
        }
    building_.push_back(std::move(element));
}

const UiElement* UiAutomation::find(std::string_view id) const
{
    for (const UiElement& e : current_)
        if (e.id == id)
            return &e;
    return nullptr;
}

bool registerWindowClose(const std::string& id, const std::string& label)
{
    static std::vector<std::string> requested; // clicked by an agent, not yet handled
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 pos = ImGui::GetWindowPos();
    const float size = ImGui::GetFontSize();
    const float titleH = ImGui::GetFontSize() + style.FramePadding.y * 2.0f;
    UiElement e;
    e.id = id;
    e.role = "button";
    e.label = label;
    e.min = ImVec2(pos.x + ImGui::GetWindowWidth() - style.FramePadding.x - size, pos.y + (titleH - size) * 0.5f);
    e.max = ImVec2(e.min.x + size, e.min.y + size);
    e.activate = [id]() { requested.push_back(id); };
    UiAutomation::instance().add(std::move(e));
    const auto it = std::find(requested.begin(), requested.end(), id);
    if (it == requested.end())
        return false;
    requested.erase(it);
    return true;
}

void UiAutomation::hover(ImVec2 at, double seconds)
{
    hoverAt_ = at;
    hoverUntil_ = ImGui::GetTime() + std::max(0.1, seconds);
}

void UiAutomation::applyHover()
{
    if (hoverUntil_ <= 0.0)
        return;
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::GetTime() < hoverUntil_) {
        io.AddMousePosEvent(hoverAt_.x, hoverAt_.y); // read at the next frame's start
        return;
    }
    hoverUntil_ = 0.0;
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX); // gone: until the real mouse moves
}

void registerLastItem(std::string id, std::string role, std::string label, std::function<void()> activate, bool enabled)
{
    UiElement e;
    e.id = std::move(id);
    e.role = std::move(role);
    e.label = std::move(label);
    e.min = ImGui::GetItemRectMin();
    e.max = ImGui::GetItemRectMax();
    e.enabled = enabled;
    e.activate = std::move(activate);
    // The enhanced UI lights the control under the pointer (ImGui knows it is this item, not
    // one under a popup that covers it).
    if (enabled && (e.role == "button" || e.role == "tab" || e.role == "toggle" || e.role == "checkbox") && ui::enhanced() && ImGui::IsItemHovered())
        ui::hoverGlow(ImGui::GetWindowDrawList(), e.min, e.max, 5.0f * ImGui::GetStyle().FontScaleDpi);
    UiAutomation::instance().add(std::move(e));
}

} // namespace enp::view
