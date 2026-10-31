#include "enp/view/UiAutomation.h"

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
    UiAutomation::instance().add(std::move(e));
}

} // namespace enp::view
