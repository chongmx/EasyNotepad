#include "enp/view/EditorView.h"

#include "enp/view/PanelFrame.h"

#include "enp/model/Highlighter.h"
#include "enp/view/SyntaxColors.h"
#include "enp/view/UiAutomation.h"

#include "IconsFontAwesome6.h"
#include "misc/cpp/imgui_stdlib.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <ctime>
#include <unordered_set>

namespace enp::view {

using model::TextPos;
using viewmodel::CommandId;

namespace {

int tabStop = 4; // of the document being drawn (its indent width); set by render() and renderMinimap()
constexpr float kLineSpacing = 1.45f;
constexpr float kTopPadding = 4.0f;

// ---- Syntax colouring -------------------------------------------------------------------------

bool isIdent(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }
bool isWordChar(char c) { return isIdent(c) || (static_cast<unsigned char>(c) & 0x80) != 0; }
bool isBlank(char c) { return c == ' ' || c == '\t'; }

// ---- Columns ----------------------------------------------------------------------------------

int visualColumn(const std::string& s, int byteColumn)
{
    int v = 0;
    const int end = std::min(byteColumn, int(s.size()));
    for (int i = 0; i < end; ++i) {
        if (s[size_t(i)] == '\t')
            v = (v / tabStop + 1) * tabStop;
        else if (!model::isUtf8Continuation(s[size_t(i)]))
            ++v;
    }
    return v;
}

void appendUtf8(std::string& out, unsigned c)
{
    if (c < 0x80) {
        out += char(c);
    } else if (c < 0x800) {
        out += char(0xC0 | (c >> 6));
        out += char(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += char(0xE0 | (c >> 12));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    } else {
        out += char(0xF0 | (c >> 18));
        out += char(0x80 | ((c >> 12) & 0x3F));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    }
}

int nextBoundary(const std::string& s, int i)
{
    ++i;
    while (i < int(s.size()) && model::isUtf8Continuation(s[size_t(i)]))
        ++i;
    return i;
}

TextPos wordLeft(const model::TextBuffer& buffer, TextPos pos)
{
    if (pos.column == 0)
        return buffer.prev(pos);
    const std::string& l = buffer.line(pos.line);
    int c = pos.column;
    while (c > 0 && isBlank(l[size_t(c - 1)]))
        --c;
    if (c > 0 && isWordChar(l[size_t(c - 1)])) {
        while (c > 0 && isWordChar(l[size_t(c - 1)]))
            --c;
    } else {
        while (c > 0 && !isWordChar(l[size_t(c - 1)]) && !isBlank(l[size_t(c - 1)]))
            --c;
    }
    return {pos.line, c};
}

TextPos wordRight(const model::TextBuffer& buffer, TextPos pos)
{
    const std::string& l = buffer.line(pos.line);
    if (pos.column >= int(l.size()))
        return buffer.next(pos);
    int c = pos.column;
    if (isWordChar(l[size_t(c)])) {
        while (c < int(l.size()) && isWordChar(l[size_t(c)]))
            ++c;
    } else if (!isBlank(l[size_t(c)])) {
        while (c < int(l.size()) && !isWordChar(l[size_t(c)]) && !isBlank(l[size_t(c)]))
            ++c;
    }
    while (c < int(l.size()) && isBlank(l[size_t(c)]))
        ++c;
    return {pos.line, c};
}

ImU32 severityColor(model::Severity severity, const Palette& p)
{
    switch (severity) {
    case model::Severity::Error: return p.severityError;
    case model::Severity::Warning: return p.severityWarning;
    case model::Severity::Info: break;
    }
    return p.severityInfo;
}

void squiggle(ImDrawList* dl, float x0, float x1, float y, ImU32 color, float scale)
{
    const float step = 3.0f * scale;
    ImVec2 prev(x0, y);
    bool up = true;
    for (float x = x0 + step; x <= x1 + 0.5f; x += step, up = !up) {
        const ImVec2 next(x, y + (up ? -step * 0.6f : 0.0f));
        dl->AddLine(prev, next, color, 1.0f * scale);
        prev = next;
    }
}

} // namespace

EditorView::EditorView(viewmodel::NotepadViewModel& vm, const Fonts& fonts, const Palette& palette, float uiScale)
    : vm_(vm), fonts_(fonts), palette_(palette), s_(uiScale), layers_(vm, fonts, palette, uiScale)
{
    subscriptions_.push_back(vm_.caretRevealRequested.subscribe([this]() { revealCaret_ = true; }));
    subscriptions_.push_back(vm_.focusFindRequested.subscribe([this]() { focusFind_ = true; }));
    subscriptions_.push_back(vm_.activeIndex.subscribe([this](const int&) {
        focusRequested_ = true;
        preferredVcol_ = -1.0f;
    }));
    subscriptions_.push_back(vm_.findVisible.subscribe([this](const bool& visible) {
        if (!visible)
            focusRequested_ = true;
    }));
}

// ---- Layout -------------------------------------------------------------------------------------

void EditorView::buildRows(const model::Document& doc, int wrapColumns)
{
    rows_.clear();
    const int lines = doc.buffer.lineCount();
    firstRowOfLine_.assign(size_t(lines), 0);
    maxColumns_ = 0;

    // Folded regions: their lines are hidden behind one summary row after the header.
    std::vector<const model::OutlineNode*> foldAt(size_t(lines), nullptr);
    if (!doc.folded.empty())
        for (const model::OutlineNode& n : vm_.outline())
            if (vm_.isFolded(n) && n.endLine < lines && !foldAt[size_t(n.startLine)])
                foldAt[size_t(n.startLine)] = &n; // outermost wins (parents come first)
    int hiddenUntil = -1;
    int summaryRow = 0;

    for (int l = 0; l < lines; ++l) {
        if (l <= hiddenUntil) {
            firstRowOfLine_[size_t(l)] = summaryRow;
            continue;
        }
        const std::string& s = doc.buffer.line(l);
        firstRowOfLine_[size_t(l)] = int(rows_.size());
        if (const model::OutlineNode* fold = foldAt[size_t(l)]) {
            // Header (unwrapped) followed by the summary row.
            rows_.push_back({l, 0, int(s.size())});
            maxColumns_ = std::max(maxColumns_, visualColumn(s, int(s.size())));
            summaryRow = int(rows_.size());
            rows_.push_back({fold->endLine, 0, 0, fold->startLine});
            hiddenUntil = fold->endLine;
            continue;
        }
        const int total = visualColumn(s, int(s.size()));
        maxColumns_ = std::max(maxColumns_, total);
        if (wrapColumns <= 0 || total <= wrapColumns) {
            rows_.push_back({l, 0, int(s.size())});
            continue;
        }
        int begin = 0;
        while (begin < int(s.size())) {
            const int v0 = visualColumn(s, begin);
            int v = v0, end = begin, lastBreak = -1;
            while (end < int(s.size())) {
                const int next = nextBoundary(s, end);
                const int nv = s[size_t(end)] == '\t' ? (v / tabStop + 1) * tabStop : v + 1;
                if (nv - v0 > wrapColumns)
                    break;
                if (s[size_t(end)] == ' ')
                    lastBreak = next;
                v = nv;
                end = next;
            }
            if (end < int(s.size()) && lastBreak > begin)
                end = lastBreak;
            if (end == begin)
                end = nextBoundary(s, begin);
            rows_.push_back({l, begin, end});
            begin = end;
        }
    }
}

int EditorView::rowOf(TextPos pos) const
{
    if (rows_.empty())
        return 0;
    const int line = std::clamp(pos.line, 0, int(firstRowOfLine_.size()) - 1);
    const int first = firstRowOfLine_[size_t(line)];
    const int last = (line + 1 < int(firstRowOfLine_.size()) ? firstRowOfLine_[size_t(line + 1)] : int(rows_.size())) - 1;
    for (int r = first; r < last; ++r)
        if (pos.column < rows_[size_t(r)].end)
            return r;
    return last;
}

float EditorView::rowX(const model::Document& doc, const Row& row, int column, const Metrics& m) const
{
    const std::string& s = doc.buffer.line(row.line);
    return m.origin.x + float(visualColumn(s, column) - visualColumn(s, row.begin)) * m.charW;
}

int EditorView::columnAtX(const model::Document& doc, const Row& row, float vcol) const
{
    const std::string& s = doc.buffer.line(row.line);
    int v = visualColumn(s, row.begin);
    const float target = float(v) + vcol;
    int i = row.begin;
    while (i < row.end) {
        const int next = nextBoundary(s, i);
        const int nv = s[size_t(i)] == '\t' ? (v / tabStop + 1) * tabStop : v + 1;
        if (target < (float(v) + float(nv)) * 0.5f)
            return i;
        v = nv;
        i = next;
    }
    return row.end;
}

int EditorView::rowAt(ImVec2 mouse, const Metrics& m) const
{
    return std::clamp(int(std::floor((mouse.y - m.origin.y) / m.lineH)), 0, std::max(0, int(rows_.size()) - 1));
}

TextPos EditorView::hitTest(const model::Document& doc, ImVec2 mouse, const Metrics& m) const
{
    if (rows_.empty())
        return {};
    const Row& row = rows_[size_t(rowAt(mouse, m))];
    if (row.fold >= 0)
        return {row.line, 0};
    return {row.line, columnAtX(doc, row, (mouse.x - m.origin.x) / m.charW)};
}

// ---- Frame --------------------------------------------------------------------------------------

void EditorView::render(ImVec2 size)
{
    model::Document* doc = vm_.activeDocument();
    if (!doc) {
        renderEmptyState(size);
        return;
    }
    tabStop = std::max(1, doc->indent.width);
    if (doc->diskState != model::Document::DiskState::InSync) {
        const float before = ImGui::GetCursorPosY();
        renderDiskBar(*doc);
        size.y = std::max(1.0f, size.y - (ImGui::GetCursorPosY() - before));
    }
    updateConflicts(*doc);
    if (!conflicts_.empty()) {
        const float before = ImGui::GetCursorPosY();
        renderConflictBar(*doc);
        size.y = std::max(1.0f, size.y - (ImGui::GetCursorPosY() - before));
    }
    if (vm_.findVisible.get()) {
        const float before = ImGui::GetCursorPosY();
        renderFindBar();
        size.y = std::max(1.0f, size.y - (ImGui::GetCursorPosY() - before));
    }

    ImGui::PushFont(fonts_.mono, Fonts::kEditorSize * vm_.zoom.get());
    Metrics m{};
    m.charW = ImGui::CalcTextSize("M").x;
    m.lineH = std::floor(ImGui::GetFontSize() * kLineSpacing);
    int digits = 1;
    for (int n = doc->buffer.lineCount(); n >= 10; n /= 10)
        ++digits;
    m.markerW = 18.0f * s_;
    m.foldW = 16.0f * s_;
    // Git: change bars for files of a real repository, and the blame column when it is on.
    const bool gitFile = doc->onDisk && !vm_.git().demo() && vm_.git().repo().repository;
    m.changeW = gitFile ? 7.0f * s_ : 0.0f;
    blameColumns_ = std::clamp(blameColumns_, 9.0f, 60.0f);
    m.blameW = gitFile && vm_.git().blameShown(doc->path) ? blameColumns_ * m.charW * 0.85f : 0.0f;
    m.gutterW = m.markerW + m.blameW + m.changeW + m.foldW +
                (vm_.showLineNumbers.get() ? float(std::max(digits, 3)) * m.charW + 6.0f * s_ : 0.0f);
    const bool sideBar = vm_.showLayersMap.get();
    if (sideBarWidth_ <= 0.0f)
        sideBarWidth_ = 280.0f * s_;
    sideBarWidth_ = std::clamp(sideBarWidth_, 190.0f * s_, std::max(190.0f * s_, size.x * 0.55f));
    const float sideW = sideBar ? sideBarWidth_ : 0.0f;
    const ImVec2 sideSize(sideW, size.y);
    size.x -= sideW;

    const bool wrap = vm_.wordWrap.get();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const std::string childId = "##editor" + std::to_string(doc->id);
    panel::beginChild(childId.c_str(), size, palette_.editorBg, ImGuiChildFlags_None,
                      (wrap ? ImGuiWindowFlags_None : ImGuiWindowFlags_HorizontalScrollbar) | ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar();
    if (focusRequested_ && !vm_.findVisible.get()) {
        ImGui::SetWindowFocus();
        focusRequested_ = false;
    }
    focused_ = ImGui::IsWindowFocused();

    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 winPos = ImGui::GetWindowPos();
    const float viewW = ImGui::GetWindowWidth() - (ImGui::GetScrollMaxY() > 0.0f ? style.ScrollbarSize : 0.0f);
    const float viewH = ImGui::GetWindowHeight() - (ImGui::GetScrollMaxX() > 0.0f ? style.ScrollbarSize : 0.0f);
    m.viewMin = winPos;
    m.viewMax = ImVec2(winPos.x + viewW, winPos.y + viewH);
    m.origin = ImVec2(winPos.x + m.gutterW + 6.0f * s_ - ImGui::GetScrollX(), winPos.y + kTopPadding * s_ - ImGui::GetScrollY());
    m.visibleRows = std::max(1, int(viewH / m.lineH));
    const int wrapColumns = wrap ? std::max(8, int((viewW - m.gutterW - 12.0f * s_) / m.charW)) : 0;

    buildRows(*doc, wrapColumns);
    const bool hovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(m.viewMin, m.viewMax);
    handleMouse(*doc, m, hovered);
    if (focused_)
        handleKeyboard(*doc, m);

    doc = vm_.activeDocument(); // input may have edited the text
    buildRows(*doc, wrapColumns);
    if (pendingScrollLine_ >= 0) { // from the minimap: centre that line
        const float rowY = float(rowOf({pendingScrollLine_, 0})) * m.lineH;
        ImGui::SetScrollY(std::max(0.0f, rowY - viewH * 0.5f));
        pendingScrollLine_ = -1;
    }
    if (revealCaret_)
        revealCaret(*doc, m);

    const int firstRow = std::clamp(int((ImGui::GetScrollY() - kTopPadding * s_) / m.lineH), 0, int(rows_.size()) - 1);
    const int lastRow = std::min(int(rows_.size()) - 1, firstRow + m.visibleRows + 1);
    if (!rows_.empty()) {
        const Row& first = rows_[size_t(firstRow)];
        firstVisibleLine_ = first.fold >= 0 ? first.fold : first.line;
        lastVisibleLine_ = rows_[size_t(std::min(lastRow, int(rows_.size()) - 1))].line;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(ImVec2(winPos.x + m.gutterW, winPos.y), m.viewMax, true);
    if (!conflicts_.empty() && !rows_.empty())
        drawConflictBackgrounds(m, firstRow, lastRow);
    drawRows(*doc, m, firstRow, lastRow);
    dl->PopClipRect();
    drawGutter(*doc, m, firstRow, lastRow);
    if (m.blameW > 0.0f && !rows_.empty())
        drawBlame(*doc, m, firstRow, lastRow);
    if (!conflicts_.empty() && !rows_.empty())
        drawConflictActions(*doc, m, firstRow, lastRow);
    if (focused_ && !conflicts_.empty() && ImGui::IsKeyPressed(ImGuiKey_F8))
        jumpToConflict(*doc, ImGui::GetIO().KeyShift ? -1 : 1);
    if (hovered && !dragging_)
        drawHoverTooltip(*doc, m);

    // Scrollable extent: every row plus half a page of slack below the last line.
    const float contentW = wrap ? 1.0f : m.gutterW + 6.0f * s_ + float(maxColumns_ + 4) * m.charW;
    ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
    ImGui::Dummy(ImVec2(contentW, float(rows_.size()) * m.lineH + viewH * 0.5f));
    ImGui::EndChild();
    ImGui::PopFont();
    renderChangePeek(*vm_.activeDocument());
    if (sideBar) {
        ImGui::SameLine(0.0f, 0.0f);
        renderSideBar(*vm_.activeDocument(), sideSize);
    }
}

void EditorView::renderEmptyState(ImVec2 size)
{
    panel::beginChild("##editorEmpty", size, palette_.editorBg);
    const char* text = "No document open";
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    ImGui::SetCursorPos(ImVec2((size.x - textSize.x) * 0.5f, size.y * 0.4f));
    ImGui::TextDisabled("%s", text);
    const float buttonsW = 220.0f * s_;
    ImGui::SetCursorPosX((size.x - buttonsW) * 0.5f);
    if (ImGui::Button(ICON_FA_FILE_CIRCLE_PLUS "  New", ImVec2(buttonsW * 0.48f, 0.0f)))
        vm_.execute(CommandId::NewFile);
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_BOOK_OPEN "  Welcome", ImVec2(buttonsW * 0.48f, 0.0f)))
        vm_.openFile("Welcome.md");
    ImGui::EndChild();
}

void EditorView::renderDiskBar(model::Document& doc)
{
    // The file changed (or vanished) on disk while it has unsaved edits here.
    const bool deleted = doc.diskState == model::Document::DiskState::Deleted;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * s_, 5.0f * s_));
    panel::beginChild("##diskBar", ImVec2(0.0f, ImGui::GetFrameHeight() + 10.0f * s_), (palette_.severityWarning & ~IM_COL32_A_MASK) | (ImU32(40) << IM_COL32_A_SHIFT),
                      ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(palette_.severityWarning), ICON_FA_TRIANGLE_EXCLAMATION);
    ImGui::SameLine();
    ImGui::TextUnformatted(deleted ? "This file was deleted on disk. Saving recreates it."
                                   : "This file changed on disk, and you have unsaved changes here.");
    ImGui::SameLine();
    if (deleted) {
        if (ImGui::SmallButton("Keep editing"))
            vm_.keepMine(doc);
        registerLastItem("disk-bar:keep", "button", "Keep editing", [this, &doc]() { vm_.keepMine(doc); });
    } else {
        if (ImGui::SmallButton(ICON_FA_ROTATE "  Reload"))
            vm_.reloadFromDisk(doc);
        registerLastItem("disk-bar:reload", "button", "Reload", [this, &doc]() { vm_.reloadFromDisk(doc); });
        ImGui::SameLine();
        if (ImGui::SmallButton("Keep mine"))
            vm_.keepMine(doc);
        registerLastItem("disk-bar:keep", "button", "Keep mine", [this, &doc]() { vm_.keepMine(doc); });
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_CODE_COMPARE "  Compare"))
            vm_.compareWithDisk(doc);
        registerLastItem("disk-bar:compare", "button", "Compare", [this, &doc]() { vm_.compareWithDisk(doc); });
    }
    ImGui::EndChild();
}

void EditorView::renderFindBar()
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * s_, 5.0f * s_));
    const float rows = vm_.replaceVisible.get() ? 2.0f : 1.0f;
    const float h = rows * ImGui::GetFrameHeightWithSpacing() - ImGui::GetStyle().ItemSpacing.y + 10.0f * s_;
    panel::beginChild("##findBar", ImVec2(0.0f, h), palette_.panelHeaderBg, ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();

    const float inputW = 280.0f * s_;
    bool findActive = false;
    if (focusFind_) {
        ImGui::SetKeyboardFocusHere();
        focusFind_ = false;
    }
    if (findBuffer_ != vm_.findQuery.get() && !ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows))
        findBuffer_ = vm_.findQuery.get();
    ImGui::SetNextItemWidth(inputW);
    if (ImGui::InputTextWithHint("##find", ICON_FA_MAGNIFYING_GLASS "  Find", &findBuffer_, ImGuiInputTextFlags_EnterReturnsTrue)) {
        vm_.findNext(ImGui::GetIO().KeyShift ? -1 : +1);
        ImGui::SetKeyboardFocusHere(-1);
    }
    if (ImGui::IsItemEdited())
        vm_.findQuery.set(findBuffer_);
    findActive = ImGui::IsItemActive();

    ImGui::SameLine();
    const auto& matches = vm_.findMatches();
    if (!vm_.findQuery.get().empty()) {
        if (matches.empty())
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(palette_.severityError), "No results");
        else if (vm_.currentMatch.get() >= 0)
            ImGui::TextDisabled("%d of %d", vm_.currentMatch.get() + 1, int(matches.size()));
        else
            ImGui::TextDisabled("%d results", int(matches.size()));
        ImGui::SameLine();
    }
    if (ImGui::Button(ICON_FA_ARROW_UP))
        vm_.findNext(-1);
    ImGui::SetItemTooltip("Previous match (Shift+F3)");
    ImGui::SameLine(0.0f, 2.0f * s_);
    if (ImGui::Button(ICON_FA_ARROW_DOWN))
        vm_.findNext(+1);
    ImGui::SetItemTooltip("Next match (F3)");
    ImGui::SameLine();
    const bool matchCase = vm_.matchCase.get();
    if (matchCase)
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(palette_.accentSoft));
    if (ImGui::Button("Aa"))
        vm_.matchCase.set(!matchCase);
    if (matchCase)
        ImGui::PopStyleColor();
    ImGui::SetItemTooltip("Match case");
    ImGui::SameLine(0.0f, 2.0f * s_);
    if (ImGui::Button(ICON_FA_RIGHT_LEFT))
        vm_.replaceVisible.set(!vm_.replaceVisible.get());
    ImGui::SetItemTooltip("Toggle replace");
    ImGui::SameLine(0.0f, 2.0f * s_);
    if (ImGui::Button(ICON_FA_XMARK) || (findActive && ImGui::IsKeyPressed(ImGuiKey_Escape)))
        vm_.findVisible.set(false);
    ImGui::SetItemTooltip("Close (Esc)");

    if (vm_.replaceVisible.get()) {
        ImGui::SetNextItemWidth(inputW);
        if (replaceBuffer_ != vm_.replaceText.get() && !ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows))
            replaceBuffer_ = vm_.replaceText.get();
        if (ImGui::InputTextWithHint("##replace", ICON_FA_RIGHT_LEFT "  Replace", &replaceBuffer_))
            vm_.replaceText.set(replaceBuffer_);
        ImGui::SameLine();
        if (ImGui::Button("Replace"))
            vm_.replaceCurrent();
        ImGui::SameLine(0.0f, 2.0f * s_);
        ImGui::BeginDisabled(matches.empty());
        if (ImGui::Button("Replace All"))
            vm_.replaceAll();
        ImGui::EndDisabled();
    }
    ImGui::EndChild();
}

// ---- Input --------------------------------------------------------------------------------------

void EditorView::handleMouse(const model::Document& doc, const Metrics& m, bool hovered)
{
    ImGuiIO& io = ImGui::GetIO();
    const bool inGutter = io.MousePos.x < m.viewMin.x + m.gutterW;
    if (hovered && !inGutter)
        ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);

    const Row* hoveredRow = hovered && !rows_.empty() ? &rows_[size_t(rowAt(io.MousePos, m))] : nullptr;
    const bool inFoldColumn = inGutter && io.MousePos.x >= m.viewMin.x + m.gutterW - m.foldW;
    if (hoveredRow && (hoveredRow->fold >= 0 || (inFoldColumn && vm_.regionAt(hoveredRow->line))))
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        lastActivity_ = ImGui::GetTime();
        preferredVcol_ = -1.0f;
        const TextPos pos = hitTest(doc, io.MousePos, m);
        if (hoveredRow && hoveredRow->fold >= 0) { // a summary: expand it
            dragging_ = false;
            vm_.setFolded(hoveredRow->fold, false);
            return;
        }
        if (inFoldColumn) {
            dragging_ = false;
            if (vm_.regionAt(pos.line))
                vm_.toggleFold(pos.line);
            return;
        }
        const float changeX = m.viewMin.x + m.gutterW - m.foldW - m.changeW;
        if (inGutter && m.changeW > 0.0f && io.MousePos.x >= changeX && io.MousePos.x < changeX + m.changeW) {
            dragging_ = false;
            int first = 0, last = 0;
            std::vector<std::string> head;
            if (vm_.git().changeAt(doc.path, pos.line, first, last, head)) {
                peekLine_ = pos.line;
                openPeek_ = true;
            }
            return;
        }
        if (inGutter && m.blameW > 0.0f && io.MousePos.x >= m.viewMin.x + m.markerW &&
            io.MousePos.x < m.viewMin.x + m.markerW + m.blameW + 4.0f * s_) {
            dragging_ = false;
            return;
        }
        if (inGutter) {
            dragging_ = false;
            if (io.MousePos.x < m.viewMin.x + m.markerW) {
                vm_.setCaret({pos.line, 0}, false);
                vm_.execute(CommandId::ToggleBookmark);
            } else {
                const int lines = doc.buffer.lineCount();
                const TextPos end = pos.line + 1 < lines ? TextPos{pos.line + 1, 0}
                                                         : TextPos{pos.line, int(doc.buffer.line(pos.line).size())};
                vm_.setSelection({pos.line, 0}, end);
            }
        } else if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            dragging_ = false;
            vm_.setSelection(doc.buffer.wordStart(pos), doc.buffer.wordEnd(pos));
        } else {
            vm_.setCaret(pos, io.KeyShift);
            dragging_ = true;
        }
    }
    if (!dragging_)
        return;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        dragging_ = false;
        return;
    }
    vm_.setCaret(hitTest(doc, io.MousePos, m), true);
    // Auto-scroll while dragging past the edges.
    if (io.MousePos.y < m.viewMin.y)
        ImGui::SetScrollY(ImGui::GetScrollY() - m.lineH);
    else if (io.MousePos.y > m.viewMax.y)
        ImGui::SetScrollY(ImGui::GetScrollY() + m.lineH);
    if (io.MousePos.x > m.viewMax.x)
        ImGui::SetScrollX(ImGui::GetScrollX() + m.charW * 2.0f);
    else if (io.MousePos.x < m.viewMin.x + m.gutterW && ImGui::GetScrollX() > 0.0f)
        ImGui::SetScrollX(std::max(0.0f, ImGui::GetScrollX() - m.charW * 2.0f));
}

void EditorView::moveVertical(const model::Document& doc, int delta, bool extend)
{
    const int r = rowOf(doc.caret);
    const Row& row = rows_[size_t(r)];
    const std::string& s = doc.buffer.line(row.line);
    if (preferredVcol_ < 0.0f)
        preferredVcol_ = float(visualColumn(s, doc.caret.column) - visualColumn(s, row.begin));
    const int target = std::clamp(r + delta, 0, int(rows_.size()) - 1);
    const Row& t = rows_[size_t(target)];
    vm_.setCaret({t.line, columnAtX(doc, t, preferredVcol_)}, extend);
}

void EditorView::handleKeyboard(const model::Document& doc, const Metrics& m)
{
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput)
        return;
    const bool shift = io.KeyShift;
    const bool ctrl = io.KeyCtrl;
    const TextPos caret = doc.caret;
    const model::TextBuffer& buffer = doc.buffer;
    auto pressed = [](ImGuiKey key) { return ImGui::IsKeyPressed(key, true); };
    bool acted = false;
    bool horizontal = false;

    if (pressed(ImGuiKey_LeftArrow)) {
        vm_.setCaret(!shift && doc.hasSelection() ? doc.selectionStart() : ctrl ? wordLeft(buffer, caret) : buffer.prev(caret), shift);
        acted = horizontal = true;
    }
    if (pressed(ImGuiKey_RightArrow)) {
        vm_.setCaret(!shift && doc.hasSelection() ? doc.selectionEnd() : ctrl ? wordRight(buffer, caret) : buffer.next(caret), shift);
        acted = horizontal = true;
    }
    if (!(ctrl && shift)) { // Ctrl+Shift+Up/Down move lines (global shortcut)
        if (pressed(ImGuiKey_UpArrow)) {
            if (ctrl)
                ImGui::SetScrollY(ImGui::GetScrollY() - m.lineH);
            else
                moveVertical(doc, -1, shift);
            acted = true;
        }
        if (pressed(ImGuiKey_DownArrow)) {
            if (ctrl)
                ImGui::SetScrollY(ImGui::GetScrollY() + m.lineH);
            else
                moveVertical(doc, +1, shift);
            acted = true;
        }
    }
    if (pressed(ImGuiKey_PageUp)) {
        moveVertical(doc, -m.visibleRows, shift);
        acted = true;
    }
    if (pressed(ImGuiKey_PageDown)) {
        moveVertical(doc, +m.visibleRows, shift);
        acted = true;
    }
    if (pressed(ImGuiKey_Home)) {
        if (ctrl) {
            vm_.setCaret({0, 0}, shift);
        } else { // smart home: first non-blank, then column 0
            const std::string& l = buffer.line(caret.line);
            int first = 0;
            while (first < int(l.size()) && isBlank(l[size_t(first)]))
                ++first;
            vm_.setCaret({caret.line, caret.column == first ? 0 : first}, shift);
        }
        acted = horizontal = true;
    }
    if (pressed(ImGuiKey_End)) {
        vm_.setCaret(ctrl ? buffer.end() : TextPos{caret.line, int(buffer.line(caret.line).size())}, shift);
        acted = horizontal = true;
    }
    if (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter)) {
        vm_.newline();
        acted = horizontal = true;
    }
    if (pressed(ImGuiKey_Backspace)) {
        if (ctrl && !doc.hasSelection())
            vm_.setSelection(caret, wordLeft(buffer, caret));
        vm_.backspace();
        acted = horizontal = true;
    }
    if (pressed(ImGuiKey_Delete)) {
        if (ctrl && !doc.hasSelection())
            vm_.setSelection(caret, wordRight(buffer, caret));
        vm_.deleteForward();
        acted = horizontal = true;
    }
    if (pressed(ImGuiKey_Tab) && !ctrl) {
        if (shift)
            vm_.execute(CommandId::Outdent);
        else if (doc.hasSelection() && doc.anchor.line != doc.caret.line)
            vm_.execute(CommandId::Indent);
        else
            vm_.insertIndent();
        acted = horizontal = true;
    }
    if (pressed(ImGuiKey_Escape)) {
        if (vm_.findVisible.get())
            vm_.findVisible.set(false);
        else if (doc.hasSelection())
            vm_.setCaret(caret, false);
    }

    if (!ctrl || io.KeyAlt) { // AltGr arrives as Ctrl+Alt on Windows
        std::string typed;
        for (ImWchar c : io.InputQueueCharacters) {
            if (c < 32 || c == 127)
                continue;
            appendUtf8(typed, unsigned(c));
        }
        if (!typed.empty()) {
            vm_.insertText(typed);
            acted = horizontal = true;
        }
    }

    if (acted) {
        lastActivity_ = ImGui::GetTime();
        revealCaret_ = true;
        if (horizontal)
            preferredVcol_ = -1.0f;
    }
}

void EditorView::revealCaret(const model::Document& doc, const Metrics& m)
{
    revealCaret_ = false;
    if (rows_.empty())
        return;
    const int r = rowOf(doc.caret);
    const float y = float(r) * m.lineH + kTopPadding * s_;
    const float viewH = m.viewMax.y - m.viewMin.y;
    const float scrollY = ImGui::GetScrollY();
    if (y < scrollY)
        ImGui::SetScrollY(std::max(0.0f, y - m.lineH));
    else if (y + m.lineH > scrollY + viewH)
        ImGui::SetScrollY(y + 2.0f * m.lineH - viewH);
    if (vm_.wordWrap.get())
        return;
    const float x = float(visualColumn(doc.buffer.line(doc.caret.line), doc.caret.column)) * m.charW;
    const float textW = m.viewMax.x - m.viewMin.x - m.gutterW - 6.0f * s_;
    const float scrollX = ImGui::GetScrollX();
    if (x < scrollX)
        ImGui::SetScrollX(std::max(0.0f, x - 4.0f * m.charW));
    else if (x > scrollX + textW - 2.0f * m.charW)
        ImGui::SetScrollX(x - textW + 6.0f * m.charW);
}

// ---- Drawing ------------------------------------------------------------------------------------

void EditorView::drawRows(const model::Document& doc, const Metrics& m, int firstRow, int lastRow)
{
    if (rows_.empty())
        return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize();
    const float textLeft = m.viewMin.x + m.gutterW;

    // Highlighter state (block comments, strings, fences) at the first visible line.
    int inBlock = 0;
    std::vector<model::Token> toks;
    const model::LanguageSpec& spec = model::languageSpec(doc.language);
    const SyntaxColors colors = resolveSyntaxColors(palette_, vm_, doc.language);
    const int firstLine = rows_[size_t(firstRow)].line;
    inBlock = doc.highlightCache.stateAt(doc.buffer, spec, firstLine); // cached: no re-scan from line 1

    const std::vector<const model::ReviewFinding*> findings = vm_.review().findingsForFile(doc.path);
    const auto& matches = vm_.findMatches();
    const int currentMatch = vm_.currentMatch.get();
    const TextPos selStart = doc.selectionStart();
    const TextPos selEnd = doc.selectionEnd();
    int lastLine = -1;

    for (int r = firstRow; r <= lastRow; ++r) {
        const Row& row = rows_[size_t(r)];
        const float y = m.origin.y + float(r) * m.lineH;
        if (row.fold >= 0) {
            // The line after the folded ones starts in the state the cache knows.
            inBlock = doc.highlightCache.stateAt(doc.buffer, spec, row.line + 1);
            lastLine = row.line;
            drawSummaryRow(doc, row, m, y);
            continue;
        }
        const std::string& text = doc.buffer.line(row.line);
        if (row.line != lastLine) {
            model::highlightLine(text, spec, inBlock, toks);
            lastLine = row.line;
        }
        const bool lastRowOfLine = r + 1 >= int(rows_.size()) || rows_[size_t(r + 1)].line != row.line;

        if (row.line == doc.caret.line)
            dl->AddRectFilled(ImVec2(textLeft, y), ImVec2(m.viewMax.x, y + m.lineH), palette_.currentLine);

        for (int i = 0; i < int(matches.size()); ++i) {
            const viewmodel::FindMatch& match = matches[size_t(i)];
            if (match.start.line != row.line || match.end.column <= row.begin || match.start.column >= row.end)
                continue;
            const float x0 = rowX(doc, row, std::max(match.start.column, row.begin), m);
            const float x1 = rowX(doc, row, std::min(match.end.column, row.end), m);
            dl->AddRectFilled(ImVec2(x0, y + 1.0f), ImVec2(x1, y + m.lineH - 1.0f),
                              i == currentMatch ? palette_.findMatchCurrent : palette_.findMatch, 2.0f * s_);
        }

        if (doc.hasSelection() && row.line >= selStart.line && row.line <= selEnd.line) {
            const int b = row.line == selStart.line ? std::max(selStart.column, row.begin) : row.begin;
            const int e = row.line == selEnd.line ? std::min(selEnd.column, row.end) : row.end;
            const bool pastEol = row.line < selEnd.line && lastRowOfLine;
            if (b < e || (b == e && pastEol && b >= row.begin))
                dl->AddRectFilled(ImVec2(rowX(doc, row, b, m), y),
                                  ImVec2(rowX(doc, row, e, m) + (pastEol ? m.charW * 0.6f : 0.0f), y + m.lineH), palette_.selection);
        }

        if (vm_.showIndentGuides.get() && row.begin == 0) {
            int lead = 0;
            while (lead < int(text.size()) && isBlank(text[size_t(lead)]))
                ++lead;
            if (lead < int(text.size())) {
                const int leadCols = visualColumn(text, lead);
                for (int k = tabStop; k < leadCols; k += tabStop) {
                    const float x = std::floor(m.origin.x + float(k) * m.charW) + 0.5f;
                    dl->AddLine(ImVec2(x, y), ImVec2(x, y + m.lineH), palette_.indentGuide);
                }
            }
        }

        // Text, in runs of equal colour, expanding tabs.
        const float textY = y + (m.lineH - fontSize) * 0.5f;
        const int rowStartCol = visualColumn(text, row.begin);
        int vcol = 0;
        for (int b = row.begin; b < row.end;) {
            const model::Token tok = toks[size_t(b)];
            int e = b + 1;
            while (e < row.end && toks[size_t(e)] == tok && text[size_t(e)] != '\t')
                ++e;
            if (text[size_t(b)] == '\t') {
                const int abs = rowStartCol + vcol;
                const int next = (abs / tabStop + 1) * tabStop;
                if (vm_.showWhitespace.get()) {
                    const float x0 = m.origin.x + float(vcol) * m.charW + 2.0f * s_;
                    const float x1 = m.origin.x + float(next - rowStartCol) * m.charW - 2.0f * s_;
                    const float cy = y + m.lineH * 0.5f;
                    dl->AddLine(ImVec2(x0, cy), ImVec2(x1, cy), palette_.whitespace);
                    dl->AddLine(ImVec2(x1 - 3.0f * s_, cy - 3.0f * s_), ImVec2(x1, cy), palette_.whitespace);
                }
                vcol = next - rowStartCol;
                b = e;
                continue;
            }
            dl->AddText(font, fontSize, ImVec2(m.origin.x + float(vcol) * m.charW, textY), colors[size_t(tok)],
                        text.data() + b, text.data() + e);
            if (vm_.showWhitespace.get()) {
                int v = vcol;
                for (int i = b; i < e; i = nextBoundary(text, i), ++v)
                    if (text[size_t(i)] == ' ')
                        dl->AddCircleFilled(ImVec2(m.origin.x + (float(v) + 0.5f) * m.charW, y + m.lineH * 0.5f),
                                            1.2f * s_, palette_.whitespace);
            }
            vcol += model::codePointCount(std::string_view(text).substr(size_t(b), size_t(e - b)));
            b = e;
        }

        for (const model::ReviewFinding* f : findings) {
            if (f->line != row.line)
                continue;
            int b = row.begin;
            while (b < row.end && isBlank(text[size_t(b)]))
                ++b;
            float x0 = rowX(doc, row, b, m);
            float x1 = rowX(doc, row, row.end, m);
            if (x1 - x0 < m.charW * 2.0f)
                x1 = x0 + m.charW * 2.0f;
            squiggle(dl, x0, x1, y + m.lineH - 2.0f * s_, severityColor(f->severity, palette_), s_);
        }
    }

    // Caret
    const bool blinkOn = !vm_.blinkCaret.get() || std::fmod(ImGui::GetTime() - lastActivity_, 1.0) < 0.6;
    if (focused_ && blinkOn) {
        const int r = rowOf(doc.caret);
        if (r >= firstRow && r <= lastRow) {
            const Row& caretRow = rows_[size_t(r)];
            const float x = caretRow.fold >= 0 ? m.origin.x : std::floor(rowX(doc, caretRow, doc.caret.column, m));
            const float y = m.origin.y + float(r) * m.lineH;
            dl->AddRectFilled(ImVec2(x, y + 2.0f * s_), ImVec2(x + std::max(1.5f, 2.0f * s_), y + m.lineH - 2.0f * s_), palette_.caret);
        }
    }
}

void EditorView::drawGutter(const model::Document& doc, const Metrics& m, int firstRow, int lastRow)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize();
    dl->AddRectFilled(m.viewMin, ImVec2(m.viewMin.x + m.gutterW, m.viewMax.y), palette_.gutterBg);
    dl->AddLine(ImVec2(m.viewMin.x + m.gutterW - 1.0f, m.viewMin.y), ImVec2(m.viewMin.x + m.gutterW - 1.0f, m.viewMax.y),
                palette_.border);
    if (rows_.empty())
        return;

    const std::vector<const model::ReviewFinding*> findings = vm_.review().findingsForFile(doc.path);
    const auto& comments = vm_.review().comments();
    const float iconSize = fontSize * 0.75f;
    // Git: what changed since the last commit, as bars beside the fold arrows.
    const std::vector<viewmodel::LineChange>* changes =
        m.changeW > 0.0f ? &vm_.git().lineChanges(doc.path, (long long)doc.buffer.version(), [&doc]() { return doc.buffer.text(); }) : nullptr;
    if (changes) {
        const float bx = m.viewMin.x + m.gutterW - m.foldW - m.changeW + 2.0f * s_;
        for (int r = firstRow; r <= lastRow; ++r) {
            const Row& row = rows_[size_t(r)];
            if (row.fold >= 0 || row.line >= int(changes->size()))
                continue;
            const viewmodel::LineChange c = (*changes)[size_t(row.line)];
            const float y = m.origin.y + float(r) * m.lineH;
            if (c == viewmodel::LineChange::Added || c == viewmodel::LineChange::Modified)
                dl->AddRectFilled(ImVec2(bx, y), ImVec2(bx + 3.0f * s_, y + m.lineH),
                                  c == viewmodel::LineChange::Added ? palette_.diffAddedText : palette_.iconBlue);
            else if (c == viewmodel::LineChange::DeletedAbove && firstRowOfLine_[size_t(row.line)] == r)
                dl->AddTriangleFilled(ImVec2(bx, y - 4.0f * s_), ImVec2(bx, y + 4.0f * s_), ImVec2(bx + 5.0f * s_, y), palette_.diffRemovedText);
        }
    }
    for (int r = firstRow; r <= lastRow; ++r) {
        const Row& row = rows_[size_t(r)];
        const float y = m.origin.y + float(r) * m.lineH;
        const float cy = y + m.lineH * 0.5f;
        const float cx = m.viewMin.x + m.markerW * 0.5f + 1.0f * s_;
        if (row.fold >= 0) {
            // Findings hidden inside a folded region still show on its summary.
            const model::ReviewFinding* worst = nullptr;
            for (const model::ReviewFinding* f : findings)
                if (f->line > row.fold && f->line <= row.line && (!worst || f->severity > worst->severity))
                    worst = f;
            if (worst)
                dl->AddCircleFilled(ImVec2(cx, cy), 3.5f * s_, severityColor(worst->severity, palette_));
            continue;
        }
        if (firstRowOfLine_[size_t(row.line)] != r)
            continue;
        const float textY = y + (m.lineH - fontSize) * 0.5f;
        if (vm_.showLineNumbers.get()) {
            char number[16];
            std::snprintf(number, sizeof(number), "%d", row.line + 1);
            const float w = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, number).x;
            dl->AddText(font, fontSize, ImVec2(m.viewMin.x + m.gutterW - m.foldW - m.changeW - 4.0f * s_ - w, textY),
                        row.line == doc.caret.line ? palette_.gutterTextCurrent : palette_.gutterText, number);
        }
        if (const model::OutlineNode* region = vm_.regionAt(row.line)) {
            const bool folded = vm_.isFolded(*region);
            const float arrowSize = fontSize * 0.62f;
            const bool gutterHovered = ImGui::IsMouseHoveringRect(m.viewMin, ImVec2(m.viewMin.x + m.gutterW, m.viewMax.y));
            if (folded || gutterHovered)
                dl->AddText(font, arrowSize,
                            ImVec2(m.viewMin.x + m.gutterW - m.foldW * 0.5f - arrowSize * 0.35f, cy - arrowSize * 0.5f),
                            folded ? palette_.accent : palette_.gutterText, folded ? ICON_FA_CHEVRON_RIGHT : ICON_FA_CHEVRON_DOWN);
        }

        if (doc.bookmarks.count(row.line)) {
            dl->AddText(font, iconSize, ImVec2(cx - iconSize * 0.4f, cy - iconSize * 0.5f), palette_.bookmark, ICON_FA_BOOKMARK);
            continue;
        }
        const model::ReviewFinding* worst = nullptr;
        for (const model::ReviewFinding* f : findings)
            if (f->line == row.line && (!worst || f->severity > worst->severity))
                worst = f;
        if (worst) {
            dl->AddCircleFilled(ImVec2(cx, cy), 3.5f * s_, severityColor(worst->severity, palette_));
            continue;
        }
        for (const model::ReviewComment& c : comments)
            if (!c.resolved && c.file == doc.path && c.line == row.line) {
                dl->AddText(font, iconSize, ImVec2(cx - iconSize * 0.5f, cy - iconSize * 0.5f), palette_.iconPurple, ICON_FA_COMMENT);
                break;
            }
    }
}

void EditorView::drawSummaryRow(const model::Document& doc, const Row& row, const Metrics& m, float y)
{
    const model::OutlineNode* region = vm_.regionAt(row.fold);
    if (!region)
        return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize();
    // Indented one level deeper than the header, like the body it stands for.
    const std::string& header = doc.buffer.line(row.fold);
    int lead = 0;
    while (lead < int(header.size()) && isBlank(header[size_t(lead)]))
        ++lead;
    const float x0 = m.origin.x + float(visualColumn(header, lead) + tabStop) * m.charW - 6.0f * s_;
    const std::string summary = vm_.summary(*region);
    const std::string count = "  " + std::to_string(region->lineCount() - 1) + " lines";
    const float textW = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, summary.c_str()).x;
    const float countW = font->CalcTextSizeA(fontSize * 0.85f, FLT_MAX, 0.0f, count.c_str()).x;
    const float iconW = fontSize * 1.4f;
    const ImVec2 min(x0, y + 2.0f * s_);
    const ImVec2 max(x0 + iconW + textW + countW + 16.0f * s_, y + m.lineH - 2.0f * s_);
    const bool hovered = ImGui::IsMouseHoveringRect(min, max) && ImGui::IsWindowHovered();
    UiAutomation::instance().add({"fold:" + std::to_string(row.fold + 1), "region", region->title, min, max, true,
                                  [this, start = row.fold]() { vm_.setFolded(start, false); }});
    dl->AddRectFilled(min, max, hovered ? palette_.selection : palette_.accentSoft, 5.0f * s_);
    dl->AddRect(min, max, (palette_.accent & ~IM_COL32_A_MASK) | (ImU32(hovered ? 160 : 70) << IM_COL32_A_SHIFT), 5.0f * s_);
    const float textY = y + (m.lineH - fontSize) * 0.5f;
    dl->AddText(font, fontSize * 0.9f, ImVec2(min.x + 7.0f * s_, textY + fontSize * 0.05f), palette_.modeAgent,
                ICON_FA_WAND_MAGIC_SPARKLES);
    dl->AddText(font, fontSize, ImVec2(min.x + 6.0f * s_ + iconW, textY), palette_.text, summary.c_str());
    dl->AddText(font, fontSize * 0.85f, ImVec2(min.x + 6.0f * s_ + iconW + textW, textY + fontSize * 0.1f), palette_.textFaint,
                count.c_str());
    if (hovered) {
        ImGui::PushFont(fonts_.ui, Fonts::kUiSize);
        if (ImGui::BeginTooltip()) {
            ImGui::TextUnformatted(region->title.c_str());
            ImGui::TextDisabled("%s, lines %d-%d. Click to expand.", model::outlineKindName(region->kind),
                                region->startLine + 1, region->endLine + 1);
            ImGui::TextDisabled(vm_.workspaceRoot().empty() ? "Summary from sample data."
                                                            : "Summary from the code's comments and structure.");
            ImGui::EndTooltip();
        }
        ImGui::PopFont();
    }
}

void EditorView::renderSideBar(const model::Document& doc, ImVec2 size)
{
    using SideBar = viewmodel::NotepadViewModel::SideBar;
    ImDrawList* parentDl = ImGui::GetWindowDrawList();

    // Resize handle on the left edge.
    const ImVec2 handle = ImGui::GetCursorScreenPos();
    const float handleW = 5.0f * s_;
    ImGui::InvisibleButton("##sideBarResize", ImVec2(handleW, std::max(1.0f, size.y)));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        parentDl->AddLine(ImVec2(handle.x + handleW * 0.5f, handle.y), ImVec2(handle.x + handleW * 0.5f, handle.y + size.y),
                          palette_.accent, 2.0f * s_);
    }
    if (ImGui::IsItemActive())
        sideBarWidth_ -= ImGui::GetIO().MouseDelta.x;
    if (ImGui::IsItemDeactivated())
        vm_.settings().markDirty(); // remembered with the layout
    ImGui::SameLine(0.0f, 0.0f);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    panel::beginChild("##sideBar", ImVec2(size.x - handleW, size.y), palette_.panelBg, ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetWindowPos();
    const float w = ImGui::GetWindowWidth(), h = ImGui::GetWindowHeight();
    dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x, p.y + h), palette_.border);
    UiAutomation::instance().add({"layers-map", "panel", "Side bar", p, ImVec2(p.x + w, p.y + h), true, {}});

    // Header: mode tabs on the left, hide on the right; same margin as the rows below.
    ImGui::PushFont(fonts_.ui, Fonts::kUiSize * 0.84f);
    const float headerH = 30.0f * s_;
    const float margin = 8.0f * s_;
    panel::fill(dl, p, ImVec2(p.x + w, p.y + headerH), palette_.panelHeaderBg);
    dl->AddLine(ImVec2(p.x, p.y + headerH - 1.0f), ImVec2(p.x + w, p.y + headerH - 1.0f), palette_.border);
    const SideBar mode = vm_.sideBar.get();
    struct Tab {
        const char* label;
        SideBar mode;
        viewmodel::CommandId command;
    };
    const Tab tabs[] = {{ICON_FA_LAYER_GROUP "  Layers", SideBar::Layers, viewmodel::CommandId::ShowLayers},
                        {ICON_FA_BARS_STAGGERED "  Minimap", SideBar::Minimap, viewmodel::CommandId::ShowMinimap}};
    float x = p.x + margin;
    const float tabH = headerH - 10.0f * s_;
    for (const Tab& t : tabs) {
        const ImVec2 ts = ImGui::CalcTextSize(t.label);
        const float tw = ts.x + 18.0f * s_;
        const ImVec2 a(x, p.y + 5.0f * s_);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(t.label);
        if (ImGui::InvisibleButton("##mode", ImVec2(tw, tabH)))
            vm_.execute(t.command);
        ImGui::PopID();
        const bool active = t.mode == mode;
        const bool hovered = ImGui::IsItemHovered();
        registerLastItem(std::string("command:") + viewmodel::commandKey(t.command), "tab", t.label,
                         [this, c = t.command]() { vm_.execute(c); });
        if (active)
            dl->AddRectFilled(a, ImVec2(a.x + tw, a.y + tabH), palette_.tabActiveBg, 5.0f * s_);
        else if (hovered)
            dl->AddRectFilled(a, ImVec2(a.x + tw, a.y + tabH), palette_.hover, 5.0f * s_);
        dl->AddText(ImVec2(std::floor(a.x + 9.0f * s_), std::floor(a.y + (tabH - ts.y) * 0.5f)),
                    active || hovered ? palette_.text : palette_.textDim, t.label);
        x += tw + 2.0f * s_;
    }
    // Hide.
    const float closeW = tabH;
    const ImVec2 c0(p.x + w - margin - closeW, p.y + 5.0f * s_);
    ImGui::SetCursorScreenPos(c0);
    if (ImGui::InvisibleButton("##hideSideBar", ImVec2(closeW, tabH)))
        vm_.showLayersMap.set(false);
    const bool closeHovered = ImGui::IsItemHovered();
    ImGui::SetItemTooltip("Hide the side bar (Read > Side Bar brings it back)");
    registerLastItem("command:ToggleLayersMap", "button", "Hide side bar", [this]() { vm_.showLayersMap.set(false); });
    if (closeHovered)
        dl->AddRectFilled(c0, ImVec2(c0.x + closeW, c0.y + tabH), palette_.hover, 5.0f * s_);
    const ImVec2 xs = ImGui::CalcTextSize(ICON_FA_XMARK);
    dl->AddText(ImVec2(std::floor(c0.x + (closeW - xs.x) * 0.5f), std::floor(c0.y + (tabH - xs.y) * 0.5f)),
                closeHovered ? palette_.text : palette_.textDim, ICON_FA_XMARK);
    ImGui::PopFont();

    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + headerH));
    const ImVec2 body(w, std::max(1.0f, h - headerH));
    if (mode == SideBar::Layers)
        layers_.render(body);
    else
        renderMinimap(doc, body);
    ImGui::EndChild();
}

void EditorView::renderMinimap(const model::Document& doc, ImVec2 size)
{
    tabStop = std::max(1, doc.indent.width);
    panel::beginChild("##minimap", size, palette_.editorBg, ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetWindowPos();
    const float w = ImGui::GetWindowWidth(), h = ImGui::GetWindowHeight();
    const float pad = 6.0f * s_;
    const float lineH = 2.0f * s_;
    const float charW = std::clamp((w - 2.0f * pad) / 110.0f, 1.0f * s_, 2.5f * s_); // ~110 columns across
    const int lines = doc.buffer.lineCount();
    const float total = float(lines) * lineH;
    UiAutomation::instance().add({"minimap", "panel", "Minimap", p, ImVec2(p.x + w, p.y + h), true, {}});

    // Long files: the strip scrolls with the editor, keeping the viewport frame in sync.
    const int visibleLines = std::max(1, lastVisibleLine_ - firstVisibleLine_ + 1);
    const float maxOffset = std::max(0.0f, total - (h - 2.0f * pad));
    const float scrollFraction = lines > visibleLines ? float(firstVisibleLine_) / float(lines - visibleLines) : 0.0f;
    const float offset = maxOffset * std::clamp(scrollFraction, 0.0f, 1.0f);
    const float top = p.y + pad - offset;
    const int first = std::clamp(int(offset / lineH), 0, std::max(0, lines - 1));
    const int last = std::min(lines - 1, first + int(h / lineH) + 1);

    // Syntax-coloured blocks per token run (whitespace stays empty).
    int inBlock = 0;
    std::vector<model::Token> toks;
    const model::LanguageSpec& spec = model::languageSpec(doc.language);
    const SyntaxColors colors = resolveSyntaxColors(palette_, vm_, doc.language);
    inBlock = doc.highlightCache.stateAt(doc.buffer, spec, first);
    const int maxCols = int((w - 2.0f * pad) / charW);
    for (int l = first; l <= last; ++l) {
        const std::string& text = doc.buffer.line(l);
        model::highlightLine(text, spec, inBlock, toks);
        const float y = top + float(l) * lineH;
        int col = 0;
        for (int i = 0; i < int(text.size()) && col < maxCols;) {
            if (text[size_t(i)] == '\t') {
                col = (col / tabStop + 1) * tabStop;
                ++i;
                continue;
            }
            if (isBlank(text[size_t(i)])) {
                ++col;
                ++i;
                continue;
            }
            const model::Token tok = toks[size_t(i)];
            const int startCol = col;
            while (i < int(text.size()) && !isBlank(text[size_t(i)]) && toks[size_t(i)] == tok) {
                i = nextBoundary(text, i);
                ++col;
            }
            const ImU32 c = (colors[size_t(tok)] & ~IM_COL32_A_MASK) | (ImU32(170) << IM_COL32_A_SHIFT);
            dl->AddRectFilled(ImVec2(p.x + pad + float(startCol) * charW, y),
                              ImVec2(p.x + pad + float(std::min(col, maxCols)) * charW, y + lineH * 0.75f), c);
        }
    }

    // Markers on the right edge: findings and the caret line.
    for (const model::ReviewFinding* f : vm_.review().findingsForFile(doc.path)) {
        const float y = top + float(f->line) * lineH;
        dl->AddRectFilled(ImVec2(p.x + w - 4.0f * s_, y - 1.0f * s_), ImVec2(p.x + w - 1.0f * s_, y + 2.0f * s_),
                          severityColor(f->severity, palette_));
    }
    const float caretY = top + float(doc.caret.line) * lineH;
    dl->AddRectFilled(ImVec2(p.x, caretY), ImVec2(p.x + w, caretY + lineH), palette_.currentLine);

    // Viewport frame; click or drag to scroll.
    const ImVec2 v0(p.x + 1.0f, top + float(firstVisibleLine_) * lineH);
    const ImVec2 v1(p.x + w - 1.0f, top + float(lastVisibleLine_ + 1) * lineH);
    ImGui::SetCursorScreenPos(p);
    ImGui::InvisibleButton("##minimapArea", ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemActive()) {
        pendingScrollLine_ = std::clamp(int((ImGui::GetIO().MousePos.y - top) / lineH), 0, std::max(0, lines - 1));
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    dl->AddRectFilled(v0, v1, hovered || ImGui::IsItemActive() ? palette_.selection : palette_.hover);
    dl->AddRect(v0, v1, palette_.border);
    ImGui::EndChild();
}

void EditorView::drawHoverTooltip(const model::Document& doc, const Metrics& m)
{
    if (rows_.empty() || doc.path.empty())
        return;
    const int line = hitTest(doc, ImGui::GetIO().MousePos, m).line;
    const std::vector<const model::ReviewFinding*> findings = vm_.review().findingsForFile(doc.path);
    std::vector<const model::ReviewFinding*> onLine;
    for (const model::ReviewFinding* f : findings)
        if (f->line == line)
            onLine.push_back(f);
    std::vector<const model::ReviewComment*> notes;
    for (const model::ReviewComment& c : vm_.review().comments())
        if (!c.resolved && c.file == doc.path && c.line == line)
            notes.push_back(&c);
    if (onLine.empty() && notes.empty())
        return;
    // Only show when hovering the gutter or the text of that line.
    ImGui::PushFont(fonts_.ui, Fonts::kUiSize);
    if (ImGui::BeginTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
        for (const model::ReviewFinding* f : onLine) {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(severityColor(f->severity, palette_)), "%s %s",
                               ICON_FA_CIRCLE, model::severityName(f->severity));
            ImGui::SameLine();
            ImGui::TextDisabled("%s  %s", model::reviewToolName(f->tool), f->rule.c_str());
            ImGui::TextWrapped("%s", f->message.c_str());
        }
        for (const model::ReviewComment* c : notes) {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(palette_.iconPurple), ICON_FA_COMMENT " %s", c->author.c_str());
            ImGui::TextWrapped("%s", c->text.c_str());
        }
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    ImGui::PopFont();
}


// ---- Git: conflicts, change peek, blame ---------------------------------------------------------

void EditorView::updateConflicts(const model::Document& doc)
{
    if (conflictsDoc_ == doc.id && conflictsVersion_ == doc.buffer.version())
        return;
    conflictsDoc_ = doc.id;
    conflictsVersion_ = doc.buffer.version();
    conflicts_.clear();
    conflictLines_.clear();
    // Only files with a conflict marker pay for the full scan.
    bool any = false;
    for (int i = 0; i < doc.buffer.lineCount() && !any; ++i)
        any = doc.buffer.line(i).rfind("<<<<<<<", 0) == 0;
    if (!any)
        return;
    conflictLines_.reserve(size_t(doc.buffer.lineCount()));
    for (int i = 0; i < doc.buffer.lineCount(); ++i)
        conflictLines_.push_back(doc.buffer.line(i));
    conflicts_ = model::git::findConflicts(conflictLines_);
}

void EditorView::renderConflictBar(const model::Document& doc)
{
    int current = 0;
    for (int i = 0; i < int(conflicts_.size()); ++i)
        if (conflicts_[size_t(i)].start <= doc.caret.line)
            current = i + 1;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * s_, 5.0f * s_));
    panel::beginChild("##conflictBar", ImVec2(0.0f, ImGui::GetFrameHeight() + 10.0f * s_),
                      (palette_.iconOrange & ~IM_COL32_A_MASK) | (ImU32(40) << IM_COL32_A_SHIFT), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(palette_.iconOrange), ICON_FA_CODE_MERGE);
    ImGui::SameLine();
    if (current > 0)
        ImGui::Text("Merge conflict %d of %d: choose what to keep with the buttons on each block.", current, int(conflicts_.size()));
    else
        ImGui::Text("%d merge conflict(s) in this file: choose what to keep with the buttons on each block.", int(conflicts_.size()));
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_FA_ARROW_UP "  Previous"))
        jumpToConflict(doc, -1);
    ImGui::SetItemTooltip("Shift+F8");
    registerLastItem("conflict:previous", "button", "Previous conflict", [this, &doc]() { jumpToConflict(doc, -1); });
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_FA_ARROW_DOWN "  Next"))
        jumpToConflict(doc, 1);
    ImGui::SetItemTooltip("F8");
    registerLastItem("conflict:next", "button", "Next conflict", [this, &doc]() { jumpToConflict(doc, 1); });
    ImGui::EndChild();
}

void EditorView::jumpToConflict(const model::Document& doc, int direction)
{
    if (conflicts_.empty())
        return;
    int target = -1;
    if (direction > 0) {
        for (const model::git::ConflictBlock& b : conflicts_)
            if (b.start > doc.caret.line) {
                target = b.start;
                break;
            }
        if (target < 0)
            target = conflicts_.front().start;
    } else {
        for (auto it = conflicts_.rbegin(); it != conflicts_.rend(); ++it)
            if (it->start < doc.caret.line) {
                target = it->start;
                break;
            }
        if (target < 0)
            target = conflicts_.back().start;
    }
    vm_.setCaret({target, 0}, false);
    revealCaret_ = true;
}

void EditorView::drawConflictBackgrounds(const Metrics& m, int firstRow, int lastRow)
{
    // Current side tinted like a change, incoming like an addition, the marker lines dim.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float left = m.viewMin.x + m.gutterW;
    for (int r = firstRow; r <= lastRow && r < int(rows_.size()); ++r) {
        const Row& row = rows_[size_t(r)];
        if (row.fold >= 0)
            continue;
        for (const model::git::ConflictBlock& b : conflicts_) {
            if (row.line < b.start || row.line > b.end)
                continue;
            const bool marker = row.line == b.start || row.line == b.middle || row.line == b.end || row.line == b.base;
            const bool currentSide = row.line < (b.base >= 0 ? b.base : b.middle);
            const bool baseSide = b.base >= 0 && row.line > b.base && row.line < b.middle;
            const ImU32 c = marker ? palette_.hover : baseSide ? palette_.hover : currentSide ? palette_.diffChangedBg : palette_.diffAddedBg;
            const float y = m.origin.y + float(r) * m.lineH;
            dl->AddRectFilled(ImVec2(left, y), ImVec2(m.viewMax.x, y + m.lineH), c);
        }
    }
}

void EditorView::drawConflictActions(const model::Document& doc, const Metrics& m, int firstRow, int lastRow)
{
    // A row of buttons on each <<<<<<< line, right of its text.
    ImGui::PushFont(nullptr, Fonts::kUiSize * 0.85f * vm_.zoom.get());
    for (int i = 0; i < int(conflicts_.size()); ++i) {
        const model::git::ConflictBlock& b = conflicts_[size_t(i)];
        if (b.start >= int(firstRowOfLine_.size()))
            continue;
        const int r = firstRowOfLine_[size_t(b.start)];
        if (r < firstRow || r > lastRow)
            continue;
        const float y = m.origin.y + float(r) * m.lineH;
        const float x = m.origin.x + float(doc.buffer.line(b.start).size() + 3) * m.charW;
        ImGui::SetCursorScreenPos(ImVec2(std::max(x, m.viewMin.x + m.gutterW + 8.0f * s_), y + (m.lineH - ImGui::GetFrameHeight()) * 0.5f));
        ImGui::PushID(i);
        const std::string current = b.current.empty() ? "current" : b.current;
        const std::string incoming = b.incoming.empty() ? "incoming" : b.incoming;
        if (ImGui::SmallButton("Accept current"))
            resolveConflict(i, int(model::git::Resolution::Current));
        ImGui::SetItemTooltip("Keep %s's lines (yours) and drop the others", current.c_str());
        registerLastItem("conflict:" + std::to_string(i) + ":current", "button", "Accept current", [this, i]() {
            resolveConflict(i, int(model::git::Resolution::Current));
        });
        ImGui::SameLine(0.0f, 4.0f * s_);
        if (ImGui::SmallButton("Accept incoming"))
            resolveConflict(i, int(model::git::Resolution::Incoming));
        ImGui::SetItemTooltip("Keep %s's lines and drop yours", incoming.c_str());
        registerLastItem("conflict:" + std::to_string(i) + ":incoming", "button", "Accept incoming", [this, i]() {
            resolveConflict(i, int(model::git::Resolution::Incoming));
        });
        ImGui::SameLine(0.0f, 4.0f * s_);
        if (ImGui::SmallButton("Accept both"))
            resolveConflict(i, int(model::git::Resolution::Both));
        ImGui::SetItemTooltip("Keep both: yours first, then the incoming lines");
        ImGui::SameLine(0.0f, 4.0f * s_);
        if (ImGui::SmallButton(ICON_FA_CODE_COMPARE " Compare")) {
            const int currentEnd = b.base >= 0 ? b.base : b.middle;
            std::vector<std::string> ours(conflictLines_.begin() + b.start + 1, conflictLines_.begin() + currentEnd);
            std::vector<std::string> theirs(conflictLines_.begin() + b.middle + 1, conflictLines_.begin() + b.end);
            vm_.review().setDiff("Conflict " + std::to_string(i + 1) + ": " + current + " vs " + incoming, model::diffLines(ours, theirs));
            vm_.showPanelTab(viewmodel::PanelTab::Diff);
        }
        ImGui::PopID();
    }
    ImGui::PopFont();
}

void EditorView::resolveConflict(int block, int resolution)
{
    if (block < 0 || block >= int(conflicts_.size()))
        return;
    const model::git::ConflictBlock b = conflicts_[size_t(block)];
    const std::vector<std::string> lines = model::git::resolveConflict(conflictLines_, b, model::git::Resolution(resolution));
    vm_.replaceLines(b.start, b.end, lines);
    conflictsVersion_ = ~0ull; // recompute
    vm_.statusText.set(conflicts_.size() == 1 ? "Last conflict resolved: save the file and it is marked resolved"
                                              : "Conflict resolved; " + std::to_string(conflicts_.size() - 1) + " left in this file (F8: next)");
}

void EditorView::renderChangePeek(const model::Document& doc)
{
    if (openPeek_) {
        ImGui::OpenPopup("##changePeek");
        openPeek_ = false;
    }
    ImGui::SetNextWindowSizeConstraints(ImVec2(360.0f * s_, 0.0f), ImVec2(720.0f * s_, 420.0f * s_));
    if (!ImGui::BeginPopup("##changePeek"))
        return;
    int first = 0, last = -1;
    std::vector<std::string> head;
    if (!vm_.git().changeAt(doc.path, peekLine_, first, last, head)) {
        ImGui::TextDisabled("No change here any more.");
        ImGui::EndPopup();
        return;
    }
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(palette_.textDim), ICON_FA_CODE_BRANCH "  Since the last commit:");
    ImGui::PushFont(fonts_.mono, Fonts::kEditorSize * 0.9f);
    if (head.empty())
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(palette_.diffAddedText), "(these lines are new)");
    else {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(palette_.textFaint), "It was:");
        for (size_t i = 0; i < head.size() && i < 20; ++i)
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(palette_.diffRemovedText), "- %s", head[i].c_str());
        if (head.size() > 20)
            ImGui::TextDisabled("... %d more", int(head.size() - 20));
    }
    ImGui::PopFont();
    ImGui::Spacing();
    if (ImGui::Button(ICON_FA_ROTATE_LEFT "  Revert this change")) {
        vm_.replaceLines(first, last, head);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SetItemTooltip("Put these lines back as they were in the last commit (Undo brings your version back)");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_CODE_COMPARE "  Open diff")) {
        vm_.git().showChange(vm_.git().repoPath(doc.path), model::git::Area::Changes);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SetItemTooltip("The whole file's changes, where you can stage some of them");
    ImGui::EndPopup();
}

void EditorView::drawBlame(const model::Document& doc, const Metrics& m, int firstRow, int lastRow)
{
    const std::vector<model::git::BlameLine>& blame = vm_.git().blame(doc.path);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float x0 = m.viewMin.x + m.markerW;
    const float fontSize = ImGui::GetFontSize() * 0.85f;
    ImFont* font = ImGui::GetFont();
    const int lines = doc.buffer.lineCount();
    const bool fresh = !blame.empty() && int(blame.size()) >= lines - 1 && int(blame.size()) <= lines;
    if (!fresh && !doc.modified && blameRequested_ != doc.buffer.version()) {
        blameRequested_ = doc.buffer.version();
        vm_.git().loadBlame(doc.path, doc.buffer.text());
    }
    const ImVec2 mouse = ImGui::GetMousePos();
    const float edge = x0 + m.blameW;
    const bool inColumn = mouse.x >= x0 && mouse.x < edge - 4.0f * s_ && ImGui::IsWindowHovered();
    // As the column narrows it drops the author, then the age; the tooltip always has everything.
    const bool showAuthor = blameColumns_ >= 22.0f;
    const bool showAge = blameColumns_ >= 15.0f;
    const int64_t now = int64_t(std::time(nullptr));
    for (int r = firstRow; r <= lastRow && r < int(rows_.size()); ++r) {
        const Row& row = rows_[size_t(r)];
        if (row.fold >= 0 || firstRowOfLine_[size_t(row.line)] != r)
            continue;
        const float y = m.origin.y + float(r) * m.lineH;
        if (!fresh) {
            if (r == firstRow)
                dl->AddText(font, fontSize, ImVec2(x0 + 4.0f * s_, y + (m.lineH - fontSize) * 0.5f), palette_.textFaint,
                            doc.modified ? "save to update blame" : "loading blame...");
            continue;
        }
        if (row.line >= int(blame.size()))
            continue;
        const model::git::BlameLine& b = blame[size_t(row.line)];
        const bool uncommitted = b.hash.find_first_not_of('0') == std::string::npos;
        const bool firstOfRun = row.line == 0 || blame[size_t(row.line - 1)].hash != b.hash;
        if (inColumn && mouse.y >= y && mouse.y < y + m.lineH) {
            dl->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + m.blameW, y + m.lineH), palette_.hover);
            if (uncommitted)
                ImGui::SetTooltip("Not committed yet");
            else
                ImGui::SetTooltip("%s\n%s, %s\n%s\nClick: show this commit in History", b.summary.c_str(), b.author.c_str(),
                                  model::git::relativeTime(b.time, now).c_str(), b.hash.substr(0, 10).c_str());
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !uncommitted)
                vm_.gitShowCommitRequested.emit(b.hash);
        }
        if (!firstOfRun)
            continue;
        std::string text = uncommitted ? std::string(showAge ? "not committed yet" : "new")
                                       : b.hash.substr(0, 7) + (showAuthor ? " " + b.author : std::string());
        float ageW = 0.0f;
        std::string age;
        if (!uncommitted && showAge) {
            age = model::git::relativeTime(b.time, now);
            if (age.size() > 4 && age.compare(age.size() - 4, 4, " ago") == 0)
                age.resize(age.size() - 4);
            ageW = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, age.c_str()).x;
            dl->AddText(font, fontSize, ImVec2(edge - 6.0f * s_ - ageW, y + (m.lineH - fontSize) * 0.5f), palette_.textFaint, age.c_str());
        }
        dl->PushClipRect(ImVec2(x0, y), ImVec2(std::max(x0, edge - (ageW > 0.0f ? ageW + 12.0f * s_ : 4.0f * s_)), y + m.lineH), true);
        dl->AddText(font, fontSize, ImVec2(x0 + 4.0f * s_, y + (m.lineH - fontSize) * 0.5f), uncommitted ? palette_.severityWarning : palette_.textDim,
                    text.c_str());
        dl->PopClipRect();
    }
    // Its right edge: drag to resize (double-click: back to the full width).
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(edge - 3.0f * s_, m.viewMin.y));
    ImGui::InvisibleButton("##blameEdge", ImVec2(6.0f * s_, std::max(1.0f, m.viewMax.y - m.viewMin.y)));
    const bool edgeActive = ImGui::IsItemActive();
    const bool edgeHovered = ImGui::IsItemHovered();
    if (edgeHovered || edgeActive)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (edgeHovered && !edgeActive)
        ImGui::SetTooltip("Drag to resize the blame column; double-click for the full width");
    if (edgeActive && ImGui::GetIO().MouseDelta.x != 0.0f) {
        blameColumns_ = std::clamp(blameColumns_ + ImGui::GetIO().MouseDelta.x / std::max(1.0f, m.blameW / blameColumns_), 9.0f, 60.0f);
        vm_.settings().markDirty();
    }
    if (edgeHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        blameColumns_ = 30.0f;
        vm_.settings().markDirty();
    }
    registerLastItem("blame:resize", "button", "Resize the blame column", nullptr);
    ImGui::SetCursorScreenPos(cursor);
    dl->AddLine(ImVec2(edge - 1.0f, m.viewMin.y), ImVec2(edge - 1.0f, m.viewMax.y),
                edgeActive || edgeHovered ? palette_.accent : palette_.border, edgeActive || edgeHovered ? 2.0f * s_ : 1.0f);
}

} // namespace enp::view
