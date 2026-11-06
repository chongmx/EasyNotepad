#include "enp/view/DataViews.h"

#include "enp/os/Memory.h"
#include "enp/view/PanelFrame.h"
#include "enp/view/TableReset.h"
#include "enp/view/UiAutomation.h"
#include "enp/view/UiStyle.h"

#include "IconsFontAwesome6.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace enp::view {

namespace data = model::data;
using model::DataView;

namespace {

bool isDataLanguage(model::Language l) { return l == model::Language::Json || l == model::Language::Yaml; }

// A number when the whole cell reads as one (for sorting), else NAN.
double numberOf(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.remove_suffix(1);
    if (s.empty() || s.size() > 63)
        return NAN;
    char buffer[64];
    std::memcpy(buffer, s.data(), s.size());
    buffer[s.size()] = '\0';
    char* end = nullptr;
    const double v = std::strtod(buffer, &end);
    return end == buffer + s.size() ? v : NAN;
}

} // namespace

DataViews::DataViews(viewmodel::NotepadViewModel& vm, const Fonts& fonts, const Palette& palette, float uiScale)
    : vm_(vm), fonts_(fonts), palette_(palette), s_(uiScale)
{
}

// ---- Entries ------------------------------------------------------------------------------------

DataViews::Entry& DataViews::entryFor(model::Document& doc, bool table)
{
    Entry& e = entries_[doc.id];
    if (e.revision == doc.revision && e.table == table)
        return e;
    os::mem::Scope scope("data-views");
    e = Entry{}; // the previous parse goes before the new one is built
    e.revision = doc.revision;
    e.table = table;
    const std::string text = doc.buffer.text();
    if (table) {
        e.csv = model::csv::parse(text, model::csv::detectDelimiter(text, doc.path));
    } else {
        e.tree = doc.language == model::Language::Json ? data::parseJson(text) : data::parseYaml(text);
        e.open.assign(e.tree.nodes.size(), 0);
        if (!e.open.empty())
            e.open[0] = 1; // the root, open
    }
    return e;
}

void DataViews::release(int documentId)
{
    if (auto it = entries_.find(documentId); it != entries_.end())
        entries_.erase(it);
}

void DataViews::releaseClosed()
{
    for (auto it = entries_.begin(); it != entries_.end();)
        it = vm_.documentIndexById(it->first) < 0 ? entries_.erase(it) : std::next(it);
}

void DataViews::resetScrollFor(int documentId)
{
    if (documentId != shownDoc_) { // another document in the shared windows: start at the top
        ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
        shownDoc_ = documentId;
    }
}

// ---- The switch -------------------------------------------------------------------------------------

bool DataViews::hasSwitch(const model::Document& doc)
{
    return doc.kind == model::FileKind::Text && (doc.language == model::Language::Csv || isDataLanguage(doc.language));
}

ImRect DataViews::switchRect(const model::Document& doc, ImVec2 topRight) const
{
    const float w = (isDataLanguage(doc.language) ? 3.0f : 2.0f) * 30.0f * s_, h = 24.0f * s_; // icons only
    return ImRect(ImVec2(topRight.x - w, topRight.y), ImVec2(topRight.x, topRight.y + h));
}

void DataViews::viewSwitch(model::Document& doc, ImVec2 topRight)
{
    const ImRect r = switchRect(doc, topRight);
    const bool data = isDataLanguage(doc.language);
    struct Option {
        const char* label;
        const char* id;
        const char* tooltip;
        int value;
    };
    // Icons only; what each one is shows on hover.
    static const Option csvOptions[] = {{ICON_FA_FILE_LINES, "data-view:text", "Text: the file to edit (Ctrl+Shift+V)", 0},
                                        {ICON_FA_TABLE, "data-view:table", "Table: the rows as a table (Ctrl+Shift+V)", 1}};
    static const Option dataOptions[] = {{ICON_FA_FILE_LINES, "data-view:text", "Text: the file to edit (Ctrl+Shift+V cycles)", 0},
                                         {ICON_FA_SITEMAP, "data-view:tree", "Tree: keys as a tree, the chosen value beside it", 1},
                                         {ICON_FA_LIST, "data-view:accordion", "Accordion: nested sections that open and close", 2}};
    const Option* options = data ? dataOptions : csvOptions;
    const int count = data ? 3 : 2;
    const int current = data ? int(doc.dataView) : (doc.rendered ? 1 : 0);
    const float segment = r.GetWidth() / float(count);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f * s_);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    for (int i = 0; i < count; ++i) {
        const bool active = i == current;
        ImGui::SetCursorScreenPos(ImVec2(r.Min.x + segment * float(i), r.Min.y));
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(active ? palette_.accentSoft : palette_.panelHeaderBg));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(active ? palette_.text : palette_.textDim));
        std::snprintf(label_, sizeof(label_), "%s##%s", options[i].label, options[i].id);
        const int value = options[i].value;
        auto choose = [this, data, value]() {
            model::Document* d = vm_.activeDocument();
            if (!d)
                return;
            if (data) {
                vm_.setDataView(DataView(value));
            } else if (d->rendered != (value == 1)) {
                vm_.execute(viewmodel::CommandId::ToggleMarkdownView);
            }
        };
        if (ImGui::Button(label_, ImVec2(segment, r.GetHeight())) && !active)
            choose();
        ImGui::PopStyleColor(2);
        ui::itemTooltip("%s", options[i].tooltip);
        registerLastItem(options[i].id, "button", options[i].tooltip, choose);
    }
    ImGui::PopStyleVar(2);
}

float DataViews::toolbar(model::Document& doc, const char* summary, float width)
{
    const float h = 32.0f * s_;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h), palette_.panelHeaderBg);
    dl->AddLine(ImVec2(p.x, p.y + h - 1.0f), ImVec2(p.x + width, p.y + h - 1.0f), palette_.border);
    const ImRect sw = switchRect(doc, ImVec2(p.x + width - 8.0f * s_, p.y + 4.0f * s_));
    dl->PushClipRect(p, ImVec2(sw.Min.x - 8.0f * s_, p.y + h), true);
    dl->AddText(ImVec2(p.x + 10.0f * s_, p.y + (h - ImGui::GetTextLineHeight()) * 0.5f), palette_.textDim, summary);
    dl->PopClipRect();
    viewSwitch(doc, ImVec2(p.x + width - 8.0f * s_, p.y + 4.0f * s_));
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
    return h;
}

bool DataViews::parseError(model::Document& doc, const Entry& e, ImVec2 size)
{
    if (e.tree.ok())
        return false;
    const char* what = doc.language == model::Language::Json ? "JSON" : "YAML";
    std::snprintf(label_, sizeof(label_), "%s  %s", ICON_FA_TRIANGLE_EXCLAMATION, what);
    toolbar(doc, label_, size.x);
    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + 16.0f * s_, ImGui::GetCursorPosY() + 16.0f * s_));
    ImGui::BeginGroup();
    ImGui::Text("This %s cannot be shown as a tree: line %d: %s.", what, e.tree.errorLine + 1, e.tree.error.c_str());
    ImGui::TextDisabled("Fix the text and come back, or keep reading it as text.");
    ImGui::Spacing();
    const int line = e.tree.errorLine;
    if (ImGui::Button(ICON_FA_FILE_LINES "  Go to the line")) {
        vm_.setDataView(DataView::Text);
        vm_.goToLine(std::max(0, line));
    }
    registerLastItem("data-view:error-line", "button", "Go to the line", [this, line]() {
        vm_.setDataView(DataView::Text);
        vm_.goToLine(std::max(0, line));
    });
    ImGui::EndGroup();
    return true;
}

ImU32 DataViews::valueColor(data::Kind kind) const
{
    switch (kind) {
    case data::Kind::String: return palette_.syntax[size_t(model::Token::String)];
    case data::Kind::Number: return palette_.syntax[size_t(model::Token::Number)];
    case data::Kind::Bool:
    case data::Kind::Null: return palette_.syntax[size_t(model::Token::Keyword)];
    default: return palette_.textDim;
    }
}

// ---- Table --------------------------------------------------------------------------------------

void DataViews::sortTable(Entry& e, const ImGuiTableSortSpecs* specs)
{
    const size_t first = e.header ? 1 : 0;
    const size_t rows = e.csv.rowCount() > first ? e.csv.rowCount() - first : 0;
    if (!specs || specs->SpecsCount == 0 || rows == 0) {
        os::mem::release(e.order); // file order
        return;
    }
    const ImGuiTableColumnSortSpecs& spec = specs->Specs[0];
    const int column = spec.ColumnIndex - 1; // column 0 is the row number
    const bool ascending = spec.SortDirection != ImGuiSortDirection_Descending;
    os::mem::Scope scope("data-views");
    e.order.resize(rows);
    for (size_t i = 0; i < rows; ++i)
        e.order[i] = std::uint32_t(first + i);
    if (column < 0) {
        if (!ascending)
            std::reverse(e.order.begin(), e.order.end());
        return;
    }
    std::stable_sort(e.order.begin(), e.order.end(), [&](std::uint32_t a, std::uint32_t b) {
        const std::string_view x = e.csv.cell(a, column), y = e.csv.cell(b, column);
        const double nx = numberOf(x), ny = numberOf(y);
        bool less;
        if (!std::isnan(nx) && !std::isnan(ny))
            less = ascending ? nx < ny : nx > ny;
        else if (std::isnan(nx) != std::isnan(ny))
            less = !std::isnan(nx); // numbers before text, either way
        else
            less = ascending ? x < y : x > y;
        return less;
    });
}

void DataViews::renderTable(model::Document& doc, ImVec2 size)
{
    Entry& e = entryFor(doc, true);
    const model::csv::Table& t = e.csv;
    const char* delimiter = t.delimiter == ',' ? "comma" : t.delimiter == ';' ? "semicolon" : t.delimiter == '\t' ? "tab" : "bar";
    const size_t first = e.header ? 1 : 0;
    const size_t dataRows = t.rowCount() > first ? t.rowCount() - first : 0;
    const int columns = std::min(t.columns, IMGUI_TABLE_MAX_COLUMNS - 1);
    std::snprintf(label_, sizeof(label_), "%s  CSV  \xC2\xB7  %zu rows  \xC2\xB7  %d columns%s  \xC2\xB7  %s", ICON_FA_TABLE, dataRows, t.columns,
                  t.columns > columns ? " (the first 511 shown)" : "", delimiter);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float summaryW = ImGui::CalcTextSize(label_).x; // before the toolbar: its switch reuses label_
    const float bar = toolbar(doc, label_, size.x);
    // "First row is header" sits in the toolbar after the summary.
    {
        const float w = summaryW + 30.0f * s_;
        ImGui::SetCursorScreenPos(ImVec2(start.x + w, start.y + (bar - ImGui::GetFrameHeight()) * 0.5f));
        if (ImGui::Checkbox("First row is the header", &e.header))
            os::mem::release(e.order);
        registerLastItem("data-view:header", "checkbox", "First row is the header", [&e]() {
            e.header = !e.header;
            os::mem::release(e.order);
        });
        ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + bar));
    }
    if (t.rowCount() == 0 || columns == 0) {
        ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + 16.0f * s_, ImGui::GetCursorPosY() + 16.0f * s_));
        ImGui::TextDisabled("The file has no rows.");
        return;
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuterH |
                                  ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate;
    const bool switched = doc.id != shownDoc_;
    resetScrollFor(doc.id);
    // One table for every document (Dear ImGui keeps a table object per id for as long as it runs).
    if (!ImGui::BeginTable("##csvTable", columns + 1, flags, ImVec2(size.x, std::max(1.0f, size.y - bar))))
        return;
    ImGui::TableSetupScrollFreeze(1, 1);
    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort, 0.0f);
    for (int c = 0; c < columns; ++c) {
        if (e.header) {
            const std::string_view name = t.cell(0, c);
            const size_t n = std::min(name.size(), sizeof(label_) - 16);
            std::memcpy(label_, name.data(), n);
            std::snprintf(label_ + n, sizeof(label_) - n, "##c%d", c);
            if (n == 0)
                std::snprintf(label_, sizeof(label_), "(column %d)##c%d", c + 1, c);
        } else {
            std::snprintf(label_, sizeof(label_), "Column %d##c%d", c + 1, c);
        }
        ImGui::TableSetupColumn(label_, ImGuiTableColumnFlags_WidthFixed, 0.0f);
    }
    if (switched) { // another document: its own widths and sort, not the last one's
        resetTableColumns();
        os::mem::release(e.order);
    }
    ImGui::TableHeadersRow();
    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs(); specs && specs->SpecsDirty) {
        sortTable(e, specs);
        specs->SpecsDirty = false;
    }

    ImGuiListClipper clipper;
    clipper.Begin(int(dataRows));
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const size_t row = e.order.empty() ? first + size_t(i) : e.order[size_t(i)];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            std::snprintf(label_, sizeof(label_), "%zu", row + 1 - first);
            ImGui::TextDisabled("%s", label_);
            for (int c = 0; c < columns; ++c) {
                if (!ImGui::TableNextColumn())
                    continue; // scrolled out of view: nothing drawn
                const std::string_view cell = t.cell(row, c);
                if (cell.empty())
                    continue;
                // One line per row: a cell with line breaks shows its first, all of it on hover.
                const size_t nl = cell.find('\n');
                const std::string_view shown = cell.substr(0, nl);
                ImGui::TextUnformatted(shown.data(), shown.data() + shown.size());
                const bool cut = nl != std::string_view::npos || ImGui::GetItemRectSize().x > ImGui::GetColumnWidth();
                if (nl != std::string_view::npos) {
                    ImGui::SameLine(0.0f, 2.0f * s_);
                    ImGui::TextDisabled("\xE2\x80\xA6");
                }
                if (cut && ImGui::IsItemHovered()) {
                    const int n = int(std::min<size_t>(cell.size(), 4000));
                    ui::tooltip("%.*s", n, cell.data());
                }
            }
        }
    }
    ImGui::EndTable();
}

// ---- Tree ---------------------------------------------------------------------------------------

void DataViews::rebuildRows(Entry& e)
{
    os::mem::Scope scope("data-views");
    e.rows.clear();
    e.depth.clear();
    if (e.tree.nodes.empty())
        return;
    // Depth-first, children in order: a stack of the next sibling to visit per level.
    std::vector<std::pair<std::uint32_t, std::uint16_t>> stack{{0u, std::uint16_t(0)}};
    while (!stack.empty()) {
        const auto [n, d] = stack.back();
        stack.pop_back();
        e.rows.push_back(n);
        e.depth.push_back(d);
        const data::Node& node = e.tree.nodes[n];
        if (node.nextSibling != data::kNone && d > 0)
            stack.push_back({node.nextSibling, d});
        if (node.container() && e.open[n] && node.firstChild != data::kNone)
            stack.push_back({node.firstChild, std::uint16_t(std::min(d + 1, 65535))});
    }
    e.rowsDirty = false;
}

void DataViews::select(Entry& e, std::uint32_t node)
{
    e.selected = node;
    for (std::uint32_t p = e.tree.nodes[node].parent; p != data::kNone; p = e.tree.nodes[p].parent)
        if (!e.open[p]) {
            e.open[p] = 1;
            e.rowsDirty = true;
        }
    e.scrollToSelected = true;
}

void DataViews::renderTree(model::Document& doc, ImVec2 size)
{
    Entry& e = entryFor(doc, false);
    if (parseError(doc, e, size))
        return;
    const data::Tree& t = e.tree;
    std::snprintf(label_, sizeof(label_), "%s  %s  \xC2\xB7  %zu values", ICON_FA_SITEMAP,
                  doc.language == model::Language::Json ? "JSON" : "YAML", t.nodes.size());
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float summaryW = ImGui::CalcTextSize(label_).x; // before the toolbar: its switch reuses label_
    const float bar = toolbar(doc, label_, size.x);
    {
        // Expand / collapse all, after the summary.
        const float x = start.x + summaryW + 30.0f * s_;
        ImGui::SetCursorScreenPos(ImVec2(x, start.y + (bar - ImGui::GetFrameHeight()) * 0.5f));
        if (ImGui::SmallButton(ICON_FA_FOLDER_OPEN "  Expand all")) {
            std::fill(e.open.begin(), e.open.end(), std::uint8_t(1));
            e.rowsDirty = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_FOLDER "  Collapse all")) {
            std::fill(e.open.begin(), e.open.end(), std::uint8_t(0));
            e.open[0] = 1;
            e.rowsDirty = true;
        }
        ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + bar));
    }
    if (e.rowsDirty)
        rebuildRows(e);

    const float height = std::max(1.0f, size.y - bar);
    if (treeWidth_ <= 0.0f)
        treeWidth_ = 320.0f * s_;
    treeWidth_ = std::clamp(treeWidth_, 160.0f * s_, std::max(160.0f * s_, size.x - 200.0f * s_));
    const float rowH = ImGui::GetFrameHeight();
    const float indentW = 14.0f * s_;

    // ---- The keys ----
    resetScrollFor(doc.id);
    panel::beginChild("##dataTreeList", ImVec2(treeWidth_, height), palette_.editorBg, ImGuiChildFlags_None, ImGuiWindowFlags_NoNav);
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        // Keys: up/down move, right opens, left closes (or goes to the parent).
        if (ImGui::IsWindowFocused() && !e.rows.empty()) {
            int at = 0;
            for (size_t i = 0; i < e.rows.size(); ++i)
                if (e.rows[i] == e.selected)
                    at = int(i);
            const data::Node& sel = t.nodes[e.selected];
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
                e.selected = e.rows[size_t(std::min(at + 1, int(e.rows.size()) - 1))];
                e.scrollToSelected = true;
            } else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
                e.selected = e.rows[size_t(std::max(at - 1, 0))];
                e.scrollToSelected = true;
            } else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && sel.container() && !e.open[e.selected]) {
                e.open[e.selected] = 1;
                e.rowsDirty = true;
            } else if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
                if (sel.container() && e.open[e.selected] && e.selected != 0) {
                    e.open[e.selected] = 0;
                    e.rowsDirty = true;
                } else if (sel.parent != data::kNone) {
                    select(e, sel.parent);
                }
            }
        }
        ImGuiListClipper clipper;
        clipper.Begin(int(e.rows.size()), rowH);
        if (e.scrollToSelected) {
            for (size_t i = 0; i < e.rows.size(); ++i)
                if (e.rows[i] == e.selected)
                    clipper.IncludeItemByIndex(int(i));
        }
        std::uint32_t toggle = data::kNone;
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const std::uint32_t n = e.rows[size_t(i)];
                const data::Node& node = t.nodes[n];
                const float x0 = ImGui::GetCursorScreenPos().x + float(e.depth[size_t(i)]) * indentW;
                ImGui::PushID(int(n));
                const ImVec2 p = ImGui::GetCursorScreenPos();
                if (ImGui::Selectable("##row", e.selected == n, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0.0f, rowH))) {
                    const bool onArrow = ImGui::GetIO().MousePos.x < x0 + indentW + 2.0f * s_;
                    if (node.container() && (onArrow || ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)))
                        toggle = n;
                    e.selected = n;
                }
                if (e.scrollToSelected && e.selected == n) {
                    ImGui::SetScrollHereY(0.4f);
                    e.scrollToSelected = false;
                }
                ImGui::PopID();
                const float ty = p.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f;
                if (node.container())
                    dl->AddText(ImVec2(x0 + 2.0f * s_, ty), palette_.textDim, e.open[n] ? ICON_FA_CARET_DOWN : ICON_FA_CARET_RIGHT);
                float x = x0 + indentW + 4.0f * s_;
                // The key: a name, an index, or "(root)".
                const std::string_view key = t.key(node);
                if (node.parent == data::kNone)
                    std::snprintf(label_, sizeof(label_), "(root)");
                else if (t.nodes[node.parent].kind == data::Kind::Array)
                    std::snprintf(label_, sizeof(label_), "[%u]", t.indexInParent(n));
                else
                    std::snprintf(label_, sizeof(label_), "%.*s", int(std::min<size_t>(key.size(), 200)), key.data());
                dl->AddText(ImVec2(x, ty), palette_.syntax[size_t(model::Token::Key)], label_);
                x += ImGui::CalcTextSize(label_).x + 8.0f * s_;
                // A preview: the value, or the size of a container.
                if (node.container())
                    std::snprintf(label_, sizeof(label_), node.kind == data::Kind::Object ? "{%u}" : "[%u]", node.childCount);
                else {
                    const std::string_view v = t.value(node);
                    const size_t nl = v.find('\n');
                    std::snprintf(label_, sizeof(label_), "%.*s%s", int(std::min<size_t>(std::min(v.size(), nl), 120)), v.data(),
                                  nl != std::string_view::npos || v.size() > 120 ? "\xE2\x80\xA6" : "");
                }
                dl->AddText(ImVec2(x, ty), node.container() ? palette_.textFaint : valueColor(node.kind), label_);
            }
        }
        if (toggle != data::kNone) {
            e.open[toggle] = e.open[toggle] ? 0 : 1;
            e.rowsDirty = true;
        }
    }
    ImGui::EndChild();

    // ---- The splitter ----
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::InvisibleButton("##treeSplit", ImVec2(6.0f * s_, height));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActive())
        treeWidth_ += ImGui::GetIO().MouseDelta.x;
    ImGui::SameLine(0.0f, 0.0f);

    // ---- The chosen value ----
    panel::beginChild("##dataTreeValue", ImVec2(0.0f, height), palette_.editorBg, ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoNav);
    {
        const std::uint32_t n = std::min<std::uint32_t>(e.selected, std::uint32_t(t.nodes.size() - 1));
        const data::Node& node = t.nodes[n];
        const std::string path = t.path(n); // one string while drawn (the path changes as the choice does)
        ImGui::TextDisabled("PATH");
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(path.c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::SmallButton(ICON_FA_COPY "  Copy path"))
            ImGui::SetClipboardText(path.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_FILE_LINES "  Show in text")) {
            vm_.setDataView(DataView::Text);
            vm_.goToLine(node.line);
        }
        const int line = node.line;
        registerLastItem("data-view:show-in-text", "button", "Show in text", [this, line]() {
            vm_.setDataView(DataView::Text);
            vm_.goToLine(line);
        });
        ImGui::Spacing();
        ImGui::TextDisabled("%s", data::kindName(node.kind));
        ImGui::SameLine();
        if (node.container())
            ImGui::TextDisabled("\xC2\xB7  %u %s  \xC2\xB7  line %d", node.childCount,
                                node.kind == data::Kind::Object ? (node.childCount == 1 ? "key" : "keys") : (node.childCount == 1 ? "item" : "items"),
                                node.line + 1);
        else
            ImGui::TextDisabled("\xC2\xB7  line %d", node.line + 1);
        ImGui::Separator();
        if (!node.container()) {
            const std::string_view v = t.value(node);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(valueColor(node.kind)));
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(v.data(), v.data() + v.size());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            if (ImGui::SmallButton(ICON_FA_COPY "  Copy value"))
                ImGui::SetClipboardText(std::string(v).c_str());
        } else if (node.childCount > 0 && ImGui::BeginTable("##children", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                                                                ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
                                                            ImGui::GetContentRegionAvail())) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(node.kind == data::Kind::Object ? "Key" : "Index", ImGuiTableColumnFlags_WidthFixed, 140.0f * s_);
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 70.0f * s_);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            // Children are a linked list: walk to the first visible one, then draw the visible ones.
            ImGuiListClipper clipper;
            clipper.Begin(int(node.childCount));
            std::uint32_t pick = data::kNone;
            while (clipper.Step()) {
                std::uint32_t c = node.firstChild;
                int index = 0;
                for (; c != data::kNone && index < clipper.DisplayStart; c = t.nodes[c].nextSibling)
                    ++index;
                for (; c != data::kNone && index < clipper.DisplayEnd; c = t.nodes[c].nextSibling, ++index) {
                    const data::Node& child = t.nodes[c];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::PushID(int(c));
                    if (node.kind == data::Kind::Array)
                        std::snprintf(label_, sizeof(label_), "[%d]", index);
                    else
                        std::snprintf(label_, sizeof(label_), "%.*s", int(std::min<size_t>(child.keyLength, 200)), t.key(child).data());
                    if (ImGui::Selectable(label_, false, ImGuiSelectableFlags_SpanAllColumns))
                        pick = c;
                    ImGui::PopID();
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextDisabled("%s", data::kindName(child.kind));
                    ImGui::TableSetColumnIndex(2);
                    if (child.container()) {
                        ImGui::TextDisabled(child.kind == data::Kind::Object ? "{%u}" : "[%u]", child.childCount);
                    } else {
                        const std::string_view v = t.value(child);
                        const std::string_view line0 = v.substr(0, v.find('\n'));
                        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(valueColor(child.kind)));
                        ImGui::TextUnformatted(line0.data(), line0.data() + line0.size());
                        ImGui::PopStyleColor();
                    }
                }
            }
            ImGui::EndTable();
            if (pick != data::kNone)
                select(e, pick);
        }
    }
    ImGui::EndChild();
}

// ---- Accordion ------------------------------------------------------------------------------------

void DataViews::accordionChildren(Entry& e, std::uint32_t node, int depth)
{
    const data::Tree& t = e.tree;
    const data::Node& parent = t.nodes[node];
    const auto extra = e.more.find(node);
    const std::uint32_t limit = kShownChildren + (extra == e.more.end() ? 0 : extra->second);
    const float rowH = ImGui::GetFrameHeight();
    const float keyW = std::min(220.0f * s_, ImGui::GetContentRegionAvail().x * 0.35f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    std::uint32_t index = 0;
    for (std::uint32_t c = parent.firstChild; c != data::kNone; c = t.nodes[c].nextSibling, ++index) {
        if (index >= limit) {
            std::snprintf(label_, sizeof(label_), ICON_FA_ANGLES_DOWN "  Show %u more (%u not shown)##more%u",
                          std::min<std::uint32_t>(kShownChildren, parent.childCount - index), parent.childCount - index, node);
            if (ImGui::SmallButton(label_))
                e.more[node] += kShownChildren;
            break;
        }
        const data::Node& child = t.nodes[c];
        if (parent.kind == data::Kind::Array)
            std::snprintf(label_, sizeof(label_), "[%u]", index);
        else
            std::snprintf(label_, sizeof(label_), "%.*s", int(std::min<size_t>(child.keyLength, 200)), t.key(child).data());
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const float ty = p.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f;
        ImGui::PushID(int(c));
        if (child.container()) {
            // A section header: the whole bar opens and closes it.
            const bool clicked = ImGui::InvisibleButton("##section", ImVec2(std::max(1.0f, width), rowH));
            const bool hovered = ImGui::IsItemHovered();
            dl->AddRectFilled(p, ImVec2(p.x + width, p.y + rowH), hovered ? palette_.hover : palette_.panelHeaderBg, 4.0f * s_);
            dl->AddText(ImVec2(p.x + 6.0f * s_, ty), palette_.textDim, e.open[c] ? ICON_FA_CARET_DOWN : ICON_FA_CARET_RIGHT);
            dl->AddText(ImVec2(p.x + 22.0f * s_, ty), palette_.syntax[size_t(model::Token::Key)], label_);
            const float kx = p.x + 22.0f * s_ + ImGui::CalcTextSize(label_).x + 10.0f * s_;
            std::snprintf(label_, sizeof(label_), child.kind == data::Kind::Object ? "{%u}" : "[%u]", child.childCount);
            dl->AddText(ImVec2(kx, ty), palette_.textFaint, label_);
            if (clicked)
                e.open[c] = e.open[c] ? 0 : 1;
            ImGui::PopID();
            if (e.open[c]) {
                ImGui::Indent(16.0f * s_);
                accordionChildren(e, c, depth + 1);
                ImGui::Unindent(16.0f * s_);
            }
            continue;
        }
        // A value: key on the left, the value beside it (one line; all of it on hover).
        ImGui::Dummy(ImVec2(std::max(1.0f, width), rowH));
        const bool hovered = ImGui::IsItemHovered();
        dl->PushClipRect(p, ImVec2(p.x + keyW - 6.0f * s_, p.y + rowH), true);
        dl->AddText(ImVec2(p.x + 6.0f * s_, ty), palette_.textDim, label_);
        dl->PopClipRect();
        const std::string_view v = t.value(child);
        const std::string_view line0 = v.substr(0, v.find('\n'));
        dl->PushClipRect(ImVec2(p.x + keyW, p.y), ImVec2(p.x + width, p.y + rowH), true);
        dl->AddText(ImVec2(p.x + keyW, ty), valueColor(child.kind), line0.data(), line0.data() + line0.size());
        dl->PopClipRect();
        if (hovered && (line0.size() != v.size() || ImGui::CalcTextSize(line0.data(), line0.data() + line0.size()).x > width - keyW))
            ui::tooltip("%.*s", int(std::min<size_t>(v.size(), 4000)), v.data());
        ImGui::PopID();
    }
}

void DataViews::renderAccordion(model::Document& doc, ImVec2 size)
{
    Entry& e = entryFor(doc, false);
    if (parseError(doc, e, size))
        return;
    const data::Tree& t = e.tree;
    std::snprintf(label_, sizeof(label_), "%s  %s  \xC2\xB7  %zu values", ICON_FA_LIST, doc.language == model::Language::Json ? "JSON" : "YAML",
                  t.nodes.size());
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float summaryW = ImGui::CalcTextSize(label_).x; // before the toolbar: its switch reuses label_
    const float bar = toolbar(doc, label_, size.x);
    {
        const float x = start.x + summaryW + 30.0f * s_;
        ImGui::SetCursorScreenPos(ImVec2(x, start.y + (bar - ImGui::GetFrameHeight()) * 0.5f));
        if (ImGui::SmallButton(ICON_FA_FOLDER_OPEN "  Expand all")) {
            std::fill(e.open.begin(), e.open.end(), std::uint8_t(1));
            e.rowsDirty = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_FOLDER "  Collapse all")) {
            std::fill(e.open.begin(), e.open.end(), std::uint8_t(0));
            e.open[0] = 1;
            e.rowsDirty = true;
        }
        ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + bar));
    }
    resetScrollFor(doc.id);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f * s_, 10.0f * s_));
    panel::beginChild("##dataAccordion", ImVec2(size.x, std::max(1.0f, size.y - bar)), palette_.editorBg,
                      ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 2.0f * s_));
    const data::Node& root = t.nodes[0];
    if (root.container()) {
        accordionChildren(e, 0, 0);
    } else {
        const std::string_view v = t.value(root);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(v.data(), v.data() + v.size());
        ImGui::PopTextWrapPos();
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
}

} // namespace enp::view
