#include "enp/view/LibraryView.h"

#include "enp/os/Memory.h"
#include "enp/view/PanelFrame.h"
#include "enp/view/TableReset.h"
#include "enp/view/UiAutomation.h"
#include "enp/view/UiStyle.h"

#include "IconsFontAwesome6.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cstring>

namespace enp::view {

namespace bin = model::bin;

namespace {

// The symbol table's columns (ImGui user ids; also the sort keys).
enum Column : int { Name, Scheme, Kind, Address, Offset, Size, Detail, Ordinal, Hint, LibraryName, MemberName, SectionName, Binding, ColumnCount };

const char* kindName(const bin::Symbol& s)
{
    switch (s.kind) {
        case bin::SymbolKind::Function: return "function";
        case bin::SymbolKind::Data: return "data";
        case bin::SymbolKind::ThreadLocal: return "thread-local";
        case bin::SymbolKind::Common: return "common";
        case bin::SymbolKind::Other: break;
    }
    return s.side == bin::Side::Import ? "" : "other";
}

const char* bindingName(bin::Binding b)
{
    switch (b) {
        case bin::Binding::Global: return "global";
        case bin::Binding::Weak: return "weak";
        case bin::Binding::Unique: return "unique";
        case bin::Binding::Local: return "local";
        case bin::Binding::None: break;
    }
    return "";
}

const char* shortScheme(bin::Mangling m)
{
    switch (m) {
        case bin::Mangling::Itanium: return "C++";
        case bin::Mangling::Microsoft: return "MSVC";
        case bin::Mangling::Rust: return "Rust";
        case bin::Mangling::D: return "D";
        case bin::Mangling::None: break;
    }
    return "";
}

char lower(char c) { return char(std::tolower(static_cast<unsigned char>(c))); }

bool containsNoCase(std::string_view text, std::string_view needle)
{
    if (needle.empty())
        return true;
    if (needle.size() > text.size())
        return false;
    for (size_t i = 0; i + needle.size() <= text.size(); ++i) {
        size_t k = 0;
        while (k < needle.size() && lower(text[i + k]) == lower(needle[k]))
            ++k;
        if (k == needle.size())
            return true;
    }
    return false;
}

int compareNoCase(std::string_view a, std::string_view b)
{
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i)
        if (const char x = lower(a[i]), y = lower(b[i]); x != y)
            return x < y ? -1 : 1;
    return a.size() < b.size() ? -1 : a.size() > b.size() ? 1 : 0;
}

// Numbers that are not recorded (kNone, kNoIndex) sort after every recorded one.
int compareNumber(std::uint64_t a, std::uint64_t b) { return a < b ? -1 : a > b ? 1 : 0; }
std::uint64_t orNone(std::uint32_t v) { return v == bin::kNoIndex ? bin::kNone : v; }

void copy(std::string_view text) { ImGui::SetClipboardText(std::string(text).c_str()); }

} // namespace

LibraryView::LibraryView(viewmodel::NotepadViewModel& vm, const Palette& palette, float uiScale)
    : vm_(vm), palette_(palette), s_(uiScale)
{
}

// ---- Entries ------------------------------------------------------------------------------------

LibraryView::Entry& LibraryView::entryFor(model::Document& doc)
{
    if (auto it = entries_.find(doc.id); it != entries_.end())
        return it->second;
    os::mem::Scope scope("library");
    Entry& e = entries_[doc.id];
    e.id = doc.id;
    std::uint64_t total = 0;
    const std::optional<std::string> head = vm_.readFileRange(doc.path, 0, 4096, &total);
    if (!head || !bin::recognises(*head))
        return e; // not readable here: the caller shows its notice (the entry remembers, so no read per frame)
    const std::string path = doc.path;
    e.image = bin::read(
        [this, &path](std::uint64_t offset, size_t size, std::string& out) {
            std::optional<std::string> part = vm_.readFileRange(path, offset, size);
            if (!part)
                return false;
            out.swap(*part); // the previous block goes with `part`
            return true;
        },
        total);
    e.readable = true;
    const bin::Image& img = e.image;
    e.open.assign(img.lists.size(), 0);
    // Opened: imports per library (a few), not the members of a large archive.
    for (size_t i = 0; i + 1 < img.lists.size(); ++i)
        if (img.lists[i].depth == 0 && img.lists[i + 1].depth > 0) {
            size_t children = 0;
            for (size_t k = i + 1; k < img.lists.size() && img.lists[k].depth > 0; ++k)
                ++children;
            e.open[i] = children <= 12 ? 1 : 0;
        }
    // Start on the functions, as that is what one opens a library for; else the overview.
    e.list = 0;
    for (size_t i = 0; i < img.lists.size(); ++i)
        if (img.lists[i].type == bin::ListType::Symbols && img.lists[i].depth == 0 && img.lists[i].kinds == bin::KindFilter::Functions &&
            img.lists[i].count > 0) {
            e.list = std::uint32_t(i);
            break;
        }
    return e;
}

void LibraryView::release(int documentId)
{
    if (auto it = entries_.find(documentId); it != entries_.end())
        entries_.erase(it);
}

void LibraryView::releaseClosed()
{
    for (auto it = entries_.begin(); it != entries_.end();)
        it = vm_.documentIndexById(it->first) < 0 ? entries_.erase(it) : std::next(it);
}

void LibraryView::choose(Entry& e, std::uint32_t list)
{
    if (list >= e.image.lists.size() || list == e.list)
        return;
    e.list = list;
    e.rowsDirty = true;
    e.selected = UINT32_MAX; // the table starts from this list's defaults (beginTable): no sort, its own columns
    // Its parent opens, so the choice can be seen in the tree.
    for (std::uint32_t p = list; p-- > 0;)
        if (e.image.lists[p].depth < e.image.lists[list].depth) {
            if (!e.open[p]) {
                e.open[p] = 1;
                e.treeDirty = true;
            }
            break;
        }
}

void LibraryView::resetFor(int documentId)
{
    if (documentId != shownDoc_) {
        shownDoc_ = documentId;
        shownList_ = UINT32_MAX;
    }
}

bool LibraryView::beginTable(const Entry& e, int columns, ImGuiTableFlags flags, ImVec2 size)
{
    // One table for every list of every document (Dear ImGui keeps a table and its window per id
    // for as long as it runs). Another list or another document starts from its own defaults:
    // columns with nothing in them hidden, widths fitted, no sort (columnsReady()).
    if (!ImGui::BeginTable("##libTable", columns, flags, size))
        return false;
    freshColumns_ = e.list != shownList_;
    return true;
}

void LibraryView::columnsReady()
{
    if (freshColumns_)
        resetTableColumns();
    freshColumns_ = false;
}

std::uint32_t LibraryView::usedColumns(const Entry& e) const
{
    // A bit per column that has a value in at least one row of the chosen list.
    const bin::Image& img = e.image;
    const bin::ListNode& node = img.lists[e.list];
    std::uint32_t used = 1u << Name;
    for (const bin::Symbol& s : img.symbols) {
        if (!bin::Image::matches(node, s))
            continue;
        used |= (s.mangling != bin::Mangling::None) << Scheme;
        used |= (*kindName(s) != '\0') << Kind;
        used |= (s.address != bin::kNone) << Address;
        used |= (s.offset != bin::kNone) << Offset;
        used |= (s.size != 0) << Size;
        used |= (s.detail.length != 0) << Detail;
        used |= (s.ordinal != bin::kNoIndex) << Ordinal;
        used |= (s.hint != bin::kNoIndex) << Hint;
        used |= (s.library != bin::kNoIndex) << LibraryName;
        used |= (s.member != bin::kNoIndex) << MemberName;
        used |= (s.section.length != 0) << SectionName;
        used |= (s.binding != bin::Binding::None) << Binding;
    }
    return used;
}

// ---- Rows ---------------------------------------------------------------------------------------

void LibraryView::rebuildTree(Entry& e)
{
    os::mem::Scope scope("library");
    std::vector<std::uint32_t> tree;
    bool parentOpen = true;
    for (std::uint32_t i = 0; i < e.image.lists.size(); ++i) {
        const bin::ListNode& n = e.image.lists[i];
        if (n.depth == 0) {
            tree.push_back(i);
            parentOpen = e.open[i] != 0;
        } else if (parentOpen) {
            tree.push_back(i);
        }
    }
    e.tree.swap(tree);
    e.treeDirty = false;
}

void LibraryView::rebuildRows(Entry& e)
{
    os::mem::Scope scope("library");
    const bin::Image& img = e.image;
    const bin::ListNode& node = img.lists[e.list];
    const std::string_view filter = e.filter;
    std::vector<std::uint32_t> rows;
    switch (node.type) {
        case bin::ListType::Symbols:
            rows.reserve(node.count);
            for (std::uint32_t i = 0; i < img.symbols.size(); ++i) {
                const bin::Symbol& s = img.symbols[i];
                if (bin::Image::matches(node, s) &&
                    (filter.empty() || containsNoCase(img.text(s.shown), filter) || containsNoCase(img.text(s.name), filter)))
                    rows.push_back(i);
            }
            break;
        case bin::ListType::Sections:
            for (std::uint32_t i = 0; i < img.sections.size(); ++i)
                if (containsNoCase(img.text(img.sections[i].name), filter))
                    rows.push_back(i);
            break;
        case bin::ListType::Libraries:
            for (std::uint32_t i = 0; i < img.libraries.size(); ++i)
                if (containsNoCase(img.text(img.libraries[i].name), filter))
                    rows.push_back(i);
            break;
        case bin::ListType::Members:
            for (std::uint32_t i = 0; i < img.members.size(); ++i)
                if (containsNoCase(img.text(img.members[i].name), filter))
                    rows.push_back(i);
            break;
        case bin::ListType::Overview: break;
    }
    if (e.sortColumn >= 0) {
        const int c = e.sortColumn;
        const bool raw = e.raw;
        auto cmp = [&](std::uint32_t a, std::uint32_t b) -> int {
            switch (node.type) {
                case bin::ListType::Symbols: {
                    const bin::Symbol &x = img.symbols[a], &y = img.symbols[b];
                    switch (c) {
                        case Name: return compareNoCase(img.text(raw ? x.name : x.shown), img.text(raw ? y.name : y.shown));
                        case Scheme: return compareNumber(std::uint64_t(x.mangling), std::uint64_t(y.mangling));
                        case Kind: return std::strcmp(kindName(x), kindName(y));
                        case Address: return compareNumber(x.address, y.address);
                        case Offset: return compareNumber(x.offset, y.offset);
                        case Size: return compareNumber(x.size, y.size);
                        case SectionName: return compareNoCase(img.text(x.section), img.text(y.section));
                        case Binding: return std::strcmp(bindingName(x.binding), bindingName(y.binding));
                        case Detail: return compareNoCase(img.text(x.detail), img.text(y.detail));
                        case Ordinal: return compareNumber(orNone(x.ordinal), orNone(y.ordinal));
                        case Hint: return compareNumber(orNone(x.hint), orNone(y.hint));
                        case LibraryName:
                            return compareNumber(orNone(x.library), orNone(y.library));
                        case MemberName: return compareNumber(orNone(x.member), orNone(y.member));
                    }
                    return 0;
                }
                case bin::ListType::Sections: {
                    const bin::Section &x = img.sections[a], &y = img.sections[b];
                    switch (c) {
                        case 0: return compareNoCase(img.text(x.name), img.text(y.name));
                        case 1: return compareNoCase(img.text(x.type), img.text(y.type));
                        case 2: return compareNumber(x.address, y.address);
                        case 3: return compareNumber(x.memorySize, y.memorySize);
                        case 4: return compareNumber(x.offset, y.offset);
                        case 5: return compareNumber(x.fileSize, y.fileSize);
                    }
                    return 0;
                }
                case bin::ListType::Libraries: {
                    const bin::Library &x = img.libraries[a], &y = img.libraries[b];
                    return c == 2 ? compareNumber(x.symbols, y.symbols) : compareNoCase(img.text(x.name), img.text(y.name));
                }
                case bin::ListType::Members: {
                    const bin::Member &x = img.members[a], &y = img.members[b];
                    switch (c) {
                        case 0: return compareNoCase(img.text(x.name), img.text(y.name));
                        case 1: return compareNoCase(img.text(x.format), img.text(y.format));
                        case 2: return compareNumber(x.offset, y.offset);
                        case 3: return compareNumber(x.size, y.size);
                        case 4: return compareNumber(x.symbols, y.symbols);
                    }
                    return 0;
                }
                case bin::ListType::Overview: break;
            }
            return 0;
        };
        const bool descending = e.sortDescending;
        std::stable_sort(rows.begin(), rows.end(), [&](std::uint32_t a, std::uint32_t b) {
            const int r = cmp(a, b);
            return descending ? r > 0 : r < 0;
        });
    }
    rows.shrink_to_fit();
    e.rows.swap(rows); // the previous list's rows go with `rows`
    e.rowsDirty = false;
}

void LibraryView::sortSpecs(Entry& e)
{
    ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs();
    if (!specs || !specs->SpecsDirty)
        return;
    if (specs->SpecsCount > 0) {
        e.sortColumn = int(specs->Specs[0].ColumnUserID);
        e.sortDescending = specs->Specs[0].SortDirection == ImGuiSortDirection_Descending;
    } else {
        e.sortColumn = -1;
    }
    specs->SpecsDirty = false;
    e.rowsDirty = true;
}

// ---- Drawing ------------------------------------------------------------------------------------

float LibraryView::tag(ImVec2 at, const char* text, ImU32 color)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize(); // the text's own size: another size would bake a second set of glyphs
    const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    const float pad = 4.0f * s_, h = ImGui::GetTextLineHeight();
    const ImVec2 max(at.x + ts.x + pad * 2.0f, at.y + h);
    dl->AddRectFilled(ImVec2(at.x, at.y + 1.0f * s_), ImVec2(max.x, max.y - 1.0f * s_), (color & 0x00FFFFFFu) | 0x30000000u, 3.0f * s_);
    dl->AddText(font, size, ImVec2(at.x + pad, at.y + (h - ts.y) * 0.5f), color, text);
    return max.x - at.x;
}

void LibraryView::toolbar(model::Document& doc, Entry& e, float width)
{
    const bin::Image& img = e.image;
    const float h = 32.0f * s_;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h), palette_.panelHeaderBg);
    dl->AddLine(ImVec2(p.x, p.y + h - 1.0f), ImVec2(p.x + width, p.y + h - 1.0f), palette_.border);

    // Right: the demangle switch and "Open as text".
    const int index = vm_.documentIndexById(doc.id);
    const char* textLabel = ICON_FA_FILE_LINES "  Open as text";
    const float textW = ImGui::CalcTextSize(textLabel).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const char* demangleLabel = "Demangle names";
    const float demangleW = img.mangled ? ImGui::CalcTextSize(demangleLabel).x + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x : 0.0f;
    const float right = p.x + width - 8.0f * s_;
    const float y = p.y + (h - ImGui::GetFrameHeight()) * 0.5f;
    float x = right - textW;
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    if (ImGui::Button(textLabel))
        vm_.openAsText(index);
    ui::itemTooltip("Show the bytes as text. Saving from the text view would rewrite the file.");
    registerLastItem("open-as-text", "button", "Open as text anyway", [this, index]() { vm_.openAsText(index); });
    if (img.mangled) {
        x -= demangleW + 14.0f * s_;
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        bool demangle = !e.raw;
        if (ImGui::Checkbox(demangleLabel, &demangle)) {
            e.raw = !demangle;
            e.rowsDirty = e.sortColumn == Name;
        }
        ui::itemTooltip("Mangled C++, Rust and D names shown readable (on) or as stored in the file (off)");
        // Actions find the entry again by id: one may run after the document closed.
        registerLastItem("library:demangle", "checkbox", "Demangle names", [this, id = e.id]() {
            if (auto it = entries_.find(id); it != entries_.end()) {
                it->second.raw = !it->second.raw;
                it->second.rowsDirty = true;
            }
        });
    }

    // Left: what the file is.
    std::snprintf(label_, sizeof(label_), "%s  %s  \xC2\xB7  %s symbols", ICON_FA_CUBES, img.summary.c_str(),
                  std::to_string(img.symbols.size()).c_str());
    if (img.mangled)
        std::snprintf(label_ + std::strlen(label_), sizeof(label_) - std::strlen(label_), "  \xC2\xB7  %u mangled, %u demangled", img.mangled,
                      img.demangledCount);
    dl->PushClipRect(p, ImVec2(x - 10.0f * s_, p.y + h), true);
    dl->AddText(ImVec2(p.x + 10.0f * s_, p.y + (h - ImGui::GetTextLineHeight()) * 0.5f), palette_.textDim, label_);
    dl->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
}

void LibraryView::treePanel(Entry& e, float height)
{
    const bin::Image& img = e.image;
    if (e.treeDirty)
        rebuildTree(e);
    panel::beginChild("##libTree", ImVec2(treeWidth_, height), palette_.editorBg, ImGuiChildFlags_None, ImGuiWindowFlags_NoNav);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rowH = ImGui::GetFrameHeight();
    const float indentW = 14.0f * s_;
    std::uint32_t toggle = UINT32_MAX, pick = UINT32_MAX;
    ImGuiListClipper clipper;
    clipper.Begin(int(e.tree.size()), rowH);
    while (clipper.Step()) {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
            const std::uint32_t i = e.tree[size_t(r)];
            const bin::ListNode& n = img.lists[i];
            const bool parent = n.depth == 0 && i + 1 < img.lists.size() && img.lists[i + 1].depth > 0;
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float x0 = p.x + float(n.depth) * indentW;
            ImGui::PushID(int(i));
            if (ImGui::Selectable("##node", e.list == i, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0.0f, rowH))) {
                const bool onArrow = ImGui::GetIO().MousePos.x < x0 + indentW + 2.0f * s_;
                if (parent && (onArrow || ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)))
                    toggle = i;
                if (!onArrow || !parent)
                    pick = i;
            }
            const std::string_view title = img.text(n.title);
            // Children can share a title (archive members of the same name): theirs carries the index.
            if (n.depth == 0)
                std::snprintf(label_, sizeof(label_), "library:list:%.*s", int(std::min<size_t>(title.size(), 200)), title.data());
            else
                std::snprintf(label_, sizeof(label_), "library:list:%.*s#%u", int(std::min<size_t>(title.size(), 200)), title.data(), i);
            registerLastItem(label_, "treeitem", std::string(title), [this, id = e.id, i]() {
                if (auto it = entries_.find(id); it != entries_.end())
                    choose(it->second, i);
            });
            ImGui::PopID();
            const float ty = p.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f;
            if (parent)
                dl->AddText(ImVec2(x0 + 2.0f * s_, ty), palette_.textDim, e.open[i] ? ICON_FA_CARET_DOWN : ICON_FA_CARET_RIGHT);
            const char* icon = ICON_FA_CODE;
            ImU32 color = palette_.iconBlue;
            switch (n.type) {
                case bin::ListType::Overview: icon = ICON_FA_CIRCLE_INFO, color = palette_.iconTeal; break;
                case bin::ListType::Sections: icon = ICON_FA_LAYER_GROUP, color = palette_.iconPurple; break;
                case bin::ListType::Libraries: icon = ICON_FA_LINK, color = palette_.iconGreen; break;
                case bin::ListType::Members: icon = ICON_FA_BOX_ARCHIVE, color = palette_.iconOrange; break;
                case bin::ListType::Symbols:
                    if (n.member != bin::kNoIndex)
                        icon = ICON_FA_FILE_CODE, color = palette_.iconOrange;
                    else if (n.library != bin::kNoIndex || n.noLibrary)
                        icon = ICON_FA_BOOK, color = palette_.iconGreen;
                    else if (n.side == bin::Side::Import)
                        icon = ICON_FA_RIGHT_TO_BRACKET, color = palette_.iconGreen;
                    else if (n.side == bin::Side::Local)
                        icon = ICON_FA_LOCK, color = palette_.iconGray;
                    else if (n.kinds == bin::KindFilter::Data)
                        icon = ICON_FA_DATABASE, color = palette_.iconPurple;
                    break;
            }
            float x = x0 + indentW + 4.0f * s_;
            dl->AddText(ImVec2(x, ty), color, icon);
            x += 20.0f * s_;
            // The count, right-aligned; the title, cut before it.
            const float right = p.x + ImGui::GetContentRegionAvail().x - 6.0f * s_;
            float countW = 0.0f;
            if (n.type != bin::ListType::Overview) {
                std::snprintf(label_, sizeof(label_), "%u", n.count);
                countW = ImGui::CalcTextSize(label_).x;
                dl->AddText(ImVec2(right - countW, ty), palette_.textFaint, label_);
            }
            dl->PushClipRect(ImVec2(x, p.y), ImVec2(std::max(x, right - countW - 6.0f * s_), p.y + rowH), true);
            dl->AddText(ImVec2(x, ty), palette_.text, title.data(), title.data() + title.size());
            dl->PopClipRect();
        }
    }
    if (toggle != UINT32_MAX) {
        e.open[toggle] = e.open[toggle] ? 0 : 1;
        e.treeDirty = true;
    }
    if (pick != UINT32_MAX)
        choose(e, pick);
    ImGui::EndChild();
}

void LibraryView::overview(Entry& e)
{
    const bin::Image& img = e.image;
    if (!img.error.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(palette_.iconOrange), "%s  %s", ICON_FA_TRIANGLE_EXCLAMATION, img.error.c_str());
        ImGui::PopTextWrapPos();
    }
    if (img.truncated)
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(palette_.iconOrange), "%s  Very large: the first %s symbols are listed.",
                           ICON_FA_TRIANGLE_EXCLAMATION, std::to_string(bin::kMaxSymbols).c_str());
    if (!beginTable(e, 2,
                    ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                    ImGui::GetContentRegionAvail()))
        return;
    const ImGuiID menu = ImGui::GetID("##libCopy"); // one menu for every row (a popup is a window, kept for good)
    ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 220.0f * s_);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
    columnsReady();
    for (size_t i = 0; i < img.overview.size(); ++i) {
        const bin::Property& p = img.overview[i];
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        const std::string_view key = img.text(p.key);
        if (p.heading) {
            if (i > 0)
                ImGui::Dummy(ImVec2(1.0f, 4.0f * s_));
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(palette_.accent));
            ImGui::TextUnformatted(key.data(), key.data() + key.size());
            ImGui::PopStyleColor();
            continue;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(palette_.textDim));
        ImGui::TextUnformatted(key.data(), key.data() + key.size());
        ImGui::PopStyleColor();
        ImGui::TableSetColumnIndex(1);
        const std::string_view value = img.text(p.value);
        ImGui::PushID(int(i));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(value.data(), value.data() + value.size());
        ImGui::PopTextWrapPos();
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            menuRow_ = std::uint32_t(i);
            ImGui::OpenPopup(menu);
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
    if (ImGui::BeginPopupEx(menu, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings)) {
        if (menuRow_ < img.overview.size() && ImGui::MenuItem(ICON_FA_COPY "  Copy"))
            copy(img.text(img.overview[menuRow_].value));
        ImGui::EndPopup();
    }
}

void LibraryView::symbolTable(Entry& e, float height)
{
    const bin::Image& img = e.image;
    const bool pe = img.format == bin::Format::Pe;
    const bool elf = img.format == bin::Format::Elf;
    const bool archive = img.format == bin::Format::Archive;
    bool objects = img.format == bin::Format::Coff || archive;
    for (const bin::Property& p : img.overview)
        if (elf && img.text(p.key) == "Type" && img.text(p.value).rfind("Relocatable", 0) == 0)
            objects = true;
    const ImGuiTableFlags flags = ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_Hideable |
                                  ImGuiTableFlags_Reorderable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                  ImGuiTableFlags_BordersOuterH | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Sortable |
                                  ImGuiTableFlags_SortTristate;
    if (!beginTable(e, ColumnCount, flags, ImVec2(0.0f, height)))
        return;
    if (freshColumns_)
        e.usedColumns = usedColumns(e);
    const ImGuiID menu = ImGui::GetID("##libRowMenu"); // one menu for every row (a popup is a window, kept for good)
    // In the order of Column. A column with nothing in it for this list starts hidden (the header's
    // menu shows it); PE bindings are all global, so they say nothing either.
    auto flagsOf = [&](int column, bool shown = true) {
        return ImGuiTableColumnFlags_WidthFixed | ((e.usedColumns >> column) & 1 && shown ? 0 : ImGuiTableColumnFlags_DefaultHide);
    };
    ImGui::TableSetupScrollFreeze(1, 1);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoHide, 460.0f * s_, Name);
    ImGui::TableSetupColumn("Mangling", flagsOf(Scheme), 0.0f, Scheme);
    ImGui::TableSetupColumn("Kind", flagsOf(Kind), 0.0f, Kind);
    ImGui::TableSetupColumn(pe ? "RVA" : objects ? "Value" : "Address", flagsOf(Address), 0.0f, Address);
    ImGui::TableSetupColumn("File offset", flagsOf(Offset), 0.0f, Offset);
    ImGui::TableSetupColumn("Size", flagsOf(Size), 0.0f, Size);
    ImGui::TableSetupColumn(elf ? "Version" : pe ? "Forwarded to" : archive ? "DLL / note" : "Note", flagsOf(Detail), 0.0f, Detail);
    ImGui::TableSetupColumn("Ordinal", flagsOf(Ordinal), 0.0f, Ordinal);
    ImGui::TableSetupColumn("Hint", flagsOf(Hint), 0.0f, Hint);
    ImGui::TableSetupColumn(pe ? "DLL" : "Library", flagsOf(LibraryName), 0.0f, LibraryName);
    ImGui::TableSetupColumn("Member", flagsOf(MemberName), 0.0f, MemberName);
    ImGui::TableSetupColumn("Section", flagsOf(SectionName), 0.0f, SectionName);
    ImGui::TableSetupColumn("Binding", flagsOf(Binding, !pe), 0.0f, Binding);
    columnsReady();
    ImGui::TableHeadersRow();
    sortSpecs(e);
    if (e.rowsDirty)
        rebuildRows(e);

    const float rowH = ImGui::GetTextLineHeightWithSpacing();
    ImGuiListClipper clipper;
    clipper.Begin(int(e.rows.size()), rowH);
    while (clipper.Step()) {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
            const std::uint32_t index = e.rows[size_t(r)];
            const bin::Symbol& s = img.symbols[index];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(int(index));
            if (ImGui::Selectable("##row", e.selected == index, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                e.selected = index;
            const bool hovered = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                e.selected = index;
                ImGui::OpenPopup(menu);
            }
            ImGui::SameLine(0.0f, 0.0f);
            // The name: demangled ones carry a tag naming the language; a name that could not be
            // demangled is marked too.
            const ImVec2 at = ImGui::GetCursorScreenPos();
            float tagW = 0.0f;
            if (s.mangling != bin::Mangling::None) {
                if (s.demangled && !e.raw)
                    tagW = tag(at, shortScheme(s.mangling), palette_.accent);
                else
                    tagW = tag(at, s.demangled ? "mangled" : "not demangled", s.demangled ? palette_.textDim : palette_.iconOrange);
                ImGui::SetCursorScreenPos(ImVec2(at.x + tagW + 6.0f * s_, at.y));
            }
            const std::string_view name = img.text(e.raw ? s.name : s.shown);
            if (s.side == bin::Side::Local || s.hidden)
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(palette_.textDim));
            ImGui::TextUnformatted(name.data(), name.data() + name.size());
            if (s.side == bin::Side::Local || s.hidden)
                ImGui::PopStyleColor();
            ImGui::PopID();
            if (hovered && (s.mangling != bin::Mangling::None || ImGui::GetItemRectMax().x > ImGui::GetWindowPos().x + ImGui::GetWindowWidth())) {
                ui::beginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
                const std::string_view shown = img.text(s.shown), stored = img.text(s.name);
                ImGui::TextUnformatted(shown.data(), shown.data() + shown.size());
                if (s.mangling != bin::Mangling::None) {
                    ImGui::Separator();
                    ImGui::TextDisabled("%s, %s. As stored in the file:", bin::manglingName(s.mangling), s.demangled ? "demangled" : "could not be demangled");
                    ImGui::TextUnformatted(stored.data(), stored.data() + stored.size());
                }
                ImGui::PopTextWrapPos();
                ui::endTooltip();
            }
            auto cell = [&](int column, const char* text, bool dim = false) {
                if (!ImGui::TableSetColumnIndex(column) || !text || !*text)
                    return;
                if (dim)
                    ImGui::TextDisabled("%s", text);
                else
                    ImGui::TextUnformatted(text);
            };
            auto hexCell = [&](int column, std::uint64_t v) {
                if (v == bin::kNone)
                    return;
                std::snprintf(label_, sizeof(label_), "0x%llx", static_cast<unsigned long long>(v));
                cell(column, label_);
            };
            auto textCell = [&](int column, bin::Text t) {
                if (t.length && ImGui::TableSetColumnIndex(column)) {
                    const std::string_view v = img.text(t);
                    ImGui::TextUnformatted(v.data(), v.data() + v.size());
                }
            };
            if (ImGui::TableSetColumnIndex(Scheme) && s.mangling != bin::Mangling::None)
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(s.demangled ? palette_.accent : palette_.iconOrange), "%s%s",
                                   bin::manglingName(s.mangling), s.demangled ? "" : " (raw)");
            cell(Kind, kindName(s), true);
            hexCell(Address, s.address);
            hexCell(Offset, s.offset);
            if (s.size) {
                std::snprintf(label_, sizeof(label_), "%llu", static_cast<unsigned long long>(s.size));
                cell(Size, label_);
            }
            textCell(SectionName, s.section);
            if (s.hidden && s.side != bin::Side::Import)
                std::snprintf(label_, sizeof(label_), "%s, hidden", bindingName(s.binding));
            else
                std::snprintf(label_, sizeof(label_), "%s", bindingName(s.binding));
            cell(Binding, label_, true);
            textCell(Detail, s.detail);
            if (s.ordinal != bin::kNoIndex) {
                std::snprintf(label_, sizeof(label_), "%u", s.ordinal);
                cell(Ordinal, label_);
            }
            if (s.hint != bin::kNoIndex) {
                std::snprintf(label_, sizeof(label_), "%u", s.hint);
                cell(Hint, label_, true);
            }
            if (s.library < img.libraries.size()) {
                textCell(LibraryName, img.libraries[s.library].name);
                if (s.delayed && ImGui::TableSetColumnIndex(LibraryName)) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(delay-loaded)");
                }
            }
            if (s.member < img.members.size())
                textCell(MemberName, img.members[s.member].name);
        }
    }
    ImGui::EndTable();
    if (ImGui::BeginPopupEx(menu, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings)) {
        if (e.selected < img.symbols.size()) {
            const bin::Symbol& s = img.symbols[e.selected];
            if (ImGui::MenuItem(ICON_FA_COPY "  Copy name"))
                copy(img.text(s.shown));
            if (s.mangling != bin::Mangling::None && ImGui::MenuItem(ICON_FA_COPY "  Copy mangled name"))
                copy(img.text(s.name));
            if (s.address != bin::kNone && ImGui::MenuItem(ICON_FA_COPY "  Copy address")) {
                std::snprintf(label_, sizeof(label_), "0x%llx", static_cast<unsigned long long>(s.address));
                ImGui::SetClipboardText(label_);
            }
            if (s.offset != bin::kNone && ImGui::MenuItem(ICON_FA_COPY "  Copy file offset")) {
                std::snprintf(label_, sizeof(label_), "0x%llx", static_cast<unsigned long long>(s.offset));
                ImGui::SetClipboardText(label_);
            }
        }
        ImGui::EndPopup();
    }
}

void LibraryView::sectionTable(Entry& e, float height)
{
    const bin::Image& img = e.image;
    const ImGuiTableFlags flags = ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuterH | ImGuiTableFlags_SizingFixedFit |
                                  ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate;
    if (!beginTable(e, 7, flags, ImVec2(0.0f, height)))
        return;
    ImGui::TableSetupScrollFreeze(1, 1);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 0.0f, 0);
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 0.0f, 1);
    ImGui::TableSetupColumn(img.format == bin::Format::Pe ? "RVA" : "Address", ImGuiTableColumnFlags_WidthFixed, 0.0f, 2);
    ImGui::TableSetupColumn("Size in memory", ImGuiTableColumnFlags_WidthFixed, 0.0f, 3);
    ImGui::TableSetupColumn("File offset", ImGuiTableColumnFlags_WidthFixed, 0.0f, 4);
    ImGui::TableSetupColumn("Size in file", ImGuiTableColumnFlags_WidthFixed, 0.0f, 5);
    ImGui::TableSetupColumn("Access", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, 0.0f, 6);
    columnsReady();
    ImGui::TableHeadersRow();
    sortSpecs(e);
    if (e.rowsDirty)
        rebuildRows(e);
    ImGuiListClipper clipper;
    clipper.Begin(int(e.rows.size()));
    while (clipper.Step())
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
            const bin::Section& s = img.sections[e.rows[size_t(r)]];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const std::string_view name = img.text(s.name), type = img.text(s.type);
            ImGui::TextUnformatted(name.data(), name.data() + name.size());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("%.*s", int(type.size()), type.data());
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("0x%llx", static_cast<unsigned long long>(s.address));
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%llu", static_cast<unsigned long long>(s.memorySize));
            if (s.offset != bin::kNone) {
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("0x%llx", static_cast<unsigned long long>(s.offset));
            }
            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%llu", static_cast<unsigned long long>(s.fileSize));
            ImGui::TableSetColumnIndex(6);
            ImGui::Text("%c%c%c", s.read ? 'R' : '-', s.write ? 'W' : '-', s.execute ? 'X' : '-');
        }
    ImGui::EndTable();
}

void LibraryView::libraryTable(Entry& e, float height)
{
    const bin::Image& img = e.image;
    const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                  ImGuiTableFlags_BordersOuterH | ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate;
    if (!beginTable(e, 3, flags, ImVec2(0.0f, height)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Library", ImGuiTableColumnFlags_WidthStretch, 0.0f, 0);
    ImGui::TableSetupColumn("How", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoSort, 0.0f, 1);
    ImGui::TableSetupColumn("Symbols", ImGuiTableColumnFlags_WidthFixed, 0.0f, 2);
    columnsReady();
    ImGui::TableHeadersRow();
    sortSpecs(e);
    if (e.rowsDirty)
        rebuildRows(e);
    std::uint32_t go = UINT32_MAX;
    for (const std::uint32_t i : e.rows) {
        const bin::Library& l = img.libraries[i];
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::PushID(int(i));
        const std::string_view name = img.text(l.name);
        std::snprintf(label_, sizeof(label_), "%.*s", int(std::min<size_t>(name.size(), 900)), name.data());
        if (ImGui::Selectable(label_, false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick) &&
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            go = i;
        ui::itemTooltip("Double-click: its symbols");
        ImGui::PopID();
        ImGui::TableSetColumnIndex(1);
        const char* how = img.format == bin::Format::Archive ? "symbols of this import library come from it"
                        : img.format == bin::Format::Pe ? (l.delayed ? "delay-loaded (on first use)" : "imported (loaded with the file)")
                        : l.needed ? "needed (DT_NEEDED)" : "named by symbol versions";
        ImGui::TextDisabled("%s", how);
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%u", l.symbols);
    }
    ImGui::EndTable();
    if (go != UINT32_MAX)
        for (std::uint32_t k = 0; k < img.lists.size(); ++k)
            if (img.lists[k].library == go) {
                choose(e, k);
                break;
            }
}

void LibraryView::memberTable(Entry& e, float height)
{
    const bin::Image& img = e.image;
    const ImGuiTableFlags flags = ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuterH | ImGuiTableFlags_SizingFixedFit |
                                  ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate;
    if (!beginTable(e, 5, flags, ImVec2(0.0f, height)))
        return;
    ImGui::TableSetupScrollFreeze(1, 1);
    ImGui::TableSetupColumn("Member", ImGuiTableColumnFlags_WidthFixed, 0.0f, 0);
    ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed, 0.0f, 1);
    ImGui::TableSetupColumn("File offset", ImGuiTableColumnFlags_WidthFixed, 0.0f, 2);
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 0.0f, 3);
    ImGui::TableSetupColumn("Symbols", ImGuiTableColumnFlags_WidthFixed, 0.0f, 4);
    columnsReady();
    ImGui::TableHeadersRow();
    sortSpecs(e);
    if (e.rowsDirty)
        rebuildRows(e);
    std::uint32_t go = UINT32_MAX;
    ImGuiListClipper clipper;
    clipper.Begin(int(e.rows.size()));
    while (clipper.Step())
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
            const std::uint32_t i = e.rows[size_t(r)];
            const bin::Member& m = img.members[i];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(int(i));
            const std::string_view name = img.text(m.name);
            std::snprintf(label_, sizeof(label_), "%.*s", int(std::min<size_t>(name.size(), 900)), name.data());
            if (ImGui::Selectable(label_, false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick) &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                go = i;
            ui::itemTooltip("Double-click: its symbols");
            ImGui::PopID();
            ImGui::TableSetColumnIndex(1);
            const std::string_view format = img.text(m.format);
            ImGui::TextDisabled("%.*s", int(format.size()), format.data());
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("0x%llx", static_cast<unsigned long long>(m.offset));
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%llu", static_cast<unsigned long long>(m.size));
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%u", m.symbols);
        }
    ImGui::EndTable();
    if (go != UINT32_MAX)
        for (std::uint32_t k = 0; k < img.lists.size(); ++k)
            if (img.lists[k].member == go) {
                choose(e, k);
                break;
            }
}

void LibraryView::symbolDetails(Entry& e)
{
    const bin::Image& img = e.image;
    if (e.selected >= img.symbols.size())
        return;
    const bin::Symbol& s = img.symbols[e.selected];
    ImGui::Separator();
    const std::string_view shown = img.text(s.shown), stored = img.text(s.name);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(shown.data(), shown.data() + shown.size());
    ImGui::PopTextWrapPos();
    if (s.mangling != bin::Mangling::None) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(s.demangled ? palette_.accent : palette_.iconOrange), "%s  %s%s", ICON_FA_CODE,
                           bin::manglingName(s.mangling), s.demangled ? ", demangled. Stored as:" : ": the demangler does not know this form. Stored as:");
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%.*s", int(stored.size()), stored.data());
        ImGui::PopTextWrapPos();
    }
    // One line of facts, then copy buttons.
    std::string facts;
    auto fact = [&](const char* label, const std::string& value) {
        if (value.empty())
            return;
        if (!facts.empty())
            facts += "   \xC2\xB7   ";
        facts += label;
        facts += value;
    };
    char number[48];
    auto hexText = [&](std::uint64_t v) {
        std::snprintf(number, sizeof(number), "0x%llx", static_cast<unsigned long long>(v));
        return std::string(number);
    };
    fact("", s.side == bin::Side::Export ? "exported" : s.side == bin::Side::Import ? "imported" : "internal");
    fact("", kindName(s));
    fact("", bindingName(s.binding));
    if (s.address != bin::kNone)
        fact(img.format == bin::Format::Pe ? "RVA " : "address ", hexText(s.address));
    if (s.offset != bin::kNone)
        fact("file offset ", hexText(s.offset));
    if (s.size)
        fact("size ", std::to_string(s.size) + " bytes");
    fact("section ", std::string(img.text(s.section)));
    if (s.ordinal != bin::kNoIndex)
        fact("ordinal ", std::to_string(s.ordinal));
    if (s.hint != bin::kNoIndex)
        fact("hint ", std::to_string(s.hint));
    fact(img.format == bin::Format::Elf ? "version " : "", std::string(img.text(s.detail)));
    if (s.library < img.libraries.size())
        fact("from ", std::string(img.text(img.libraries[s.library].name)) + (s.delayed ? " (delay-loaded)" : ""));
    if (s.member < img.members.size())
        fact("in ", std::string(img.text(img.members[s.member].name)));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", facts.c_str());
    ImGui::PopTextWrapPos();
    if (ImGui::SmallButton(ICON_FA_COPY "  Copy name"))
        copy(shown);
    if (s.mangling != bin::Mangling::None) {
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_COPY "  Copy mangled name"))
            copy(stored);
    }
}

bool LibraryView::render(model::Document& doc, ImVec2 size)
{
    Entry& e = entryFor(doc);
    if (!e.readable)
        return false;
    const bin::Image& img = e.image;
    resetFor(doc.id);
    panel::beginChild("##library", size, palette_.editorBg, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav);
    toolbar(doc, e, size.x);
    const float height = std::max(1.0f, size.y - 32.0f * s_);
    if (treeWidth_ <= 0.0f)
        treeWidth_ = 260.0f * s_;
    treeWidth_ = std::clamp(treeWidth_, 160.0f * s_, std::max(160.0f * s_, size.x - 240.0f * s_));
    treePanel(e, height);

    ImGui::SameLine(0.0f, 0.0f);
    ImGui::InvisibleButton("##libSplit", ImVec2(6.0f * s_, height));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActive())
        treeWidth_ += ImGui::GetIO().MouseDelta.x;
    ImGui::SameLine(0.0f, 0.0f);

    panel::beginChild("##libContent", ImVec2(0.0f, height), palette_.editorBg, ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoNav);
    const bin::ListNode& node = img.lists[std::min<size_t>(e.list, img.lists.size() - 1)];
    // The list's title and, for lists of names, the filter.
    const std::string_view title = img.text(node.title);
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%.*s", int(title.size()), title.data());
    if (node.type != bin::ListType::Overview) {
        ImGui::SameLine();
        if (e.rows.size() != node.count && !e.rowsDirty)
            ImGui::TextDisabled("%zu of %u", e.rows.size(), node.count);
        else
            ImGui::TextDisabled("%u", node.count);
        ImGui::SameLine(0.0f, 24.0f * s_);
        ImGui::SetNextItemWidth(std::min(320.0f * s_, std::max(80.0f * s_, ImGui::GetContentRegionAvail().x)));
        if (ImGui::InputTextWithHint("##libFilter", ICON_FA_MAGNIFYING_GLASS "  Filter names", e.filter, sizeof(e.filter)))
            e.rowsDirty = true;
        registerLastItem("library:filter", "textbox", "Filter names", nullptr);
    }
    if (shownList_ != e.list && shownList_ != UINT32_MAX)
        ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
    const bool details = node.type == bin::ListType::Symbols && e.selected < img.symbols.size();
    const float detailsH = details ? ImGui::GetTextLineHeightWithSpacing() * 5.5f + ImGui::GetFrameHeight() : 0.0f;
    const float tableH = std::max(ImGui::GetFrameHeight() * 3.0f, ImGui::GetContentRegionAvail().y - detailsH);
    switch (node.type) {
        case bin::ListType::Overview: overview(e); break;
        case bin::ListType::Sections: sectionTable(e, tableH); break;
        case bin::ListType::Libraries: libraryTable(e, tableH); break;
        case bin::ListType::Members: memberTable(e, tableH); break;
        case bin::ListType::Symbols: symbolTable(e, tableH); break;
    }
    shownList_ = e.list;
    if (details)
        symbolDetails(e);
    ImGui::EndChild();
    ImGui::EndChild();
    return true;
}

} // namespace enp::view
