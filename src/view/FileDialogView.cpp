#include "enp/view/FileDialogView.h"
#include "enp/view/Fit.h"

#include "enp/model/Language.h"
#include "enp/view/TouchScroll.h"
#include "enp/view/UiAutomation.h"
#include "enp/view/UiStyle.h"

#include "IconsFontAwesome6.h"
#include "misc/cpp/imgui_stdlib.h"

#include <algorithm>

namespace enp::view {

using viewmodel::FileDialogMode;

namespace {

// Below this width the dialog uses the narrow arrangement (a phone, a thin window).
constexpr float kNarrowWidth = 560.0f;
// A row of the list is at least this tall for a finger.
constexpr float kTouchRow = 40.0f;

const char* entryIcon(const viewmodel::FileDialogViewModel::Entry& e)
{
    if (e.isDirectory)
        return ICON_FA_FOLDER;
    switch (model::languageFromPath(e.name)) {
    case model::Language::Markdown: return ICON_FA_FILE_PEN;
    case model::Language::PlainText: return ICON_FA_FILE_LINES;
    default: return ICON_FA_FILE_CODE;
    }
}

std::string placeLabel(const std::string& label)
{
    return std::string(label == "Workspace" ? ICON_FA_FOLDER_TREE "  "
                       : label == "Home"    ? ICON_FA_HOUSE "  "
                                            : ICON_FA_HARD_DRIVE "  ") +
           label;
}

} // namespace

FileDialogView::FileDialogView(viewmodel::FileDialogViewModel& vm, const Fonts& fonts, const Palette& palette, float uiScale)
    : vm_(vm), fonts_(fonts), palette_(palette), s_(uiScale)
{
}

const char* FileDialogView::nameLabel() const
{
    return vm_.mode() == FileDialogMode::SaveAs       ? "File name"
           : vm_.mode() == FileDialogMode::OpenFolder ? "Folder"
                                                      : "File name, path or filter";
}

void FileDialogView::render()
{
    const char* title = vm_.mode() == FileDialogMode::Open     ? "Open File###fileDialog"
                        : vm_.mode() == FileDialogMode::SaveAs ? "Save As###fileDialog"
                        : vm_.mode() == FileDialogMode::OpenWorkspace ? "Open Workspace (.code-workspace, .sln, .slnx)###fileDialog"
                                                               : "Open Folder###fileDialog";
    const bool wanted = vm_.visible.get();
    if (wanted && !ImGui::IsPopupOpen("###fileDialog")) {
        ImGui::OpenPopup("###fileDialog");
        selected_ = -1;
        focusName_ = true;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    // A phone gives the dialog the whole screen (windowSize keeps a margin): the list needs the height.
    const bool phone = ImGui::GetMainViewport()->WorkSize.x < kNarrowWidth * s_;
    ImGui::SetNextWindowSize(fit::windowSize(ImVec2(780.0f * s_, (phone ? 2000.0f : 500.0f) * s_)), ImGuiCond_Appearing);
    bool open = true;
    fit::constrainNextWindow();
    const bool shown = ImGui::BeginPopupModal(title, &open);
    // The title bar's ×: ImGui closes the popup and returns false in the same frame, so tell the
    // view-model here (else it stays visible and the dialog reopens on the next frame).
    if (!open && wanted)
        vm_.cancel();
    if (!shown)
        return;
    if (registerWindowClose("dialog:file:close", "Close the dialog"))
        vm_.cancel();
    if (!wanted || !open) { // closed by the view-model (or an agent)
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::PushID("fileDialog");

    const bool narrow = fit::narrow(ImGui::GetContentRegionAvail().x, kNarrowWidth * s_);
    renderToolbar();
    const float footer = footerHeight(narrow, ImGui::GetContentRegionAvail().x);
    renderPlaces(narrow, footer);
    renderEntries(narrow, footer);
    renderFooter(narrow);

    ImGui::PopID();
    ImGui::EndPopup();
}

// Back, up, the folder (editable).
void FileDialogView::renderToolbar()
{
    if (shownFolder_ != vm_.folder()) {
        shownFolder_ = vm_.folder();
        pathEdit_ = shownFolder_;
        selected_ = -1;
    }
    ImGui::BeginDisabled(!vm_.canGoBack());
    if (ImGui::Button(ICON_FA_ARROW_LEFT))
        vm_.back();
    ImGui::EndDisabled();
    ui::itemTooltip("Back");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_ARROW_UP))
        vm_.up();
    ui::itemTooltip("Up one folder");
    registerLastItem("file-dialog:up", "button", "Up one folder", [this]() { vm_.up(); });
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##folder", &pathEdit_, ImGuiInputTextFlags_EnterReturnsTrue))
        vm_.goTo(pathEdit_);
    registerLastItem("file-dialog:folder", "textbox", "Folder", nullptr);
}

// What lies below the list: the name (and the buttons), and a line for an error.
float FileDialogView::footerHeight(bool narrow, float width) const
{
    const float row = ImGui::GetFrameHeightWithSpacing();
    if (!narrow)
        return row * 2.0f + 14.0f * s_;
    // The name, the buttons; an error wraps to the width. (The folder is in the box above.)
    const std::string& error = vm_.error.get();
    const float message = error.empty() ? 0.0f : ImGui::CalcTextSize(error.c_str(), nullptr, false, width).y + ImGui::GetStyle().ItemSpacing.y;
    return row * 2.0f + message + 6.0f * s_;
}

// Wide: a pane on the left. Narrow: buttons in a row above the list (which keeps the width).
void FileDialogView::renderPlaces(bool narrow, float footerHeight)
{
    if (narrow) {
        renderPlaceButtons();
        return;
    }
    ImGui::BeginChild("##places", ImVec2(150.0f * s_, -footerHeight), ImGuiChildFlags_Borders);
    for (const auto& [label, path] : vm_.places()) {
        if (ImGui::Selectable(placeLabel(label).c_str(), vm_.folder() == path))
            vm_.goTo(path);
        registerLastItem("file-dialog:place:" + label, "button", label, [this, p = path]() { vm_.goTo(p); });
    }
    ImGui::EndChild();
    ImGui::SameLine();
}

void FileDialogView::renderPlaceButtons()
{
    bool first = true;
    for (const auto& [label, path] : vm_.places()) {
        const std::string text = placeLabel(label);
        if (!first)
            fit::sameLineIfFitsButton(text.c_str());
        first = false;
        const bool here = vm_.folder() == path;
        if (here)
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(palette_.accentSoft));
        if (ImGui::Button((text + "##place" + label).c_str()))
            vm_.goTo(path);
        if (here)
            ImGui::PopStyleColor();
        registerLastItem("file-dialog:place:" + label, "button", label, [this, p = path]() { vm_.goTo(p); });
    }
}

void FileDialogView::renderEntries(bool narrow, float footerHeight)
{
    ImGui::BeginChild("##entries", ImVec2(0.0f, -footerHeight), ImGuiChildFlags_Borders);
    vm_.checkStorageAccess();
    if (vm_.storageAccessMissing()) { // Android: only the app's own folders until the user allows more
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(palette_.severityWarning));
        ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION "  EasyNotepad can only see its own files. To open files "
                           "and folders elsewhere on the device (Download, Documents, an SD card), allow it "
                           "access to all files (Android's \"All files access\"), then come back here.");
        ImGui::PopStyleColor();
        if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Allow access to all files"))
            vm_.requestStorageAccess();
        registerLastItem("file-dialog:storage-access", "button", "Allow access to all files",
                         [this]() { vm_.requestStorageAccess(); });
        ImGui::Separator();
    }
    const auto entries = vm_.visibleEntries();
    if (entries.empty())
        ImGui::TextDisabled(vm_.mode() == FileDialogMode::OpenFolder ? "No folders here" : "Nothing here");
    for (int i = 0; i < int(entries.size()); ++i)
        renderEntry(i, entries[size_t(i)], narrow);
    ImGui::EndChild();
}

// One row: a click selects (a file: its name goes to the name box). A folder opens on a double
// click, or on a tap of a finger, which has no double click.
void FileDialogView::renderEntry(int index, const viewmodel::FileDialogViewModel::Entry& e, bool narrow)
{
    ImGui::PushID(index);
    const float rowH = narrow ? std::max(ImGui::GetFrameHeight(), kTouchRow * s_) : ImGui::GetTextLineHeightWithSpacing();
    if (ImGui::Selectable("##entry", selected_ == index, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0.0f, rowH))) {
        selected_ = index;
        const bool twice = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        if (!e.isDirectory)
            vm_.choose(e, twice); // a file: select it (double click: open / save)
        else if (twice || touch::fingerDown())
            vm_.choose(e, false); // a folder: double click or a tap goes in
    }
    registerLastItem("file-dialog:entry:" + e.name, e.isDirectory ? "folder" : "file", e.name,
                     [this, e]() { vm_.choose(e, !e.isDirectory); });
    // Icon and name, the name cut short with "..." instead of running out of the row.
    const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float y = min.y + (max.y - min.y - ImGui::GetTextLineHeight()) * 0.5f;
    const ImU32 color = e.isDirectory ? palette_.text : palette_.textDim;
    const float x = min.x + ImGui::GetStyle().FramePadding.x;
    const float iconEnd = fit::text(dl, ImVec2(x, y), max.x, color, entryIcon(e));
    fit::text(dl, ImVec2(std::max(iconEnd, x + ImGui::GetFontSize() * 1.3f), y), max.x - ImGui::GetStyle().FramePadding.x, color, e.name);
    ImGui::PopID();
}

// The name, the buttons, an error or the folder.
void FileDialogView::renderFooter(bool narrow)
{
    const char* action = vm_.mode() == FileDialogMode::Open || vm_.mode() == FileDialogMode::OpenWorkspace ? ICON_FA_FOLDER_OPEN "  Open"
                         : vm_.mode() == FileDialogMode::SaveAs ? (vm_.confirmOverwrite ? ICON_FA_TRIANGLE_EXCLAMATION "  Replace" : ICON_FA_FLOPPY_DISK "  Save")
                                                                : ICON_FA_FOLDER_TREE "  Open Folder";
    if (!narrow) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", nameLabel());
        ImGui::SameLine();
    }
    const float buttonsW = 230.0f * s_;
    ImGui::SetNextItemWidth(narrow ? -1.0f : std::max(100.0f * s_, ImGui::GetContentRegionAvail().x - buttonsW));
    if (focusName_) {
        ImGui::SetKeyboardFocusHere();
        focusName_ = false;
    }
    bool confirm = narrow ? ImGui::InputTextWithHint("##name", nameLabel(), &vm_.fileName, ImGuiInputTextFlags_EnterReturnsTrue)
                          : ImGui::InputText("##name", &vm_.fileName, ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::IsItemEdited())
        vm_.confirmOverwrite = false;
    registerLastItem("file-dialog:name", "textbox", nameLabel(), nullptr);
    if (narrow) { // the buttons share the next line
        const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        confirm = ImGui::Button(action, ImVec2(half, 0.0f)) || confirm;
    } else {
        ImGui::SameLine();
        confirm = ImGui::Button(action, ImVec2(buttonsW * 0.55f, 0.0f)) || confirm;
    }
    registerLastItem("file-dialog:confirm", "button", "Confirm", [this]() { vm_.confirm(); });
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(-1.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
        vm_.cancel();
    registerLastItem("file-dialog:cancel", "button", "Cancel", [this]() { vm_.cancel(); });
    if (confirm) {
        vm_.confirm();
        focusName_ = vm_.visible.get();
    }
    if (!vm_.error.get().empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(vm_.confirmOverwrite ? palette_.severityWarning : palette_.severityError));
        ImGui::TextWrapped("%s", vm_.error.get().c_str());
        ImGui::PopStyleColor();
    } else if (!narrow) {
        ImGui::TextDisabled("%s", vm_.folder().c_str());
    }
}

} // namespace enp::view
