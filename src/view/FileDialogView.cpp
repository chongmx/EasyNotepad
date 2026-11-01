#include "enp/view/FileDialogView.h"
#include "enp/view/Fit.h"

#include "enp/model/Language.h"
#include "enp/view/UiAutomation.h"

#include "IconsFontAwesome6.h"
#include "misc/cpp/imgui_stdlib.h"

namespace enp::view {

using viewmodel::FileDialogMode;

namespace {

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

} // namespace

FileDialogView::FileDialogView(viewmodel::FileDialogViewModel& vm, const Fonts& fonts, const Palette& palette, float uiScale)
    : vm_(vm), fonts_(fonts), palette_(palette), s_(uiScale)
{
}

void FileDialogView::render()
{
    const char* title = vm_.mode() == FileDialogMode::Open     ? "Open File###fileDialog"
                        : vm_.mode() == FileDialogMode::SaveAs ? "Save As###fileDialog"
                                                               : "Open Folder###fileDialog";
    const bool wanted = vm_.visible.get();
    if (wanted && !ImGui::IsPopupOpen("###fileDialog")) {
        ImGui::OpenPopup("###fileDialog");
        selected_ = -1;
        focusName_ = true;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(fit::windowSize(ImVec2(780.0f * s_, 500.0f * s_)), ImGuiCond_Appearing);
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

    // Toolbar: back, up, the folder (editable).
    if (shownFolder_ != vm_.folder()) {
        shownFolder_ = vm_.folder();
        pathEdit_ = shownFolder_;
        selected_ = -1;
    }
    ImGui::BeginDisabled(!vm_.canGoBack());
    if (ImGui::Button(ICON_FA_ARROW_LEFT))
        vm_.back();
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Back");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_ARROW_UP))
        vm_.up();
    ImGui::SetItemTooltip("Up one folder");
    registerLastItem("file-dialog:up", "button", "Up one folder", [this]() { vm_.up(); });
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##folder", &pathEdit_, ImGuiInputTextFlags_EnterReturnsTrue))
        vm_.goTo(pathEdit_);
    registerLastItem("file-dialog:folder", "textbox", "Folder", nullptr);

    // Places and entries.
    const float footerH = ImGui::GetFrameHeightWithSpacing() * 2.0f + 14.0f * s_;
    ImGui::BeginChild("##places", ImVec2(150.0f * s_, -footerH), ImGuiChildFlags_Borders);
    for (const auto& [label, path] : vm_.places()) {
        const std::string text = std::string(label == "Workspace" ? ICON_FA_FOLDER_TREE "  "
                                             : label == "Home"    ? ICON_FA_HOUSE "  "
                                                                  : ICON_FA_HARD_DRIVE "  ") +
                                 label;
        if (ImGui::Selectable(text.c_str(), vm_.folder() == path))
            vm_.goTo(path);
        registerLastItem("file-dialog:place:" + label, "button", label, [this, p = path]() { vm_.goTo(p); });
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##entries", ImVec2(0.0f, -footerH), ImGuiChildFlags_Borders);
    const auto entries = vm_.visibleEntries();
    if (entries.empty())
        ImGui::TextDisabled(vm_.mode() == FileDialogMode::OpenFolder ? "No folders here" : "Nothing here");
    for (int i = 0; i < int(entries.size()); ++i) {
        const auto& e = entries[size_t(i)];
        ImGui::PushID(i);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(e.isDirectory ? palette_.text : palette_.textDim));
        const std::string label = std::string(entryIcon(e)) + "  " + e.name;
        if (ImGui::Selectable(label.c_str(), selected_ == i, ImGuiSelectableFlags_AllowDoubleClick)) {
            selected_ = i;
            const bool twice = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
            if (!e.isDirectory)
                vm_.choose(e, twice); // a file: select it (double click: open / save)
            else if (twice)
                vm_.choose(e, false); // a folder: double click goes in
        }
        ImGui::PopStyleColor();
        registerLastItem("file-dialog:entry:" + e.name, e.isDirectory ? "folder" : "file", e.name,
                         [this, e]() { vm_.choose(e, !e.isDirectory); });
        ImGui::PopID();
    }
    ImGui::EndChild();

    // File name, error, buttons.
    const char* nameLabel = vm_.mode() == FileDialogMode::SaveAs       ? "File name"
                            : vm_.mode() == FileDialogMode::OpenFolder ? "Folder"
                                                                       : "File name, path or filter";
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", nameLabel);
    ImGui::SameLine();
    const char* action = vm_.mode() == FileDialogMode::Open ? ICON_FA_FOLDER_OPEN "  Open"
                         : vm_.mode() == FileDialogMode::SaveAs ? (vm_.confirmOverwrite ? ICON_FA_TRIANGLE_EXCLAMATION "  Replace" : ICON_FA_FLOPPY_DISK "  Save")
                                                                : ICON_FA_FOLDER_TREE "  Open Folder";
    const float buttonsW = 230.0f * s_;
    ImGui::SetNextItemWidth(std::max(100.0f * s_, ImGui::GetContentRegionAvail().x - buttonsW));
    if (focusName_) {
        ImGui::SetKeyboardFocusHere();
        focusName_ = false;
    }
    bool confirm = ImGui::InputText("##name", &vm_.fileName, ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::IsItemEdited())
        vm_.confirmOverwrite = false;
    registerLastItem("file-dialog:name", "textbox", nameLabel, nullptr);
    ImGui::SameLine();
    confirm = ImGui::Button(action, ImVec2(buttonsW * 0.55f, 0.0f)) || confirm;
    registerLastItem("file-dialog:confirm", "button", "Confirm", [this]() { vm_.confirm(); });
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(-1.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
        vm_.cancel();
    registerLastItem("file-dialog:cancel", "button", "Cancel", [this]() { vm_.cancel(); });
    if (confirm) {
        vm_.confirm();
        focusName_ = vm_.visible.get();
    }
    if (!vm_.error.get().empty())
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(vm_.confirmOverwrite ? palette_.severityWarning : palette_.severityError), "%s",
                           vm_.error.get().c_str());
    else
        ImGui::TextDisabled("%s", vm_.folder().c_str());

    ImGui::PopID();
    ImGui::EndPopup();
}

} // namespace enp::view
