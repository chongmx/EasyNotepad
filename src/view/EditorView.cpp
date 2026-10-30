#include "enp/view/EditorView.h"

#include "IconsFontAwesome6.h"
#include "misc/cpp/imgui_stdlib.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <unordered_set>

namespace enp::view {

using model::TextPos;
using viewmodel::CommandId;

namespace {

constexpr int kTabSize = 4;
constexpr float kLineSpacing = 1.45f;
constexpr float kTopPadding = 4.0f;

// ---- Syntax colouring -------------------------------------------------------------------------

enum class Tok : unsigned char { Text, Keyword, Type, String, Number, Comment, Preproc, Heading };

bool isIdentStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool isIdent(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }
bool isWordChar(char c) { return isIdent(c) || (static_cast<unsigned char>(c) & 0x80) != 0; }
bool isBlank(char c) { return c == ' ' || c == '\t'; }

const std::unordered_set<std::string>& keywords(model::Language language)
{
    static const std::unordered_set<std::string> none;
    static const std::unordered_set<std::string> cpp = {
        "alignas", "auto", "bool", "break", "case", "catch", "char", "class", "const", "constexpr", "continue",
        "default", "delete", "do", "double", "else", "enum", "explicit", "extern", "false", "final", "float", "for",
        "friend", "if", "inline", "int", "long", "namespace", "new", "noexcept", "nullptr", "operator", "override",
        "private", "protected", "public", "return", "short", "signed", "sizeof", "static", "static_cast", "struct",
        "switch", "template", "this", "throw", "true", "try", "typedef", "typename", "union", "unsigned", "using",
        "virtual", "void", "volatile", "while", "size_t"};
    static const std::unordered_set<std::string> python = {
        "and", "as", "assert", "async", "await", "break", "class", "continue", "def", "del", "elif", "else", "except",
        "False", "finally", "for", "from", "global", "if", "import", "in", "is", "lambda", "None", "nonlocal", "not",
        "or", "pass", "raise", "return", "True", "try", "while", "with", "yield", "self"};
    static const std::unordered_set<std::string> js = {
        "async", "await", "break", "case", "catch", "class", "const", "continue", "default", "do", "else", "export",
        "extends", "false", "finally", "for", "from", "function", "if", "import", "in", "instanceof", "let", "new",
        "null", "of", "return", "switch", "this", "throw", "true", "try", "typeof", "undefined", "var", "while"};
    static const std::unordered_set<std::string> json = {"true", "false", "null"};
    switch (language) {
    case model::Language::Cpp: return cpp;
    case model::Language::Python: return python;
    case model::Language::JavaScript: return js;
    case model::Language::Json: return json;
    default: return none;
    }
}

void fill(std::vector<Tok>& toks, size_t begin, size_t end, Tok tok)
{
    std::fill(toks.begin() + std::ptrdiff_t(std::min(begin, toks.size())),
              toks.begin() + std::ptrdiff_t(std::min(end, toks.size())), tok);
}

void highlightMarkdown(const std::string& line, std::vector<Tok>& out)
{
    const size_t n = line.size();
    if (line.rfind("    ", 0) == 0) {
        fill(out, 0, n, Tok::String);
        return;
    }
    size_t ws = 0;
    while (ws < n && isBlank(line[ws]))
        ++ws;
    if (ws < n && line[ws] == '#') {
        fill(out, 0, n, Tok::Heading);
        return;
    }
    if (ws < n && line[ws] == '>') {
        fill(out, 0, n, Tok::Comment);
        return;
    }
    if (ws + 1 < n && (line[ws] == '-' || line[ws] == '*') && line[ws + 1] == ' ')
        out[ws] = Tok::Keyword;
    for (size_t i = 0; i < n;) {
        if (line[i] == '`') {
            size_t j = line.find('`', i + 1);
            j = j == std::string::npos ? n : j + 1;
            fill(out, i, j, Tok::String);
            i = j;
        } else if (line.compare(i, 2, "**") == 0 && line.find("**", i + 2) != std::string::npos) {
            const size_t j = line.find("**", i + 2) + 2;
            fill(out, i, j, Tok::Keyword);
            i = j;
        } else {
            ++i;
        }
    }
}

// Colour class of every byte of `line`; `inBlock` carries /* */ comments across lines.
void highlight(const std::string& line, model::Language language, bool& inBlock, std::vector<Tok>& out)
{
    using L = model::Language;
    out.assign(line.size(), Tok::Text);
    if (language == L::PlainText)
        return;
    if (language == L::Markdown) {
        highlightMarkdown(line, out);
        return;
    }
    const bool cLike = language == L::Cpp || language == L::JavaScript || language == L::Json;
    const bool hashComments = language == L::Python || language == L::CMake;
    const auto& kw = keywords(language);
    const size_t n = line.size();
    size_t i = 0;

    if (inBlock) {
        const size_t end = line.find("*/");
        if (end == std::string::npos) {
            fill(out, 0, n, Tok::Comment);
            return;
        }
        fill(out, 0, end + 2, Tok::Comment);
        i = end + 2;
        inBlock = false;
    }
    if (language == L::Cpp) {
        size_t ws = i;
        while (ws < n && isBlank(line[ws]))
            ++ws;
        if (ws < n && line[ws] == '#') {
            size_t j = ws + 1;
            while (j < n && isIdent(line[j]))
                ++j;
            fill(out, ws, j, Tok::Preproc);
            const size_t lt = line.find('<', j);
            const size_t gt = lt == std::string::npos ? lt : line.find('>', lt);
            if (gt != std::string::npos)
                fill(out, lt, gt + 1, Tok::String);
            i = j;
        }
    }

    while (i < n) {
        const char c = line[i];
        if (cLike && c == '/' && i + 1 < n && line[i + 1] == '/') {
            fill(out, i, n, Tok::Comment);
            return;
        }
        if (cLike && c == '/' && i + 1 < n && line[i + 1] == '*') {
            const size_t end = line.find("*/", i + 2);
            if (end == std::string::npos) {
                fill(out, i, n, Tok::Comment);
                inBlock = true;
                return;
            }
            fill(out, i, end + 2, Tok::Comment);
            i = end + 2;
            continue;
        }
        if (hashComments && c == '#') {
            fill(out, i, n, Tok::Comment);
            return;
        }
        if (c == '"' || c == '\'' || (language == L::JavaScript && c == '`')) {
            size_t j = i + 1;
            while (j < n && line[j] != c)
                j += line[j] == '\\' ? 2 : 1;
            j = std::min(j + 1, n);
            Tok tok = Tok::String;
            if (language == L::Json) {
                size_t after = j;
                while (after < n && isBlank(line[after]))
                    ++after;
                if (after < n && line[after] == ':')
                    tok = Tok::Type;
            }
            fill(out, i, j, tok);
            i = j;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c))) {
            size_t j = i;
            while (j < n && (isIdent(line[j]) || line[j] == '.'))
                ++j;
            fill(out, i, j, Tok::Number);
            i = j;
            continue;
        }
        if (language == L::CMake && c == '$' && i + 1 < n && line[i + 1] == '{') {
            const size_t end = line.find('}', i);
            const size_t j = end == std::string::npos ? n : end + 1;
            fill(out, i, j, Tok::Type);
            i = j;
            continue;
        }
        if (isIdentStart(c)) {
            size_t j = i;
            while (j < n && isIdent(line[j]))
                ++j;
            const std::string word = line.substr(i, j - i);
            const bool call = j < n && line[j] == '(';
            const bool scope = j + 1 < n && line[j] == ':' && line[j + 1] == ':';
            if (kw.count(word))
                fill(out, i, j, Tok::Keyword);
            else if (language == L::CMake && call)
                fill(out, i, j, Tok::Keyword);
            else if (language == L::CMake && std::all_of(word.begin(), word.end(), [](char ch) {
                         return !std::islower(static_cast<unsigned char>(ch));
                     }))
                fill(out, i, j, Tok::Type);
            else if ((language == L::Cpp || language == L::JavaScript) &&
                     (std::isupper(static_cast<unsigned char>(c)) || scope))
                fill(out, i, j, Tok::Type);
            else if (call)
                fill(out, i, j, Tok::Preproc); // function names
            i = j;
            continue;
        }
        ++i;
    }
}

ImU32 tokColor(Tok tok, const Palette& p)
{
    switch (tok) {
    case Tok::Keyword: return p.synKeyword;
    case Tok::Type: return p.synType;
    case Tok::String: return p.synString;
    case Tok::Number: return p.synNumber;
    case Tok::Comment: return p.synComment;
    case Tok::Preproc: return p.synPreprocessor;
    case Tok::Heading: return p.synHeading;
    case Tok::Text: break;
    }
    return p.synText;
}

// ---- Columns ----------------------------------------------------------------------------------

int visualColumn(const std::string& s, int byteColumn)
{
    int v = 0;
    const int end = std::min(byteColumn, int(s.size()));
    for (int i = 0; i < end; ++i) {
        if (s[size_t(i)] == '\t')
            v = (v / kTabSize + 1) * kTabSize;
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
    : vm_(vm), fonts_(fonts), palette_(palette), s_(uiScale)
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
    for (int l = 0; l < lines; ++l) {
        const std::string& s = doc.buffer.line(l);
        firstRowOfLine_[size_t(l)] = int(rows_.size());
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
                const int nv = s[size_t(end)] == '\t' ? (v / kTabSize + 1) * kTabSize : v + 1;
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
        const int nv = s[size_t(i)] == '\t' ? (v / kTabSize + 1) * kTabSize : v + 1;
        if (target < (float(v) + float(nv)) * 0.5f)
            return i;
        v = nv;
        i = next;
    }
    return row.end;
}

TextPos EditorView::hitTest(const model::Document& doc, ImVec2 mouse, const Metrics& m) const
{
    if (rows_.empty())
        return {};
    const int r = std::clamp(int(std::floor((mouse.y - m.origin.y) / m.lineH)), 0, int(rows_.size()) - 1);
    const Row& row = rows_[size_t(r)];
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
    m.gutterW = m.markerW + (vm_.showLineNumbers.get() ? float(std::max(digits, 3)) * m.charW + 14.0f * s_ : 4.0f * s_);

    const bool wrap = vm_.wordWrap.get();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, palette_.editorBg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const std::string childId = "##editor" + std::to_string(doc->id);
    ImGui::BeginChild(childId.c_str(), size, ImGuiChildFlags_None,
                      (wrap ? ImGuiWindowFlags_None : ImGuiWindowFlags_HorizontalScrollbar) | ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
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
    if (revealCaret_)
        revealCaret(*doc, m);

    const int firstRow = std::clamp(int((ImGui::GetScrollY() - kTopPadding * s_) / m.lineH), 0, int(rows_.size()) - 1);
    const int lastRow = std::min(int(rows_.size()) - 1, firstRow + m.visibleRows + 1);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(ImVec2(winPos.x + m.gutterW, winPos.y), m.viewMax, true);
    drawRows(*doc, m, firstRow, lastRow);
    dl->PopClipRect();
    drawGutter(*doc, m, firstRow, lastRow);
    if (hovered && !dragging_)
        drawHoverTooltip(*doc, m);

    // Scrollable extent: every row plus half a page of slack below the last line.
    const float contentW = wrap ? 1.0f : m.gutterW + 6.0f * s_ + float(maxColumns_ + 4) * m.charW;
    ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
    ImGui::Dummy(ImVec2(contentW, float(rows_.size()) * m.lineH + viewH * 0.5f));
    ImGui::EndChild();
    ImGui::PopFont();
}

void EditorView::renderEmptyState(ImVec2 size)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, palette_.editorBg);
    ImGui::BeginChild("##editorEmpty", size);
    ImGui::PopStyleColor();
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

void EditorView::renderFindBar()
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, palette_.panelHeaderBg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * s_, 5.0f * s_));
    const float rows = vm_.replaceVisible.get() ? 2.0f : 1.0f;
    const float h = rows * ImGui::GetFrameHeightWithSpacing() - ImGui::GetStyle().ItemSpacing.y + 10.0f * s_;
    ImGui::BeginChild("##findBar", ImVec2(0.0f, h), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

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

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        lastActivity_ = ImGui::GetTime();
        preferredVcol_ = -1.0f;
        const TextPos pos = hitTest(doc, io.MousePos, m);
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
            vm_.insertText(std::string(size_t(kTabSize - visualColumn(buffer.line(caret.line), caret.column) % kTabSize), ' '));
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

    // Block-comment state at the first visible line.
    bool inBlock = false;
    std::vector<Tok> toks;
    const int firstLine = rows_[size_t(firstRow)].line;
    for (int l = 0; l < firstLine; ++l)
        highlight(doc.buffer.line(l), doc.language, inBlock, toks);

    const std::vector<const model::ReviewFinding*> findings = vm_.review().findingsForFile(doc.path);
    const auto& matches = vm_.findMatches();
    const int currentMatch = vm_.currentMatch.get();
    const TextPos selStart = doc.selectionStart();
    const TextPos selEnd = doc.selectionEnd();
    int lastLine = -1;

    for (int r = firstRow; r <= lastRow; ++r) {
        const Row& row = rows_[size_t(r)];
        const std::string& text = doc.buffer.line(row.line);
        if (row.line != lastLine) {
            highlight(text, doc.language, inBlock, toks);
            lastLine = row.line;
        }
        const float y = m.origin.y + float(r) * m.lineH;
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
                for (int k = kTabSize; k < leadCols; k += kTabSize) {
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
            const Tok tok = toks[size_t(b)];
            int e = b + 1;
            while (e < row.end && toks[size_t(e)] == tok && text[size_t(e)] != '\t')
                ++e;
            if (text[size_t(b)] == '\t') {
                const int abs = rowStartCol + vcol;
                const int next = (abs / kTabSize + 1) * kTabSize;
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
            dl->AddText(font, fontSize, ImVec2(m.origin.x + float(vcol) * m.charW, textY), tokColor(tok, palette_),
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
    const bool blinkOn = std::fmod(ImGui::GetTime() - lastActivity_, 1.0) < 0.6;
    if (focused_ && blinkOn) {
        const int r = rowOf(doc.caret);
        if (r >= firstRow && r <= lastRow) {
            const float x = std::floor(rowX(doc, rows_[size_t(r)], doc.caret.column, m));
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
    for (int r = firstRow; r <= lastRow; ++r) {
        const Row& row = rows_[size_t(r)];
        if (firstRowOfLine_[size_t(row.line)] != r)
            continue;
        const float y = m.origin.y + float(r) * m.lineH;
        const float textY = y + (m.lineH - fontSize) * 0.5f;
        if (vm_.showLineNumbers.get()) {
            char number[16];
            std::snprintf(number, sizeof(number), "%d", row.line + 1);
            const float w = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, number).x;
            dl->AddText(font, fontSize, ImVec2(m.viewMin.x + m.gutterW - 10.0f * s_ - w, textY),
                        row.line == doc.caret.line ? palette_.gutterTextCurrent : palette_.gutterText, number);
        }

        const float cx = m.viewMin.x + m.markerW * 0.5f + 1.0f * s_;
        const float cy = y + m.lineH * 0.5f;
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

} // namespace enp::view
