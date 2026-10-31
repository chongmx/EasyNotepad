#include "enp/view/TerminalView.h"

#include "enp/view/PanelFrame.h"

#include "IconsFontAwesome6.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <utility>

namespace enp::view {

using model::TermKey;
using viewmodel::TerminalInstance;

namespace {

constexpr float kLineSpacing = 1.25f;
constexpr float kPadding = 6.0f;
constexpr float kListWidth = 170.0f;

void appendUtf8(std::string& out, char32_t c)
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

ImU32 withAlpha(ImU32 color, float alpha)
{
    ImVec4 c = ImGui::ColorConvertU32ToFloat4(color);
    c.w *= alpha;
    return ImGui::ColorConvertFloat4ToU32(c);
}

bool isWordChar(char32_t c)
{
    return c > 0x7F || std::isalnum(int(c)) || c == '_' || c == '-' || c == '.' || c == '/' || c == '\\' || c == ':';
}

struct KeyMapping {
    ImGuiKey key;
    TermKey term;
};

const KeyMapping kKeys[] = {
    {ImGuiKey_Enter, TermKey::Enter},       {ImGuiKey_KeypadEnter, TermKey::Enter}, {ImGuiKey_Backspace, TermKey::Backspace},
    {ImGuiKey_Tab, TermKey::Tab},           {ImGuiKey_Escape, TermKey::Escape},     {ImGuiKey_UpArrow, TermKey::Up},
    {ImGuiKey_DownArrow, TermKey::Down},    {ImGuiKey_LeftArrow, TermKey::Left},    {ImGuiKey_RightArrow, TermKey::Right},
    {ImGuiKey_Home, TermKey::Home},         {ImGuiKey_End, TermKey::End},           {ImGuiKey_PageUp, TermKey::PageUp},
    {ImGuiKey_PageDown, TermKey::PageDown}, {ImGuiKey_Insert, TermKey::Insert},     {ImGuiKey_Delete, TermKey::Delete},
    {ImGuiKey_F1, TermKey::F1},             {ImGuiKey_F2, TermKey::F2},             {ImGuiKey_F3, TermKey::F3},
    {ImGuiKey_F4, TermKey::F4},             {ImGuiKey_F5, TermKey::F5},             {ImGuiKey_F6, TermKey::F6},
    {ImGuiKey_F7, TermKey::F7},             {ImGuiKey_F8, TermKey::F8},             {ImGuiKey_F9, TermKey::F9},
    {ImGuiKey_F10, TermKey::F10},           {ImGuiKey_F11, TermKey::F11},           {ImGuiKey_F12, TermKey::F12},
};

} // namespace

TerminalView::TerminalView(viewmodel::TerminalViewModel& vm, const Fonts& fonts, const Palette& palette, float uiScale)
    : vm_(vm), fonts_(fonts), palette_(palette), s_(uiScale)
{
    subscriptions_.push_back(vm_.focusRequested.subscribe([this]() { focusRequested_ = true; }));
}

void TerminalView::render(ImVec2 size, float zoom)
{
    TerminalInstance* terminal = vm_.active();
    if (!terminal) {
        focused_ = false;
        renderPlaceholder(size);
        return;
    }
    const bool list = vm_.terminals().size() > 1;
    const float listW = list ? std::min(kListWidth * s_, size.x * 0.35f) : 0.0f;
    renderScreen(*terminal, ImVec2(size.x - listW, size.y), zoom);
    if (list) {
        ImGui::SameLine(0.0f, 0.0f);
        renderList(listW, size.y);
    }
    const Deferred action = std::exchange(deferred_, Deferred::None);
    if (action == Deferred::Close)
        vm_.killActive();
    else if (action == Deferred::Clear)
        vm_.clearActive();
}

void TerminalView::renderPlaceholder(ImVec2 size)
{
    ImGui::BeginChild("##terminalPlaceholder", size);
    ImGui::Spacing();
    if (!vm_.supported()) {
        ImGui::TextDisabled("%s  Terminals are not available on this platform.", ICON_FA_TERMINAL);
    } else if (!vm_.lastError.get().empty()) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(palette_.severityError), "%s  %s", ICON_FA_TRIANGLE_EXCLAMATION,
                           vm_.lastError.get().c_str());
        if (ImGui::Button(ICON_FA_ROTATE " Retry"))
            vm_.newTerminal(0);
    } else {
        ImGui::TextDisabled("No terminal is open.");
        if (ImGui::Button(ICON_FA_TERMINAL " New Terminal"))
            vm_.newTerminal(0);
    }
    ImGui::EndChild();
}

void TerminalView::renderList(float width, float height)
{
    panel::beginChild("##terminalList", ImVec2(width, height), palette_.panelBg, ImGuiChildFlags_Borders);
    int closeIndex = -1;
    const auto& terminals = vm_.terminals();
    for (int i = 0; i < int(terminals.size()); ++i) {
        const TerminalInstance& t = *terminals[size_t(i)];
        ImGui::PushID(t.id);
        const std::string label = std::string(ICON_FA_TERMINAL "  ") + t.name + (t.exited ? " (exited)" : "");
        if (ImGui::Selectable(label.c_str(), i == vm_.activeIndex.get(), ImGuiSelectableFlags_AllowOverlap))
            vm_.activate(i);
        if (!t.screen.title().empty())
            ImGui::SetItemTooltip("%s", t.screen.title().c_str());
        if (ImGui::IsItemHovered() || i == vm_.activeIndex.get()) {
            ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - ImGui::GetFrameHeight());
            if (ImGui::SmallButton(ICON_FA_TRASH_CAN "##kill"))
                closeIndex = i;
            ImGui::SetItemTooltip("Kill terminal");
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (closeIndex >= 0)
        vm_.close(closeIndex);
}

ImU32 TerminalView::resolve(uint32_t color, bool foreground, uint8_t flags) const
{
    const uint32_t type = color & model::termcolor::kTypeMask;
    if (type == model::termcolor::kDefault)
        return foreground ? palette_.synText : palette_.editorBg;
    uint32_t rgb = color & 0xFFFFFFu;
    if (type == model::termcolor::kIndexed) {
        int index = int(color & 0xFF);
        if (foreground && (flags & model::kBold) && index < 8)
            index += 8; // bold brightens the base colours, as most terminals do
        rgb = model::termcolor::paletteRgb(index, palette_.ansi);
    }
    return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 255);
}

bool TerminalView::selected(int line, int column) const
{
    if (!selection_.active)
        return false;
    int l0 = selection_.anchorLine, c0 = selection_.anchorColumn, l1 = selection_.line, c1 = selection_.column;
    if (l1 < l0 || (l1 == l0 && c1 < c0)) {
        std::swap(l0, l1);
        std::swap(c0, c1);
    }
    if (line < l0 || line > l1)
        return false;
    if (line == l0 && column < c0)
        return false;
    if (line == l1 && column >= c1)
        return false;
    return true;
}

// ---- Screen -------------------------------------------------------------------------------------

void TerminalView::renderScreen(TerminalInstance& terminal, ImVec2 size, float zoom)
{
    ImGui::PushFont(fonts_.mono, Fonts::kEditorSize * zoom);
    const float charW = ImGui::CalcTextSize("M").x;
    const float lineH = std::floor(ImGui::GetFontSize() * kLineSpacing);
    const float pad = kPadding * s_;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const std::string id = "##terminal" + std::to_string(terminal.id);
    panel::beginChild(id.c_str(), size, palette_.editorBg, ImGuiChildFlags_None, ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar();
    if (focusRequested_) {
        ImGui::SetWindowFocus();
        focusRequested_ = false;
    }
    focused_ = ImGui::IsWindowFocused();

    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 winPos = ImGui::GetWindowPos();
    const float viewW = ImGui::GetWindowWidth() - style.ScrollbarSize;
    const float viewH = ImGui::GetWindowHeight();
    vm_.resize(int((viewW - 2.0f * pad) / charW), int((viewH - 2.0f * pad) / lineH));

    const model::TerminalScreen& screen = terminal.screen;
    if (terminal.id != shownId_) {
        shownId_ = terminal.id;
        stickToBottom_ = true;
        selection_ = Selection();
    }
    // Follow new output only while the view is at the bottom (scrolling up pauses it).
    const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
    if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0.0f)
        stickToBottom_ = atBottom && ImGui::GetIO().MouseWheel < 0.0f;
    else if (atBottom)
        stickToBottom_ = true;

    const int total = screen.totalLines();
    const ImVec2 origin(winPos.x + pad, winPos.y + pad - ImGui::GetScrollY());
    const int first = std::max(0, int((ImGui::GetScrollY() - pad) / lineH));
    const int last = std::min(total - 1, first + int(viewH / lineH) + 1);

    // Mouse: selection with the left button, context menu with the right one.
    ImGuiIO& io = ImGui::GetIO();
    const bool hovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(winPos, ImVec2(winPos.x + viewW, winPos.y + viewH));
    auto cellAt = [&](ImVec2 p) {
        const int line = std::clamp(int(std::floor((p.y - origin.y) / lineH)), 0, total - 1);
        const int column = std::clamp(int(std::lround((p.x - origin.x) / charW)), 0, screen.columns());
        return std::make_pair(line, column);
    };
    if (hovered)
        ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const auto [line, column] = cellAt(io.MousePos);
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            const auto& cells = screen.lineAt(line).cells;
            int a = std::min(column, int(cells.size()) - 1), b = a;
            while (a > 0 && isWordChar(cells[size_t(a - 1)].ch))
                --a;
            while (b < int(cells.size()) && isWordChar(cells[size_t(b)].ch))
                ++b;
            selection_ = {true, line, a, line, b};
            dragging_ = false;
        } else {
            selection_ = {false, line, column, line, column};
            dragging_ = true;
        }
    }
    if (dragging_) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const auto [line, column] = cellAt(io.MousePos);
            selection_.line = line;
            selection_.column = column;
            selection_.active = line != selection_.anchorLine || column != selection_.anchorColumn;
            if (io.MousePos.y < winPos.y)
                ImGui::SetScrollY(ImGui::GetScrollY() - lineH);
            else if (io.MousePos.y > winPos.y + viewH)
                ImGui::SetScrollY(ImGui::GetScrollY() + lineH);
        } else {
            dragging_ = false;
        }
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        ImGui::OpenPopup("##terminalMenu");
    renderContextMenu(terminal);

    if (focused_)
        handleKeyboard(terminal, viewH);

    // Cells: background runs, then text runs of equal style.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize();
    const float textOffset = (lineH - fontSize) * 0.5f;
    std::string run;
    for (int l = first; l <= last; ++l) {
        const model::TermLine& line = screen.lineAt(l);
        const float y = origin.y + float(l) * lineH;
        const int columns = int(line.cells.size());
        for (int c = 0; c < columns; ++c) {
            const model::TermCell& cell = line.cells[size_t(c)];
            const bool inverse = (cell.attr.flags & model::kInverse) != 0;
            const uint32_t bg = inverse ? cell.attr.fg : cell.attr.bg;
            const bool sel = selected(l, c);
            if (bg != model::termcolor::kDefault || inverse || sel) {
                const ImU32 color = sel ? palette_.selection : resolve(bg, inverse, cell.attr.flags);
                const float x = origin.x + float(c) * charW;
                dl->AddRectFilled(ImVec2(x, y), ImVec2(x + charW + 0.5f, y + lineH), color);
            }
        }
        for (int c = 0; c < columns;) {
            const model::TermCell& start = line.cells[size_t(c)];
            if (start.ch == U' ' && !(start.attr.flags & (model::kUnderline | model::kStrike))) {
                ++c;
                continue;
            }
            // ASCII runs share one AddText; other characters are placed per cell to stay on the grid.
            int e = c + 1;
            if (start.ch < 0x80)
                while (e < columns && line.cells[size_t(e)].attr == start.attr && line.cells[size_t(e)].ch < 0x80)
                    ++e;
            run.clear();
            for (int k = c; k < e; ++k)
                appendUtf8(run, line.cells[size_t(k)].ch);
            const model::TermAttr& a = start.attr;
            const bool inverse = (a.flags & model::kInverse) != 0;
            ImU32 fg = inverse ? resolve(a.bg, false, a.flags) : resolve(a.fg, true, a.flags);
            if (a.flags & model::kDim)
                fg = withAlpha(fg, 0.6f);
            const float x = origin.x + float(c) * charW;
            if (!(a.flags & model::kHidden))
                dl->AddText(font, fontSize, ImVec2(x, y + textOffset), fg, run.data(), run.data() + run.size());
            if (a.flags & model::kUnderline)
                dl->AddLine(ImVec2(x, y + lineH - 2.0f), ImVec2(x + float(e - c) * charW, y + lineH - 2.0f), fg);
            if (a.flags & model::kStrike)
                dl->AddLine(ImVec2(x, y + lineH * 0.5f), ImVec2(x + float(e - c) * charW, y + lineH * 0.5f), fg);
            c = e;
        }
    }

    // Cursor: a blinking block while focused, an outline otherwise.
    if (screen.cursorVisible() && !terminal.exited) {
        const int line = screen.scrollbackSize() + screen.cursorRow();
        const float x = origin.x + float(screen.cursorColumn()) * charW;
        const float y = origin.y + float(line) * lineH;
        if (!focused_) {
            dl->AddRect(ImVec2(x, y), ImVec2(x + charW, y + lineH), palette_.caret);
        } else if (!blinkCursor_ || std::fmod(ImGui::GetTime() - lastActivity_, 1.0) < 0.6) {
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + charW, y + lineH), palette_.caret);
            const model::TermCell& cell = screen.row(screen.cursorRow()).cells[size_t(screen.cursorColumn())];
            if (cell.ch != U' ') {
                run.clear();
                appendUtf8(run, cell.ch);
                dl->AddText(font, fontSize, ImVec2(x, y + textOffset), palette_.editorBg, run.c_str());
            }
        }
    }

    ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
    ImGui::Dummy(ImVec2(1.0f, float(total) * lineH + 2.0f * pad));
    if (screen.revision() != shownRevision_) {
        shownRevision_ = screen.revision();
        if (stickToBottom_)
            ImGui::SetScrollY(float(total) * lineH + 2.0f * pad); // clamped to the bottom
    }
    ImGui::EndChild();
    ImGui::PopFont();
}

// ---- Input --------------------------------------------------------------------------------------

void TerminalView::send(const std::string& bytes)
{
    if (bytes.empty())
        return;
    if (const TerminalInstance* t = vm_.active(); t && t->exited) {
        deferred_ = Deferred::Close; // "Press any key to close."
        return;
    }
    vm_.sendInput(bytes);
    stickToBottom_ = true;
    selection_.active = false;
    lastActivity_ = ImGui::GetTime();
}

void TerminalView::copySelection(const TerminalInstance& terminal)
{
    if (!selection_.active)
        return;
    ImGui::SetClipboardText(terminal.screen.text(selection_.anchorLine, selection_.anchorColumn, selection_.line,
                                                 selection_.column).c_str());
    selection_.active = false;
}

void TerminalView::paste()
{
    if (const TerminalInstance* t = vm_.active(); t && t->exited) {
        deferred_ = Deferred::Close;
        return;
    }
    if (const char* text = ImGui::GetClipboardText()) {
        vm_.paste(text);
        stickToBottom_ = true;
        lastActivity_ = ImGui::GetTime();
    }
}

void TerminalView::handleKeyboard(const TerminalInstance& terminal, float pageHeight)
{
    ImGuiIO& io = ImGui::GetIO();
    const model::TermMods mods{io.KeyShift, io.KeyAlt, io.KeyCtrl};

    for (const KeyMapping& k : kKeys) {
        if (!ImGui::IsKeyPressed(k.key, true))
            continue;
        if (io.KeyShift && !io.KeyCtrl && (k.term == TermKey::PageUp || k.term == TermKey::PageDown)) {
            ImGui::SetScrollY(ImGui::GetScrollY() + (k.term == TermKey::PageUp ? -pageHeight : pageHeight));
            stickToBottom_ = false;
            continue;
        }
        send(model::encodeKey(k.term, mods, terminal.screen.applicationCursorKeys()));
    }

    if (io.KeyCtrl && !io.KeyAlt) {
        for (int key = ImGuiKey_A; key <= ImGuiKey_Z; ++key) {
            if (!ImGui::IsKeyPressed(ImGuiKey(key), true))
                continue;
            const char letter = char('a' + (key - ImGuiKey_A));
            if (letter == 'c' && (io.KeyShift || selection_.active)) {
                copySelection(terminal); // Ctrl+C with a selection copies instead of interrupting
            } else if (letter == 'v') {
                paste();
            } else if (!io.KeyShift && letter != 'j') { // Ctrl+J toggles the panel
                send(model::encodeControl(letter));
            }
        }
        const std::pair<ImGuiKey, char> punctuation[] = {{ImGuiKey_LeftBracket, '['}, {ImGuiKey_Backslash, '\\'},
                                                         {ImGuiKey_RightBracket, ']'}, {ImGuiKey_Space, ' '},
                                                         {ImGuiKey_Minus, '_'}};
        for (const auto& [key, c] : punctuation)
            if (ImGui::IsKeyPressed(key, true))
                send(model::encodeControl(c));
    }

    // Typed text; AltGr arrives as Ctrl+Alt on Windows and must still type.
    if (!io.KeyCtrl || io.KeyAlt) {
        std::string typed;
        for (ImWchar c : io.InputQueueCharacters)
            if (c >= 32 && c != 127)
                appendUtf8(typed, char32_t(c));
        if (!typed.empty())
            send(model::encodeText(typed, io.KeyAlt && !io.KeyCtrl));
    }
}

void TerminalView::renderContextMenu(const TerminalInstance& terminal)
{
    ImGui::PushFont(fonts_.ui, Fonts::kUiSize);
    if (ImGui::BeginPopup("##terminalMenu")) {
        if (ImGui::MenuItem(ICON_FA_COPY "  Copy", "Ctrl+C", false, selection_.active))
            copySelection(terminal);
        if (ImGui::MenuItem(ICON_FA_PASTE "  Paste", "Ctrl+V"))
            paste();
        if (ImGui::MenuItem(ICON_FA_OBJECT_GROUP "  Select All")) {
            selection_ = {true, 0, 0, terminal.screen.totalLines() - 1, terminal.screen.columns()};
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_ERASER "  Clear"))
            deferred_ = Deferred::Clear;
        if (ImGui::MenuItem(ICON_FA_TRASH_CAN "  Kill Terminal"))
            deferred_ = Deferred::Close;
        ImGui::EndPopup();
    }
    ImGui::PopFont();
}

} // namespace enp::view
