#include "enp/view/SearchPanel.h"

#include "enp/view/UiAutomation.h"

#include "IconsFontAwesome6.h"
#include "misc/cpp/imgui_stdlib.h"

#include <algorithm>

namespace enp::view {

SearchPanel::SearchPanel(viewmodel::NotepadViewModel& vm, const Fonts& fonts, const Palette& palette, float uiScale)
    : vm_(vm), fonts_(fonts), palette_(palette), s_(uiScale)
{
    subscriptions_.push_back(vm_.searchFocusRequested.subscribe([this]() { focusQuery_ = true; }));
}

void SearchPanel::render()
{
    viewmodel::FindInFilesViewModel& find = vm_.findInFiles();
    ImGui::PushID("searchPanel");

    // Query row: replace toggle, query, options, include filter.
    if (ImGui::Button(showReplace_ ? ICON_FA_CHEVRON_DOWN : ICON_FA_CHEVRON_RIGHT))
        showReplace_ = !showReplace_;
    ImGui::SetItemTooltip("Replace in files");
    ImGui::SameLine();
    const float optionsW = 3.0f * (ImGui::GetFrameHeight() + 4.0f * s_);
    const float includeW = 170.0f * s_;
    ImGui::SetNextItemWidth(std::max(120.0f * s_, ImGui::GetContentRegionAvail().x - optionsW - includeW - 16.0f * s_));
    if (focusQuery_) {
        ImGui::SetKeyboardFocusHere();
        focusQuery_ = false;
    }
    bool run = ImGui::InputTextWithHint("##query", ICON_FA_MAGNIFYING_GLASS "  Search in the workspace", &find.query,
                                        ImGuiInputTextFlags_EnterReturnsTrue);
    registerLastItem("search:query", "textbox", "Search in files", nullptr);
    auto toggle = [&](const char* label, const char* tip, bool& value, const char* id) {
        ImGui::SameLine(0.0f, 4.0f * s_);
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(value ? palette_.accentSoft : palette_.panelHeaderBg));
        if (ImGui::Button(label, ImVec2(ImGui::GetFrameHeight(), 0.0f))) {
            value = !value;
            run = true;
        }
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("%s", tip);
        registerLastItem(id, "button", tip, [&value, this]() { value = !value; vm_.findInFiles().search(); });
    };
    toggle("Aa", "Match case", find.options.matchCase, "search:match-case");
    toggle("ab", "Whole word", find.options.wholeWord, "search:whole-word");
    toggle(".*", "Regular expression", find.options.regex, "search:regex");
    ImGui::SameLine(0.0f, 8.0f * s_);
    ImGui::SetNextItemWidth(-1.0f);
    run = ImGui::InputTextWithHint("##include", "Files: *.cpp;*.h", &find.include, ImGuiInputTextFlags_EnterReturnsTrue) || run;
    registerLastItem("search:include", "textbox", "Files to include", nullptr);
    if (showReplace_) {
        ImGui::Dummy(ImVec2(ImGui::GetFrameHeight(), 0.0f));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(std::max(120.0f * s_, ImGui::GetContentRegionAvail().x - 140.0f * s_));
        ImGui::InputTextWithHint("##replace", find.options.regex ? "Replace with ($1 = first group)" : "Replace with",
                                 &find.replacement);
        registerLastItem("search:replacement", "textbox", "Replace with", nullptr);
        ImGui::SameLine();
        ImGui::BeginDisabled(find.results().empty());
        if (ImGui::Button(ICON_FA_RIGHT_LEFT "  Replace All", ImVec2(-1.0f, 0.0f)))
            find.replaceAll();
        ImGui::EndDisabled();
        registerLastItem("search:replace-all", "button", "Replace All", [&find]() { find.replaceAll(); }, !find.results().empty());
    }
    if (run)
        find.search();

    // Summary.
    if (!find.error.get().empty())
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(palette_.severityError), "%s", find.error.get().c_str());
    else if (find.running.get())
        ImGui::TextDisabled(ICON_FA_SPINNER "  Searching...");
    else if (!find.query.empty() && find.generation() > 0)
        ImGui::TextDisabled("%d result(s) in %d file(s)%s, %d file(s) searched", find.hitCount(), int(find.results().size()),
                            find.truncated() ? " (stopped: too many)" : "", find.filesSearched());
    else
        ImGui::TextDisabled("Searches every file of the workspace (open files in their current text).");

    // Results: file -> lines, the match highlighted.
    ImGui::BeginChild("##results", ImVec2(0.0f, 0.0f));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (const auto& file : find.results()) {
        ImGui::PushID(file.path.c_str());
        const bool open = !collapsed_.count(file.path);
        const std::string header = std::string(open ? ICON_FA_CHEVRON_DOWN "  " : ICON_FA_CHEVRON_RIGHT "  ") + file.path;
        if (ImGui::Selectable(header.c_str(), false))
            open ? (void)collapsed_.insert(file.path) : (void)collapsed_.erase(file.path);
        ImGui::SameLine();
        ImGui::TextDisabled("%d", int(file.hits.size()));
        if (open) {
            ImGui::PushFont(fonts_.mono, Fonts::kEditorSize * 0.9f);
            for (size_t k = 0; k < file.hits.size(); ++k) {
                const auto& hit = file.hits[k];
                ImGui::PushID(int(k));
                // Leading blanks are dropped so matches line up; the match is drawn highlighted.
                size_t lead = 0;
                while (lead < hit.text.size() && (hit.text[lead] == ' ' || hit.text[lead] == '\t'))
                    ++lead;
                const std::string number = std::to_string(hit.line + 1);
                const ImVec2 rowMin = ImGui::GetCursorScreenPos();
                if (ImGui::Selectable("##hit", false, 0, ImVec2(0.0f, ImGui::GetTextLineHeight())))
                    find.open(file.path, hit);
                registerLastItem("search:result:" + file.path + ":" + number, "button", file.path + ":" + number,
                                 [&find, path = file.path, hit]() { find.open(path, hit); });
                float x = rowMin.x + 22.0f * s_;
                dl->AddText(ImVec2(x, rowMin.y), palette_.gutterText, number.c_str());
                x += ImGui::CalcTextSize("00000").x;
                const std::string before = hit.text.substr(lead, size_t(std::max(0, hit.column - int(lead))));
                const std::string match = hit.text.substr(size_t(std::min<int>(hit.column, int(hit.text.size()))), size_t(hit.length));
                const std::string after = size_t(hit.column + hit.length) < hit.text.size() ? hit.text.substr(size_t(hit.column + hit.length)) : "";
                dl->AddText(ImVec2(x, rowMin.y), palette_.textDim, before.c_str());
                x += ImGui::CalcTextSize(before.c_str()).x;
                const float mw = ImGui::CalcTextSize(match.c_str()).x;
                dl->AddRectFilled(ImVec2(x - 1.0f, rowMin.y), ImVec2(x + mw + 1.0f, rowMin.y + ImGui::GetTextLineHeight()),
                                  palette_.findMatch, 2.0f * s_);
                dl->AddText(ImVec2(x, rowMin.y), palette_.text, match.c_str());
                x += mw;
                dl->AddText(ImVec2(x, rowMin.y), palette_.textDim, after.c_str());
                ImGui::PopID();
            }
            ImGui::PopFont();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::PopID();
}

} // namespace enp::view
