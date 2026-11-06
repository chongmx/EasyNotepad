#include "enp/view/viewer/ViewerView.h"

#include "enp/os/Memory.h"
#include "enp/view/PanelFrame.h"
#include "enp/view/UiAutomation.h"

#include "IconsFontAwesome6.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

namespace enp::view {
namespace {

namespace mv = model::viewer;

// "A", "Z", "AA", ...
void columnName(std::uint32_t c, char* out, std::size_t size)
{
    char tmp[8];
    int n = 0;
    std::uint32_t v = c + 1;
    while (v > 0 && n < 7) {
        tmp[n++] = char('A' + (v - 1) % 26);
        v = (v - 1) / 26;
    }
    std::size_t i = 0;
    while (n > 0 && i + 1 < size)
        out[i++] = tmp[--n];
    out[i] = 0;
}

const char* iconFor(mv::Shape shape, const std::string& format)
{
    if (format == "PDF document")
        return ICON_FA_FILE_PDF;
    switch (shape) {
    case mv::Shape::Flow: return ICON_FA_FILE_WORD;
    case mv::Shape::Sheet: return ICON_FA_FILE_EXCEL;
    case mv::Shape::Pages: return ICON_FA_FILE_POWERPOINT;
    default: return ICON_FA_FILE;
    }
}

} // namespace

ViewerView::ViewerView(viewmodel::NotepadViewModel& vm, const Fonts& fonts, const Palette& palette, float uiScale, MarkdownImages& images,
                       EmojiCache*& emoji)
    : vm_(vm), fonts_(fonts), palette_(palette), s_(uiScale), images_(images), emoji_(emoji)
{
}

ViewerView::~ViewerView()
{
    for (auto& [id, e] : entries_) {
        for (auto& [media, held] : e.images)
            images_.release(held.key);
        for (auto& [page, held] : e.pageImages)
            for (auto& [media, h] : held)
                images_.release(h.key);
    }
}

std::size_t ViewerView::heldImageCount() const
{
    std::size_t n = 0;
    for (const auto& [id, e] : entries_) {
        n += e.images.size();
        for (const auto& [page, held] : e.pageImages)
            n += held.size();
    }
    return n;
}

void ViewerView::releaseEntry(Entry& e)
{
    for (auto& [media, held] : e.images)
        images_.release(held.key);
    for (auto& [page, held] : e.pageImages)
        for (auto& [media, h] : held)
            images_.release(h.key);
    if (shownDoc_ == e.id)
        shownDoc_ = -1;
    entries_.erase(e.id);
    if (entries_.empty())
        painter_.release();
    vm_.requestMemoryTrim(); // a document's model can be large: give the pages back to the OS
}

void ViewerView::release(int documentId)
{
    const auto it = entries_.find(documentId);
    if (it != entries_.end())
        releaseEntry(it->second);
}

void ViewerView::releaseClosed()
{
    for (auto it = entries_.begin(); it != entries_.end();) {
        const int id = it->first;
        ++it; // releaseEntry erases the entry
        if (vm_.documentIndexById(id) < 0)
            release(id);
    }
}

ViewerView::Entry& ViewerView::entryFor(model::Document& doc)
{
    auto it = entries_.find(doc.id);
    if (it != entries_.end())
        return it->second;
    os::mem::Scope scope("viewer");
    Entry& e = entries_[doc.id];
    e.id = doc.id;
    e.keyPrefix = vm_.absolutePath(doc.path) + "#";
    e.doc = mv::readViewerDocument(doc.path, vm_.readWorkspaceFile(doc.path));
    e.flow.emoji = emoji_;
    for (std::size_t i = 0; i < e.doc.book.sheets.size(); ++i)
        if (!e.doc.book.sheets[i].hidden) {
            e.sheet = int(i);
            break;
        }
    return e;
}

const MarkdownImage* ViewerView::image(Entry& e, std::map<std::string, HeldImage>& held, const std::string& media,
                                       std::map<std::string, std::string>& bytes)
{
    const auto found = held.find(media);
    if (found != held.end())
        return found->second.image;
    os::mem::Scope scope("images");
    HeldImage h;
    h.key = e.keyPrefix + media;
    const auto data = bytes.find(media);
    if (data == bytes.end()) {
        h.image = images_.acquireUnavailable(h.key, "not in the file");
    } else {
        h.image = images_.acquire(h.key, data->second);
        // The cache keeps the pixels; the encoded bytes are not needed any more (a PDF page read
        // again puts them back).
        os::mem::release(data->second);
        bytes.erase(data);
    }
    const MarkdownImage* image = h.image;
    held.emplace(media, std::move(h));
    return image;
}

void ViewerView::render(model::Document& doc, ImVec2 size)
{
    Entry& e = entryFor(doc);
    os::mem::Scope scope("viewer");
    ImGui::PushFont(fonts_.ui, Fonts::kUiSize);
    const float pad = 8.0f * s_;
    if (e.doc.shape == mv::Shape::None) {
        unreadable(doc, e, size);
        ImGui::PopFont();
        return;
    }
    const float before = ImGui::GetCursorPosY();
    ImGui::SetCursorPos(ImVec2(pad, pad * 0.5f));
    toolbar(doc, e, size.x - pad * 2.0f);
    ImGui::SetCursorPosX(0.0f);
    const float used = ImGui::GetCursorPosY() - before;
    const ImVec2 rest(size.x, std::max(1.0f, size.y - used));
    if (doc.id != shownDoc_) { // another document in the shared windows: its own scroll
        ImGui::SetNextWindowScroll(e.scroll);
        shownDoc_ = doc.id;
    }
    switch (e.doc.shape) {
    case mv::Shape::Flow: flow(doc, e, rest); break;
    case mv::Shape::Sheet: sheet(e, rest); break;
    case mv::Shape::Pages: pages(e, rest); break;
    case mv::Shape::None: break;
    }
    ImGui::PopFont();
}

void ViewerView::toolbar(model::Document& doc, Entry& e, float width)
{
    const mv::ViewerDocument& d = e.doc;
    ImGui::AlignTextToFramePadding();
    if (d.shape == mv::Shape::Pages) {
        const bool slides = d.format != "PDF document";
        const int n = int(d.pages.pages.size());
        const int current = std::clamp(e.currentPage, 0, std::max(0, n - 1));
        const char* title = n > 0 ? d.pages.pages[size_t(current)].title.c_str() : "";
        if (slides && *title)
            std::snprintf(label_, sizeof(label_), "%s  %s  ·  Slide %d of %d  ·  %s", iconFor(d.shape, d.format), d.format.c_str(), current + 1, n,
                          title);
        else
            std::snprintf(label_, sizeof(label_), "%s  %s  ·  %s %d of %d", iconFor(d.shape, d.format), d.format.c_str(), slides ? "Slide" : "Page",
                          current + 1, n);
    } else if (d.shape == mv::Shape::Sheet) {
        int visible = 0;
        for (const mv::Sheet& s : d.book.sheets)
            visible += s.hidden ? 0 : 1;
        std::snprintf(label_, sizeof(label_), "%s  %s  ·  %d %s", iconFor(d.shape, d.format), d.format.c_str(), visible, visible == 1 ? "sheet" : "sheets");
    } else {
        std::snprintf(label_, sizeof(label_), "%s  %s  ·  %zu blocks", iconFor(d.shape, d.format), d.format.c_str(), d.flow.blocks.size());
    }
    ImGui::TextDisabled("%s", label_);
    if (d.shape == mv::Shape::Pages) {
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_MAGNIFYING_GLASS_MINUS "##viewerZoomOut"))
            e.zoom = std::max(0.25f, (e.zoom > 0.0f ? e.zoom : 1.0f) / 1.25f);
        registerLastItem("viewer:zoom-out", "button", "Zoom out", [&e]() { e.zoom = std::max(0.25f, (e.zoom > 0.0f ? e.zoom : 1.0f) / 1.25f); });
        ImGui::SameLine();
        if (ImGui::SmallButton("Fit##viewerZoomFit"))
            e.zoom = 0.0f;
        registerLastItem("viewer:zoom-fit", "button", "Fit to width", [&e]() { e.zoom = 0.0f; });
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_MAGNIFYING_GLASS_PLUS "##viewerZoomIn"))
            e.zoom = std::min(8.0f, (e.zoom > 0.0f ? e.zoom : 1.0f) * 1.25f);
        registerLastItem("viewer:zoom-in", "button", "Zoom in", [&e]() { e.zoom = std::min(8.0f, (e.zoom > 0.0f ? e.zoom : 1.0f) * 1.25f); });
    }
    if (!d.notShown.empty()) {
        std::size_t at = 0;
        at += std::size_t(std::snprintf(label_, sizeof(label_), "Not shown yet: "));
        for (std::size_t i = 0; i < d.notShown.size() && at + 2 < sizeof(label_); ++i)
            at += std::size_t(std::snprintf(label_ + at, sizeof(label_) - at, "%s%s", i ? ", " : "", d.notShown[i].c_str()));
        ImGui::SameLine();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(palette_.iconOrange), ICON_FA_CIRCLE_INFO);
        ImGui::SetItemTooltip("%s", label_);
    }
    const float button = ImGui::CalcTextSize(ICON_FA_FILE_LINES "  Open as text").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), width - button + ImGui::GetStyle().WindowPadding.x));
    const int index = vm_.documentIndexById(doc.id);
    if (ImGui::SmallButton(ICON_FA_FILE_LINES "  Open as text"))
        vm_.openAsText(index);
    registerLastItem("open-as-text", "button", "Open as text anyway", [this, index]() { vm_.openAsText(index); });
}

void ViewerView::unreadable(model::Document& doc, Entry& e, ImVec2 size)
{
    const float pad = 24.0f * s_;
    const float column = std::min(size.x - pad * 2.0f, 560.0f * s_);
    ImGui::SetCursorPos(ImVec2(std::max(pad, (size.x - column) * 0.5f), std::max(pad, size.y * 0.25f)));
    ImGui::BeginGroup();
    ImGui::PushFont(nullptr, ImGui::GetFontSize() * 2.0f);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(palette_.iconOrange), "%s", ICON_FA_FILE_CIRCLE_EXCLAMATION);
    ImGui::PopFont();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + column);
    ImGui::Text("%s cannot be shown", doc.title.c_str());
    ImGui::TextDisabled("%s: %s.", e.doc.format.empty() ? doc.format.c_str() : e.doc.format.c_str(), e.doc.error.c_str());
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    const int index = vm_.documentIndexById(doc.id);
    if (ImGui::Button(ICON_FA_FILE_LINES "  Open as text anyway"))
        vm_.openAsText(index);
    registerLastItem("open-as-text", "button", "Open as text anyway", [this, index]() { vm_.openAsText(index); });
    ImGui::EndGroup();
}

// ---- Word: the shared Markdown renderer ----------------------------------------------------------

void ViewerView::flow(model::Document& doc, Entry& e, ImVec2 size)
{
    (void)doc;
    if (!e.flow.onLink)
        e.flow.onLink = [this](const std::string& url) {
            if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0 || url.rfind("mailto:", 0) == 0)
                vm_.openLink(url);
        };
    // `e` is a map node: its address holds until the document closes and the entry is erased.
    if (!e.flow.onImage) // set once: `e` is a map node, its address holds until the entry is erased
        e.flow.onImage = [this, &e](const std::string& source) { return image(e, e.images, source, e.doc.flow.media); };
    e.flow.emoji = emoji_;
    panel::beginChild("##viewerFlow", size, palette_.editorBg, ImGuiChildFlags_None, ImGuiWindowFlags_NoNav);
    const float viewW = ImGui::GetContentRegionAvail().x;
    const float zoom = vm_.zoom.get();
    const float pad = 24.0f * s_;
    const float column = std::min(std::max(1.0f, viewW - pad * 2.0f), 820.0f * s_ * zoom);
    ImGui::SetCursorPos(ImVec2(std::max(pad, (viewW - column) * 0.5f), pad));
    e.flow.renderBlocks(e.doc.flow.blocks, 1, column, fonts_, palette_, Fonts::kUiSize * 1.08f * zoom, s_);
    ImGui::Dummy(ImVec2(1.0f, size.y * 0.4f));
    e.scroll = ImVec2(ImGui::GetScrollX(), ImGui::GetScrollY());
    ImGui::EndChild();
}

// ---- Excel: a grid that draws only the visible cells ---------------------------------------------

void ViewerView::sheet(Entry& e, ImVec2 size)
{
    const mv::Workbook& book = e.doc.book;
    if (book.sheets.empty())
        return;
    e.sheet = std::clamp(e.sheet, 0, int(book.sheets.size()) - 1);
    const float pad = 8.0f * s_;
    const float rowLine = ImGui::GetFrameHeight();
    // Sheet tabs, then the cell inspector, then the grid.
    float top = ImGui::GetCursorPosY();
    int visible = 0;
    for (const mv::Sheet& s : book.sheets)
        visible += s.hidden ? 0 : 1;
    if (visible > 1) {
        ImGui::SetCursorPos(ImVec2(pad, top));
        for (std::size_t i = 0; i < book.sheets.size(); ++i) {
            if (book.sheets[i].hidden)
                continue;
            ImGui::PushID(int(i));
            const bool current = int(i) == e.sheet;
            if (current)
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetColorU32(ImGuiCol_ButtonActive));
            if (ImGui::SmallButton(book.sheets[i].name.empty() ? "(sheet)" : book.sheets[i].name.c_str()))
                e.sheet = int(i);
            if (current)
                ImGui::PopStyleColor();
            ImGui::PopID();
            ImGui::SameLine();
        }
        ImGui::NewLine();
        top = ImGui::GetCursorPosY();
    }
    const mv::Sheet& sh = book.sheets[size_t(e.sheet)];
    {
        char ref[16];
        columnName(e.selCol, ref, sizeof(ref));
        const mv::Cell* cell = sh.cell(e.selRow, e.selCol);
        if (cell && !cell->formula.empty())
            std::snprintf(label_, sizeof(label_), "%s%u   =%s   %s", ref, e.selRow + 1, cell->formula.c_str(), cell->text.c_str());
        else
            std::snprintf(label_, sizeof(label_), "%s%u   %s", ref, e.selRow + 1, cell ? cell->text.c_str() : "");
        ImGui::SetCursorPos(ImVec2(pad, top));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label_);
        top = ImGui::GetCursorPosY();
    }
    ImGui::SetCursorPos(ImVec2(0.0f, top));
    const ImVec2 gridSize(size.x, std::max(1.0f, size.y - (top - ImGui::GetCursorStartPos().y) + (size.y > 0 ? 0.0f : 0.0f) - rowLine * 0.0f));

    // Geometry: Excel's column width is in characters of the default font (7 pixels each at 96
    // dpi, plus 5 pixels of padding); row heights are in points.
    const float zoom = vm_.zoom.get();
    const float pxPerPoint = 96.0f / 72.0f * s_ * zoom;
    const std::uint32_t columns = std::min<std::uint32_t>(16384, std::max<std::uint32_t>(sh.columnCount + 8, 26));
    const std::uint32_t rows = std::min<std::uint32_t>(1048576, std::max<std::uint32_t>(sh.rowCount + 20, 60));
    if (e.columnSheet != e.sheet || e.columnScale != pxPerPoint || e.columnX.size() != columns + 1) {
        e.columnX.resize(columns + 1);
        e.columnX[0] = 0.0f;
        for (std::uint32_t c = 0; c < columns; ++c) {
            const float w = sh.columnWidth(c);
            e.columnX[c + 1] = e.columnX[c] + (w <= 0.0f ? 0.0f : (w * 7.0f + 5.0f) * 0.75f * pxPerPoint);
        }
        if (e.columnX.capacity() > 2 * e.columnX.size() + 64)
            e.columnX.shrink_to_fit();
        e.columnSheet = e.sheet;
        e.columnScale = pxPerPoint;
    }
    const float defaultH = sh.defaultRowHeight * pxPerPoint;
    // Rows with their own height are few: a row's top is the default height times its index plus
    // the custom rows above it, found by binary search.
    auto rowHeight = [&](std::uint32_t r) {
        const mv::Row* row = sh.row(r);
        return row && row->height > 0.0f ? row->height * pxPerPoint : defaultH;
    };
    auto rowTop = [&](std::uint32_t r) {
        float y = float(r) * defaultH;
        for (const mv::Row& row : sh.rows) { // rows are sorted; only the custom ones add
            if (row.index >= r)
                break;
            if (row.height > 0.0f)
                y += row.height * pxPerPoint - defaultH;
        }
        return y;
    };
    (void)rowHeight;
    const float headerH = ImGui::GetFontSize() + 6.0f * s_;
    char rowLabel[16];
    std::snprintf(rowLabel, sizeof(rowLabel), "%u", rows);
    const float headerW = ImGui::CalcTextSize(rowLabel).x + 14.0f * s_;
    const float totalW = headerW + e.columnX.back();
    const float totalH = headerH + rowTop(rows);

    panel::beginChild("##viewerSheet", gridSize, palette_.editorBg, ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoNav);
    ImGui::Dummy(ImVec2(totalW, totalH));
    const float scrollX = ImGui::GetScrollX(), scrollY = ImGui::GetScrollY();
    e.scroll = ImVec2(scrollX, scrollY);
    const ImVec2 win = ImGui::GetWindowPos();
    const ImVec2 view = ImGui::GetWindowSize();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const std::uint32_t fr = std::min(sh.frozenRows, rows), fc = std::min(sh.frozenColumns, columns);
    const float frozenW = e.columnX[fc], frozenH = rowTop(fr);
    const float bodyX = win.x + headerW + frozenW, bodyY = win.y + headerH + frozenH;
    const ImU32 grid = (palette_.border & 0x00FFFFFFu) | 0x60000000u;
    const ImU32 headerBg = IM_COL32(128, 128, 128, 30);
    const float fontPx = ImGui::GetFontSize() * zoom;

    // Which column or row is at an x / y of the scrolled part.
    auto columnAt = [&](float x) {
        const auto it = std::upper_bound(e.columnX.begin(), e.columnX.end(), x);
        return std::uint32_t(std::max<std::ptrdiff_t>(0, (it - e.columnX.begin()) - 1));
    };
    auto rowAt = [&](float y) {
        std::uint32_t lo = 0, hi = rows;
        while (lo + 1 < hi) {
            const std::uint32_t mid = lo + (hi - lo) / 2;
            if (rowTop(mid) <= y)
                lo = mid;
            else
                hi = mid;
        }
        return lo;
    };
    const auto mergeOf = [&](std::uint32_t r, std::uint32_t c) -> const mv::Merge* {
        if (sh.merges.size() > 512)
            return nullptr;
        for (const mv::Merge& m : sh.merges)
            if (r >= m.row0 && r <= m.row1 && c >= m.col0 && c <= m.col1)
                return &m;
        return nullptr;
    };

    // One region of cells: rows [r0, r1), columns [c0, c1), placed so that row `rBase` / column
    // `cBase` is at (x0, y0), clipped to `clip`.
    auto region = [&](std::uint32_t r0, std::uint32_t r1, std::uint32_t c0, std::uint32_t c1, std::uint32_t rBase, std::uint32_t cBase, float x0,
                      float y0, ImVec4 clip) {
        if (r0 >= r1 || c0 >= c1)
            return;
        dl->PushClipRect(ImVec2(clip.x, clip.y), ImVec2(clip.z, clip.w), true);
        const float baseTop = rowTop(rBase);
        float y = y0 + rowTop(r0) - baseTop;
        for (std::uint32_t r = r0; r < r1; ++r) {
            const mv::Row* row = sh.row(r);
            const float h = row && row->height > 0.0f ? row->height * pxPerPoint : defaultH;
            if (sh.showGrid && h > 0.5f)
                dl->AddLine(ImVec2(clip.x, y + h - 0.5f), ImVec2(clip.z, y + h - 0.5f), grid);
            if (row && h > 0.5f) {
                const auto first = std::lower_bound(row->cells.begin(), row->cells.end(), c0,
                                                    [](const mv::Cell& cell, std::uint32_t c) { return cell.column < c; });
                for (auto it = first; it != row->cells.end() && it->column < c1; ++it) {
                    const mv::Cell& cell = *it;
                    std::uint32_t cr1 = r, cc1 = cell.column;
                    if (const mv::Merge* m = mergeOf(r, cell.column)) {
                        if (m->row0 != r || m->col0 != cell.column)
                            continue; // inside a merge: drawn by its first cell
                        cr1 = m->row1;
                        cc1 = m->col1;
                    }
                    const float cx = x0 + e.columnX[cell.column] - e.columnX[cBase];
                    const float cw = e.columnX[std::min(cc1 + 1, columns)] - e.columnX[cell.column];
                    const float ch = cr1 > r ? rowTop(cr1 + 1) - rowTop(r) : h;
                    const ImVec2 a(cx, y), b(cx + cw, y + ch);
                    if (cell.fill)
                        dl->AddRectFilled(a, ImVec2(b.x - 1.0f, b.y - 1.0f), cell.fill);
                    if (cell.text.empty() || cw < 2.0f)
                        continue;
                    const char* tb = cell.text.data();
                    const char* te = tb + cell.text.size();
                    const ImVec2 tsize = fonts_.ui->CalcTextSizeA(fontPx, FLT_MAX, 0.0f, tb, te);
                    const int align = cell.align >= 0 ? cell.align : (cell.flags & mv::Cell::Number) ? 2 : 0;
                    const float inset = 3.0f * s_;
                    float tx = a.x + inset;
                    if (align == 1)
                        tx = a.x + (cw - tsize.x) * 0.5f;
                    else if (align == 2)
                        tx = b.x - inset - tsize.x;
                    const ImU32 color = cell.color ? cell.color : (cell.flags & mv::Cell::Error) ? palette_.iconOrange : palette_.text;
                    const ImVec4 cellClip(std::max(a.x, clip.x), std::max(a.y, clip.y), std::min(b.x, clip.z), std::min(b.y, clip.w));
                    const ImVec2 at(tx, a.y + (std::min(ch, h) - fontPx) * 0.5f);
                    dl->AddText(fonts_.ui, fontPx, at, color, tb, te, 0.0f, &cellClip);
                    if (cell.flags & mv::Cell::Bold)
                        dl->AddText(fonts_.ui, fontPx, ImVec2(at.x + std::max(1.0f, fontPx * 0.045f), at.y), color, tb, te, 0.0f, &cellClip);
                }
            }
            y += h;
            if (y > clip.w)
                break;
        }
        if (sh.showGrid) {
            for (std::uint32_t c = c0; c <= c1 && c <= columns; ++c) {
                const float x = x0 + e.columnX[c] - e.columnX[cBase] - 0.5f;
                if (x > clip.z)
                    break;
                dl->AddLine(ImVec2(x, clip.y), ImVec2(x, clip.w), grid);
            }
        }
        dl->PopClipRect();
    };

    const float viewRight = win.x + view.x, viewBottom = win.y + view.y;
    // The scrolled part's visible rows and columns.
    const std::uint32_t c0 = std::max(fc, columnAt(frozenW + scrollX));
    const std::uint32_t c1 = std::min(columns, columnAt(frozenW + scrollX + (viewRight - bodyX)) + 2);
    const std::uint32_t r0 = std::max(fr, rowAt(frozenH + scrollY));
    const std::uint32_t r1 = std::min(rows, rowAt(frozenH + scrollY + (viewBottom - bodyY)) + 2);
    // The scrolled body, the frozen columns beside it, the frozen rows above it and their corner.
    const float scrolledX = bodyX - (e.columnX[c0] - e.columnX[fc]) - (scrollX - (e.columnX[c0] - frozenW - e.columnX[fc] + e.columnX[fc]));
    (void)scrolledX;
    const float sx = bodyX - scrollX; // x of column fc in the scrolled part
    const float sy = bodyY - scrollY; // y of row fr
    region(r0, r1, c0, c1, fr, fc, sx, sy, ImVec4(bodyX, bodyY, viewRight, viewBottom));
    if (fc > 0)
        region(r0, r1, 0, fc, fr, 0, win.x + headerW, sy, ImVec4(win.x + headerW, bodyY, bodyX, viewBottom));
    if (fr > 0)
        region(0, fr, c0, c1, 0, fc, sx, win.y + headerH, ImVec4(bodyX, win.y + headerH, viewRight, bodyY));
    if (fr > 0 && fc > 0)
        region(0, fr, 0, fc, 0, 0, win.x + headerW, win.y + headerH, ImVec4(win.x + headerW, win.y + headerH, bodyX, bodyY));
    if (fr > 0)
        dl->AddLine(ImVec2(win.x, bodyY), ImVec2(viewRight, bodyY), palette_.border, 1.5f * s_);
    if (fc > 0)
        dl->AddLine(ImVec2(bodyX, win.y), ImVec2(bodyX, viewBottom), palette_.border, 1.5f * s_);

    // Headers: column letters along the top, row numbers down the side.
    dl->AddRectFilled(ImVec2(win.x, win.y), ImVec2(viewRight, win.y + headerH), palette_.editorBg);
    dl->AddRectFilled(ImVec2(win.x, win.y), ImVec2(viewRight, win.y + headerH), headerBg);
    dl->AddRectFilled(ImVec2(win.x, win.y + headerH), ImVec2(win.x + headerW, viewBottom), palette_.editorBg);
    dl->AddRectFilled(ImVec2(win.x, win.y + headerH), ImVec2(win.x + headerW, viewBottom), headerBg);
    auto columnHeader = [&](std::uint32_t c, float x) {
        const float w = e.columnX[c + 1] - e.columnX[c];
        if (w < 1.0f)
            return;
        char name[8];
        columnName(c, name, sizeof(name));
        const float tw = ImGui::CalcTextSize(name).x;
        dl->AddText(ImVec2(x + (w - tw) * 0.5f, win.y + 3.0f * s_), c == e.selCol ? palette_.text : palette_.textDim, name);
        dl->AddLine(ImVec2(x + w - 0.5f, win.y), ImVec2(x + w - 0.5f, win.y + headerH), grid);
    };
    dl->PushClipRect(ImVec2(win.x + headerW, win.y), ImVec2(bodyX, win.y + headerH), true);
    for (std::uint32_t c = 0; c < fc; ++c)
        columnHeader(c, win.x + headerW + e.columnX[c]);
    dl->PopClipRect();
    dl->PushClipRect(ImVec2(bodyX, win.y), ImVec2(viewRight, win.y + headerH), true);
    for (std::uint32_t c = c0; c < c1; ++c)
        columnHeader(c, sx + e.columnX[c] - e.columnX[fc]);
    dl->PopClipRect();
    auto rowHeader = [&](std::uint32_t r, float y) {
        const mv::Row* row = sh.row(r);
        const float h = row && row->height > 0.0f ? row->height * pxPerPoint : defaultH;
        if (h < 1.0f)
            return h;
        char n[16];
        std::snprintf(n, sizeof(n), "%u", r + 1);
        const float tw = ImGui::CalcTextSize(n).x;
        dl->AddText(ImVec2(win.x + headerW - tw - 6.0f * s_, y + (h - ImGui::GetFontSize()) * 0.5f), r == e.selRow ? palette_.text : palette_.textDim, n);
        dl->AddLine(ImVec2(win.x, y + h - 0.5f), ImVec2(win.x + headerW, y + h - 0.5f), grid);
        return h;
    };
    dl->PushClipRect(ImVec2(win.x, win.y + headerH), ImVec2(win.x + headerW, bodyY), true);
    {
        float y = win.y + headerH;
        for (std::uint32_t r = 0; r < fr; ++r)
            y += rowHeader(r, y);
    }
    dl->PopClipRect();
    dl->PushClipRect(ImVec2(win.x, bodyY), ImVec2(win.x + headerW, viewBottom), true);
    {
        float y = sy + rowTop(r0) - rowTop(fr);
        for (std::uint32_t r = r0; r < r1 && y < viewBottom; ++r)
            y += rowHeader(r, y);
    }
    dl->PopClipRect();

    // The selected cell: a click picks it; Ctrl+C copies it; the arrows move.
    auto cellRect = [&](std::uint32_t r, std::uint32_t c, ImVec2& a, ImVec2& b) {
        const float x = c < fc ? win.x + headerW + e.columnX[c] : sx + e.columnX[c] - e.columnX[fc];
        const float y = r < fr ? win.y + headerH + rowTop(r) : sy + rowTop(r) - rowTop(fr);
        a = ImVec2(x, y);
        b = ImVec2(x + e.columnX[std::min(c + 1, columns)] - e.columnX[c], y + (rowTop(r + 1) - rowTop(r)));
    };
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const ImVec2 m = ImGui::GetMousePos();
        if (m.x > win.x + headerW && m.y > win.y + headerH) {
            e.selCol = m.x < bodyX ? columnAt(m.x - win.x - headerW) : columnAt(m.x - sx + e.columnX[fc]);
            e.selRow = m.y < bodyY ? rowAt(m.y - win.y - headerH) : rowAt(m.y - sy + rowTop(fr));
            e.selCol = std::min(e.selCol, columns - 1);
            e.selRow = std::min(e.selRow, rows - 1);
            ImGui::SetWindowFocus();
        }
    }
    if (ImGui::IsWindowFocused()) {
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && e.selCol + 1 < columns)
            ++e.selCol;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && e.selCol > 0)
            --e.selCol;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && e.selRow + 1 < rows)
            ++e.selRow;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && e.selRow > 0)
            --e.selRow;
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C))
            if (const mv::Cell* cell = sh.cell(e.selRow, e.selCol))
                ImGui::SetClipboardText(cell->text.c_str());
    }
    {
        ImVec2 a, b;
        cellRect(e.selRow, e.selCol, a, b);
        dl->PushClipRect(ImVec2(win.x + headerW, win.y + headerH), ImVec2(viewRight, viewBottom), true);
        dl->AddRect(a, b, palette_.accent, 0.0f, 2.0f * s_);
        dl->PopClipRect();
    }
    ImGui::EndChild();
}

// ---- PowerPoint and PDF: pages drawn by the page painter -----------------------------------------

void ViewerView::pages(Entry& e, ImVec2 size)
{
    mv::PageDocument& pd = e.doc.pages;
    const int n = int(pd.pages.size());
    const bool slides = e.doc.format != "PDF document";
    panel::beginChild("##viewerPages", size, IM_COL32(110, 110, 110, 60), ImGuiChildFlags_None, ImGuiWindowFlags_NoNav);
    const float viewW = ImGui::GetContentRegionAvail().x;
    const float pad = 16.0f * s_, gap = 14.0f * s_;
    float widest = 1.0f;
    for (const mv::Page& p : pd.pages)
        widest = std::max(widest, p.width);
    const float fit = std::max(0.05f, (viewW - pad * 2.0f) / widest);
    const float scale = std::min(fit * (e.zoom > 0.0f ? e.zoom : 1.0f), 12.0f);
    // Page tops (and speaker notes below slides) at this scale and width.
    const float wrap = std::max(50.0f, viewW - pad * 2.0f);
    if (e.pageScale != scale || e.pageTop.size() != size_t(n) + 1) {
        e.pageTop.resize(size_t(n) + 1);
        float y = pad;
        for (int i = 0; i < n; ++i) {
            e.pageTop[size_t(i)] = y;
            y += pd.pages[size_t(i)].height * scale;
            if (!pd.pages[size_t(i)].notes.empty())
                y += ImGui::CalcTextSize(pd.pages[size_t(i)].notes.c_str(), nullptr, false, wrap).y + gap;
            y += gap;
        }
        e.pageTop[size_t(n)] = y;
        e.pageScale = scale;
    }
    float contentW = 0.0f;
    for (const mv::Page& p : pd.pages)
        contentW = std::max(contentW, p.width * scale);
    ImGui::Dummy(ImVec2(contentW + pad * 2.0f, e.pageTop[size_t(n)] + pad));
    const float scrollY = ImGui::GetScrollY();
    e.scroll = ImVec2(ImGui::GetScrollX(), scrollY);
    const ImVec2 win = ImGui::GetWindowPos();
    const float viewH = ImGui::GetWindowSize().y;
    // The pages in view, read now; pages far away dropped with their images.
    int first = 0, last = -1;
    for (int i = 0; i < n; ++i) {
        if (e.pageTop[size_t(i + 1)] < scrollY)
            continue;
        if (e.pageTop[size_t(i)] > scrollY + viewH)
            break;
        if (last < 0)
            first = i;
        last = i;
    }
    if (last < 0)
        last = first;
    e.currentPage = first;
    for (int i = std::max(0, first - 1); i <= std::min(n - 1, last + 1); ++i)
        pd.load(size_t(i));
    if (pd.source)
        for (int i = 0; i < n; ++i)
            if ((i < first - 6 || i > last + 6) && pd.pages[size_t(i)].loaded) {
                pd.unload(size_t(i));
                const auto held = e.pageImages.find(size_t(i));
                if (held != e.pageImages.end()) {
                    for (auto& [media, h] : held->second)
                        images_.release(h.key);
                    e.pageImages.erase(held);
                }
            }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float scrollX = ImGui::GetScrollX();
    for (int i = first; i <= last && i < n; ++i) {
        const mv::Page& page = pd.pages[size_t(i)];
        const float w = page.width * scale;
        const ImVec2 at(win.x + std::max(pad, (viewW - w) * 0.5f) - scrollX, win.y + e.pageTop[size_t(i)] - scrollY);
        dl->AddRectFilled(ImVec2(at.x + 3.0f * s_, at.y + 3.0f * s_), ImVec2(at.x + w + 3.0f * s_, at.y + page.height * scale + 3.0f * s_),
                          IM_COL32(0, 0, 0, 50));
        PageImageSource source(*this, e, size_t(i));
        painter_.draw(page, at, scale, fonts_, &source);
        if (!page.notes.empty()) {
            ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + page.height * scale + gap * 0.5f));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::min(wrap, std::max(w, 200.0f * s_)));
            ImGui::TextDisabled("%s", page.notes.c_str());
            ImGui::PopTextWrapPos();
        }
        if (!slides || n > 1) {
            std::snprintf(label_, sizeof(label_), "%d", i + 1);
            dl->AddText(ImVec2(at.x + w + 6.0f * s_, at.y), palette_.textDim, label_);
        }
    }
    // Page Up / Page Down step a page.
    if (ImGui::IsWindowHovered() || ImGui::IsWindowFocused()) {
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown) && first + 1 < n)
            ImGui::SetScrollY(e.pageTop[size_t(first + 1)] - pad);
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp))
            ImGui::SetScrollY(e.pageTop[size_t(std::max(0, first - (scrollY > e.pageTop[size_t(first)] - pad + 1.0f ? 0 : 1)))] - pad);
    }
    ImGui::EndChild();
}

} // namespace enp::view
