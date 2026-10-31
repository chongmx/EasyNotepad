#include "enp/view/DocumentListView.h"

#include "enp/model/Language.h"
#include "enp/view/UiAutomation.h"

#include "IconsFontAwesome6.h"

#include <algorithm>
#include <cctype>

namespace enp::view {

namespace {

const char* fileIcon(const std::string& name)
{
    switch (model::languageFromPath(name)) {
    case model::Language::Markdown: return ICON_FA_FILE_PEN;
    case model::Language::PlainText: return ICON_FA_FILE_LINES;
    default: return ICON_FA_FILE_CODE;
    }
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

} // namespace

DocumentListView::DocumentListView(viewmodel::NotepadViewModel& vm, const Palette& palette, float uiScale)
    : vm_(vm), palette_(palette), s_(uiScale)
{
}

bool DocumentListView::matches(const model::Document& doc, const std::string& filter)
{
    return filter.empty() || lower(doc.path.empty() ? doc.title : doc.path).find(lower(filter)) != std::string::npos;
}

int DocumentListView::render(const char* id, const std::string& filter, Options options, bool enterPicks)
{
    ImGui::PushID(id);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rowH = ImGui::GetFrameHeight();
    int picked = -1, closeIndex = -1, moveFrom = -1, moveTo = -1, first = -1;
    const int count = int(vm_.documents().size());
    for (int i = 0; i < count; ++i) {
        const model::Document& doc = *vm_.documents()[size_t(i)];
        if (!matches(doc, filter))
            continue;
        if (first < 0)
            first = i;
        const bool active = i == vm_.activeIndex.get();
        ImGui::PushID(doc.id);
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const ImVec2 max(min.x + width, min.y + rowH);
        ImGui::SetNextItemAllowOverlap();
        if (ImGui::InvisibleButton("##row", ImVec2(std::max(1.0f, width), rowH)))
            picked = i;
        registerLastItem(std::string(id) + ":" + (doc.path.empty() ? doc.title : doc.path), "file", doc.title,
                         [this, i]() { vm_.activate(i); });
        const bool hovered = ImGui::IsItemHovered();
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
            closeIndex = i;
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            requestMenu(i);
        if (options.reorder && filter.empty() && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f * s_)) {
            const float my = ImGui::GetIO().MousePos.y;
            if (my < min.y - rowH * 0.5f && i > 0) {
                moveFrom = i;
                moveTo = i - 1;
            } else if (my > max.y + rowH * 0.5f && i + 1 < count) {
                moveFrom = i;
                moveTo = i + 1;
            }
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        }
        if (active)
            dl->AddRectFilled(min, max, palette_.selection, 4.0f * s_);
        else if (hovered)
            dl->AddRectFilled(min, max, palette_.hover, 4.0f * s_);

        // Close button (hover) or unsaved dot, on the right.
        const float size = ImGui::GetTextLineHeight();
        const float yc = min.y + rowH * 0.5f;
        const ImVec2 closeMin(max.x - size - 6.0f * s_, yc - size * 0.5f);
        ImGui::SetCursorScreenPos(closeMin);
        if (ImGui::InvisibleButton("##close", ImVec2(size, size)))
            closeIndex = i;
        const bool closeHovered = ImGui::IsItemHovered();
        if (closeHovered)
            dl->AddRectFilled(closeMin, ImVec2(closeMin.x + size, closeMin.y + size), palette_.hover, 3.0f * s_);
        const ImVec2 c(closeMin.x + size * 0.5f, yc);
        if (hovered || closeHovered) {
            const float r = size * 0.22f;
            const ImU32 color = closeHovered ? palette_.text : palette_.textFaint;
            dl->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), color, 1.5f * s_);
            dl->AddLine(ImVec2(c.x - r, c.y + r), ImVec2(c.x + r, c.y - r), color, 1.5f * s_);
        } else if (doc.modified) {
            dl->AddCircleFilled(c, 3.5f * s_, palette_.accent);
        }

        // Icon, name, then the folder (dim), cut to fit.
        const char* icon = fileIcon(doc.title);
        const float textY = yc - ImGui::GetTextLineHeight() * 0.5f;
        float x = min.x + 8.0f * s_;
        dl->AddText(ImVec2(x, textY), active ? palette_.accent : palette_.textFaint, icon);
        x += ImGui::CalcTextSize(icon).x + 7.0f * s_;
        dl->PushClipRect(ImVec2(x, min.y), ImVec2(closeMin.x - 6.0f * s_, max.y), true);
        dl->AddText(ImVec2(x, textY), active ? palette_.text : palette_.textDim, doc.title.c_str());
        const std::string folder = doc.path.find('/') != std::string::npos ? doc.path.substr(0, doc.path.rfind('/')) : std::string();
        if (!folder.empty())
            dl->AddText(ImVec2(x + ImGui::CalcTextSize(doc.title.c_str()).x + 8.0f * s_, textY), palette_.textFaint, folder.c_str());
        dl->PopClipRect();
        if (hovered && !closeHovered)
            ImGui::SetItemTooltip("%s%s", doc.path.empty() ? doc.title.c_str() : doc.path.c_str(), doc.modified ? "\n(unsaved changes)" : "");
        ImGui::SetCursorScreenPos(ImVec2(min.x, max.y));
        ImGui::PopID();
    }
    if (first < 0)
        ImGui::TextDisabled(count == 0 ? "No open documents" : "No open documents match");
    else
        ImGui::Dummy(ImVec2(1.0f, 4.0f * s_)); // ends the list with an item (the cursor moved past the last row)
    if (enterPicks && first >= 0 && ImGui::IsKeyPressed(ImGuiKey_Enter))
        picked = first;
    ImGui::PopID();

    if (moveFrom >= 0)
        vm_.moveDocument(moveFrom, moveTo);
    else if (closeIndex >= 0)
        vm_.requestCloseDocument(closeIndex);
    if (picked >= 0)
        vm_.activate(picked);
    return picked;
}

void DocumentListView::requestMenu(int index)
{
    menuIndex_ = index;
    openMenu_ = true;
}

void DocumentListView::renderMenu()
{
    if (openMenu_) {
        ImGui::OpenPopup("##documentMenu");
        openMenu_ = false;
    }
    if (!ImGui::BeginPopup("##documentMenu"))
        return;
    const int i = menuIndex_;
    const int count = int(vm_.documents().size());
    if (i < 0 || i >= count) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const model::Document& doc = *vm_.documents()[size_t(i)];
    if (ImGui::MenuItem(ICON_FA_XMARK "  Close", "Ctrl+W"))
        vm_.requestCloseDocument(i);
    if (ImGui::MenuItem(ICON_FA_RECTANGLE_XMARK "  Close Others", nullptr, false, count > 1))
        vm_.closeOthers(i);
    if (ImGui::MenuItem(ICON_FA_ARROW_RIGHT_TO_BRACKET "  Close to the Right", nullptr, false, i + 1 < count))
        vm_.closeToTheRight(i);
    if (ImGui::MenuItem(ICON_FA_CIRCLE_CHECK "  Close Saved"))
        vm_.closeSaved();
    if (ImGui::MenuItem(ICON_FA_BROOM "  Close All"))
        vm_.closeAll();
    ImGui::Separator();
    if (ImGui::MenuItem(ICON_FA_COPY "  Copy Path", nullptr, false, !doc.path.empty()))
        ImGui::SetClipboardText(vm_.absolutePath(doc.path).c_str());
    if (ImGui::MenuItem(ICON_FA_CROSSHAIRS "  Reveal in Explorer", nullptr, false, !doc.path.empty()) && onReveal)
        onReveal(i);
    ImGui::EndPopup();
}

} // namespace enp::view
