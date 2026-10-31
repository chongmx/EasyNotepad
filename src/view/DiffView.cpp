#include "enp/view/DiffView.h"

#include "enp/view/PanelFrame.h"

#include <algorithm>
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
                          const Palette& palette, float zoom, float uiScale, ImVec2 size, std::vector<bool>* chosen)
{
    static int anchor = -1; // Shift-click ranges start here
    if (chosen)
        chosen->resize(rows.size(), false);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    panel::beginChild(id, size, palette.editorBg, ImGuiChildFlags_None, ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar();

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
            if (chosen && row.kind != model::DiffKind::Same) {
                ImGui::PushID(i);
                ImGui::InvisibleButton("##row", ImVec2(width, lineH));
                if (ImGui::IsItemClicked()) {
                    std::vector<bool>& c = *chosen;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        // The whole change: every adjacent changed row.
                        int a = i, b = i;
                        while (a > 0 && rows[size_t(a - 1)].kind != model::DiffKind::Same)
                            --a;
                        while (b + 1 < int(rows.size()) && rows[size_t(b + 1)].kind != model::DiffKind::Same)
                            ++b;
                        for (int k = a; k <= b; ++k)
                            c[size_t(k)] = true;
                    } else if (ImGui::GetIO().KeyShift && anchor >= 0 && anchor < int(rows.size())) {
                        for (int k = std::min(anchor, i); k <= std::max(anchor, i); ++k)
                            c[size_t(k)] = rows[size_t(k)].kind != model::DiffKind::Same;
                    } else {
                        c[size_t(i)] = !c[size_t(i)];
                        anchor = i;
                    }
                }
                if (ImGui::IsItemHovered())
                    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + lineH), palette.hover);
                if ((*chosen)[size_t(i)]) {
                    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + lineH), palette.accentSoft);
                    dl->AddRectFilled(p, ImVec2(p.x + 3.0f * uiScale, p.y + lineH), palette.accent);
                }
                ImGui::SetItemTooltip("Click to choose this line, Shift-click a range, double-click the whole change");
                ImGui::PopID();
            } else
                ImGui::Dummy(ImVec2(width, lineH));
        }
    }
    const ImVec2 winPos = ImGui::GetWindowPos();
    dl->AddLine(ImVec2(winPos.x + half, winPos.y), ImVec2(winPos.x + half, winPos.y + ImGui::GetWindowHeight()), palette.border);
    ImGui::PopFont();
    ImGui::EndChild();
}

} // namespace enp::view
