#include "enp/view/DiffView.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace enp::view {

namespace {

// Tabs are shown as four spaces; diffs are read, not edited.
std::string expandTabs(const std::string& s)
{
    std::string out;
    for (char c : s) {
        if (c == '\t')
            out.append(4 - out.size() % 4, ' ');
        else
            out += c;
    }
    return out;
}

void drawSide(ImDrawList* dl, ImVec2 min, ImVec2 max, int line, const std::string& text, char sign, ImU32 bg,
              ImU32 signColor, const Palette& p, float numberW)
{
    if (bg)
        dl->AddRectFilled(min, max, bg);
    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize();
    const float y = min.y + (max.y - min.y - size) * 0.5f;
    dl->PushClipRect(min, max, true);
    if (line >= 0) {
        char number[16];
        std::snprintf(number, sizeof(number), "%d", line + 1);
        const float w = font->CalcTextSizeA(size, FLT_MAX, 0.0f, number).x;
        dl->AddText(font, size, ImVec2(min.x + numberW - w, y), p.gutterText, number);
        if (sign != ' ') {
            const char signText[2] = {sign, 0};
            dl->AddText(font, size, ImVec2(min.x + numberW + size * 0.4f, y), signColor, signText);
        }
        dl->AddText(font, size, ImVec2(min.x + numberW + size * 1.4f, y), p.synText, expandTabs(text).c_str());
    }
    dl->PopClipRect();
}

} // namespace

void renderSideBySideDiff(const char* id, const std::vector<model::DiffRow>& rows, const Fonts& fonts,
                          const Palette& palette, float zoom, float uiScale, ImVec2 size)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, palette.editorBg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild(id, size, ImGuiChildFlags_None, ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    if (rows.empty()) {
        ImGui::SetCursorPos(ImVec2(12.0f * uiScale, 10.0f * uiScale));
        ImGui::TextDisabled("Nothing to compare.");
        ImGui::EndChild();
        return;
    }

    ImGui::PushFont(fonts.mono, Fonts::kEditorSize * zoom);
    const float lineH = std::floor(ImGui::GetFontSize() * 1.4f);
    const float numberW = ImGui::CalcTextSize("0000").x;
    const float width = ImGui::GetContentRegionAvail().x;
    const float half = std::floor(width * 0.5f);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImGuiListClipper clipper;
    clipper.Begin(int(rows.size()), lineH);
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const model::DiffRow& row = rows[size_t(i)];
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const bool removed = row.kind == model::DiffKind::Removed || row.kind == model::DiffKind::Changed;
            const bool added = row.kind == model::DiffKind::Added || row.kind == model::DiffKind::Changed;
            drawSide(dl, p, ImVec2(p.x + half - 1.0f, p.y + lineH), row.leftLine, row.leftText, removed ? '-' : ' ',
                     removed ? palette.diffRemovedBg : (row.leftLine < 0 ? palette.hover : 0), palette.diffRemovedText,
                     palette, numberW);
            drawSide(dl, ImVec2(p.x + half + 1.0f, p.y), ImVec2(p.x + width, p.y + lineH), row.rightLine, row.rightText,
                     added ? '+' : ' ', added ? palette.diffAddedBg : (row.rightLine < 0 ? palette.hover : 0),
                     palette.diffAddedText, palette, numberW);
            ImGui::Dummy(ImVec2(width, lineH));
        }
    }
    const ImVec2 winPos = ImGui::GetWindowPos();
    dl->AddLine(ImVec2(winPos.x + half, winPos.y), ImVec2(winPos.x + half, winPos.y + ImGui::GetWindowHeight()), palette.border);
    ImGui::PopFont();
    ImGui::EndChild();
}

} // namespace enp::view
