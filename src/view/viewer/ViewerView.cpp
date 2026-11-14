#include "enp/view/viewer/ViewerView.h"

#include "enp/os/Memory.h"
#include "enp/view/DocumentFonts.h"
#include "enp/view/PanelFrame.h"
#include "enp/view/TouchScroll.h"
#include "enp/view/UiAutomation.h"
#include "enp/view/UiStyle.h"

#include "IconsFontAwesome6.h"
#include "imgui_internal.h" // ImTextCharFromUtf8

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

// A floating picture's box (left, top, right, bottom) in pixels from the page's text area, its
// paragraph `anchorTop` into the text area.
ImVec4 floatBox(const mv::FlowDocument::Float& f, const mv::FlowDocument::PageSetup& ps, float ppp, float anchorTop)
{
    const float w = f.width * ppp, h = f.height * ppp;
    const float areaX = f.fromH == 0 ? -ps.left * ppp : 0.0f, areaW = (f.fromH == 0 ? ps.width : ps.width - ps.left - ps.right) * ppp;
    const float areaY = f.fromV == 0 ? -ps.top * ppp : f.fromV == 1 ? 0.0f : anchorTop;
    const float areaH = (f.fromV == 0 ? ps.height : ps.height - ps.top - ps.bottom) * ppp;
    const float x = f.alignH >= 0 ? areaX + (areaW - w) * (f.alignH * 0.5f) : areaX + f.x * ppp;
    const float y = f.alignV >= 0 && f.fromV != 2 ? areaY + (areaH - h) * (f.alignV * 0.5f) : areaY + f.y * ppp;
    return ImVec4(x, y, x + w, y + h);
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
        releaseImages(e.images);
        for (auto& [page, held] : e.pageImages)
            releaseImages(held);
    }
}

void ViewerView::releaseImages(std::map<std::string, HeldImage>& held)
{
    for (auto& [media, h] : held)
        images_.release(h.key);
    std::map<std::string, HeldImage>().swap(held);
}

std::uint8_t ViewerView::scripts() const
{
    std::uint8_t scripts = 0;
    for (const auto& [id, e] : entries_)
        scripts |= e.scripts;
    return scripts;
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
    releaseImages(e.images);
    for (auto& [page, held] : e.pageImages)
        releaseImages(held);
    if (shownDoc_ == e.id)
        shownDoc_ = -1;
    entries_.erase(e.id);
    if (entries_.empty())
        painter_.release();
    vm_.requestMemoryTrim(); // the pictures' pixels can be large: give the pages back to the OS
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
        if (vm_.documentIndexById(id) < 0 || !vm_.viewer().find(id))
            release(id);
    }
}

ViewerView::Entry& ViewerView::entryFor(const model::Document& doc, const viewmodel::ViewerViewModel::Entry& model)
{
    auto it = entries_.find(doc.id);
    if (it != entries_.end() && it->second.generation != model.generation) { // read again: start over
        releaseEntry(it->second);
        it = entries_.end();
    }
    if (it != entries_.end())
        return it->second;
    os::mem::Scope scope("viewer");
    Entry& e = entries_[doc.id];
    e.id = doc.id;
    e.generation = model.generation;
    e.keyPrefix = model.path + "#";
    e.flow.emoji = emoji_;
    return e;
}

const MarkdownImage* ViewerView::image(Entry& e, std::map<std::string, HeldImage>& held, const std::string& media)
{
    const auto found = held.find(media);
    if (found != held.end())
        return found->second.image;
    os::mem::Scope scope("images");
    HeldImage h;
    h.key = e.keyPrefix + media;
    // The encoded bytes come out of the view-model once: the cache keeps the pixels (a PDF page
    // read again brings them back). Freed at the end of this function.
    const std::string bytes = vm_.viewer().takeMedia(e.id, media);
    if (bytes.empty())
        h.image = images_.acquireUnavailable(h.key, "not in the file");
    else
        h.image = images_.acquire(h.key, bytes);
    const MarkdownImage* image = h.image;
    held.emplace(media, std::move(h));
    return image;
}

void ViewerView::render(model::Document& doc, ImVec2 size)
{
    viewmodel::ViewerViewModel& viewer = vm_.viewer();
    const viewmodel::ViewerViewModel::Entry* model = viewer.find(doc.id);
    if (!model) { // not asked for yet (a document restored with the session): the view-model reads it
        viewer.open(doc.id, vm_.absolutePath(doc.path));
        model = viewer.find(doc.id);
    }
    os::mem::Scope scope("viewer");
    ImGui::PushFont(fonts_.ui, Fonts::kUiSize);
    if (!model || model->state == viewmodel::ViewerViewModel::State::Loading) {
        loading(doc, size);
        ImGui::PopFont();
        return;
    }
    const mv::ViewerDocument& d = model->doc;
    if (model->state == viewmodel::ViewerViewModel::State::Failed) {
        unreadable(doc, d.format, d.error, size);
        ImGui::PopFont();
        return;
    }
    Entry& e = entryFor(doc, *model);
    e.scripts |= d.scripts;
    const float pad = 8.0f * s_;
    const float before = ImGui::GetCursorPosY();
    ImGui::SetCursorPos(ImVec2(pad, pad * 0.5f));
    toolbar(doc, e, d, size.x - pad * 2.0f);
    ImGui::SetCursorPosX(0.0f);
    const float used = ImGui::GetCursorPosY() - before;
    const ImVec2 rest(size.x, std::max(1.0f, size.y - used));
    if (doc.id != shownDoc_) { // another document in the shared windows: its own scroll
        ImGui::SetNextWindowScroll(e.scroll);
        shownDoc_ = doc.id;
    }
    switch (d.shape) {
    case mv::Shape::Flow: flow(e, d, rest); break;
    case mv::Shape::Sheet: sheet(e, d, rest); break;
    case mv::Shape::Pages: pages(e, d, rest); break;
    case mv::Shape::None: break;
    }
    ImGui::PopFont();
}

void ViewerView::toolbar(model::Document& doc, Entry& e, const mv::ViewerDocument& d, float width)
{
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
    } else if (!e.reflow && !d.flow.printed.empty() && !e.wordPages.empty()) {
        std::snprintf(label_, sizeof(label_), "%s  %s  ·  Page %d of %zu", iconFor(d.shape, d.format), d.format.c_str(),
                      std::clamp(e.currentPage, 0, int(e.wordPages.size()) - 1) + 1, e.wordPages.size());
    } else {
        std::snprintf(label_, sizeof(label_), "%s  %s  ·  %zu blocks", iconFor(d.shape, d.format), d.format.c_str(), d.flow.blocks.size());
    }
    ImGui::TextDisabled("%s", label_);
    if (d.shape == mv::Shape::Flow && !d.flow.printed.empty()) { // Word: pages as printed, or reflowed to the window
        ImGui::SameLine();
        if (ImGui::SmallButton(e.reflow ? ICON_FA_FILE "  Pages##viewerLayout" : ICON_FA_ALIGN_LEFT "  Reflow##viewerLayout"))
            e.reflow = !e.reflow;
        registerLastItem("viewer:word-layout", "button", e.reflow ? "Show as pages" : "Reflow to the window", [&e]() { e.reflow = !e.reflow; });
    }
    const bool printedPages = d.shape == mv::Shape::Flow && !e.reflow && !d.flow.printed.empty();
    if (d.shape == mv::Shape::Pages || printedPages) {
        const int pageCount = printedPages ? int(e.wordPages.size()) : int(d.pages.pages.size());
        auto go = [&e, pageCount](int delta) { e.goToPage = std::clamp(e.currentPage + delta, 0, std::max(0, pageCount - 1)); };
        ImGui::SameLine();
        ImGui::BeginDisabled(e.currentPage <= 0);
        if (ImGui::SmallButton(ICON_FA_CHEVRON_UP "##viewerPrevious"))
            go(-1);
        ImGui::EndDisabled();
        registerLastItem("viewer:previous-page", "button", "Previous page", [go]() { go(-1); });
        ImGui::SameLine();
        ImGui::BeginDisabled(e.currentPage + 1 >= pageCount);
        if (ImGui::SmallButton(ICON_FA_CHEVRON_DOWN "##viewerNext"))
            go(1);
        ImGui::EndDisabled();
        registerLastItem("viewer:next-page", "button", "Next page", [go]() { go(1); });
        if (!printedPages) { // slides and PDF pages: their own zoom (Word follows the editor's)
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_FA_MAGNIFYING_GLASS_MINUS "##viewerZoomOut"))
                e.zoom = std::max(0.25f, (e.zoom > 0.0f ? e.zoom : 1.0f) / 1.25f);
            registerLastItem("viewer:zoom-out", "button", "Zoom out", [&e]() { e.zoom = std::max(0.25f, (e.zoom > 0.0f ? e.zoom : 1.0f) / 1.25f); });
            ImGui::SameLine();
            if (ImGui::SmallButton("Fit##viewerZoomFit"))
                e.zoom = 0.0f;
            registerLastItem("viewer:zoom-fit", "button", d.format == "PDF document" ? "Fit to width" : "Fit the slide", [&e]() { e.zoom = 0.0f; });
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_FA_MAGNIFYING_GLASS_PLUS "##viewerZoomIn"))
                e.zoom = std::min(8.0f, (e.zoom > 0.0f ? e.zoom : 1.0f) * 1.25f);
            registerLastItem("viewer:zoom-in", "button", "Zoom in", [&e]() { e.zoom = std::min(8.0f, (e.zoom > 0.0f ? e.zoom : 1.0f) * 1.25f); });
        }
    }
    if (!d.notShown.empty()) {
        std::size_t at = 0;
        at += std::size_t(std::snprintf(label_, sizeof(label_), "Not shown yet: "));
        for (std::size_t i = 0; i < d.notShown.size() && at + 2 < sizeof(label_); ++i)
            at += std::size_t(std::snprintf(label_ + at, sizeof(label_) - at, "%s%s", i ? ", " : "", d.notShown[i].c_str()));
        ImGui::SameLine();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(palette_.iconOrange), ICON_FA_CIRCLE_INFO);
        ui::itemTooltip("%s", label_);
    }
    const float button = ImGui::CalcTextSize(ICON_FA_FILE_LINES "  Open as text").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), width - button + ImGui::GetStyle().WindowPadding.x));
    const int index = vm_.documentIndexById(doc.id);
    if (ImGui::SmallButton(ICON_FA_FILE_LINES "  Open as text"))
        vm_.openAsText(index);
    registerLastItem("open-as-text", "button", "Open as text anyway", [this, index]() { vm_.openAsText(index); });
}

void ViewerView::loading(model::Document& doc, ImVec2 size)
{
    // The reader service reads the document on its own thread; the dots move meanwhile.
    const float pad = 24.0f * s_;
    ImGui::SetCursorPos(ImVec2(pad, std::max(pad, size.y * 0.3f)));
    static const char* const kDots[] = {"", ".", "..", "..."};
    ImGui::TextDisabled("%s  Reading %s%s", ICON_FA_HOURGLASS_HALF, doc.title.c_str(), kDots[int(ImGui::GetTime() * 3.0) % 4]);
}

void ViewerView::unreadable(model::Document& doc, const std::string& format, const std::string& error, ImVec2 size)
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
    ImGui::TextDisabled("%s: %s.", format.empty() ? doc.format.c_str() : format.c_str(), error.c_str());
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    const int index = vm_.documentIndexById(doc.id);
    if (ImGui::Button(ICON_FA_FILE_LINES "  Open as text anyway"))
        vm_.openAsText(index);
    registerLastItem("open-as-text", "button", "Open as text anyway", [this, index]() { vm_.openAsText(index); });
    ImGui::EndGroup();
}

// ---- Word: the shared Markdown renderer ----------------------------------------------------------

void ViewerView::flow(Entry& e, const mv::ViewerDocument& d, ImVec2 size)
{
    if (!e.flow.onLink)
        e.flow.onLink = [this](const std::string& url) {
            if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0 || url.rfind("mailto:", 0) == 0)
                vm_.openLink(url);
        };
    // `e` is a map node: its address holds until the document closes and the entry is erased.
    if (!e.flow.onImage) // set once: `e` is a map node, its address holds until the entry is erased
        e.flow.onImage = [this, &e](const std::string& source) { return image(e, e.images, source); };
    // A drawing (a chart): its page at its size in points, smaller when the column is narrower.
    if (!e.flow.onDrawing)
        e.flow.onDrawing = [this, &e](int index, ImVec2 at, float width, bool draw) {
            const viewmodel::ViewerViewModel::Entry* model = vm_.viewer().find(e.id);
            if (!model || index < 0 || std::size_t(index) >= model->doc.flow.drawings.size())
                return 0.0f;
            const mv::Page& page = model->doc.flow.drawings[std::size_t(index)];
            const float scale = std::min(width / std::max(1.0f, page.width), 96.0f / 72.0f * s_ * vm_.zoom.get() * e.pinch);
            if (draw) {
                EntryImageSource images(*this, e);
                painter_.draw(page, at, scale, fonts_, &images);
            }
            return page.height * scale + 6.0f * s_;
        };
    e.flow.emoji = emoji_;
    e.flow.paper = true;
    // The page view's header and footer: their pictures (a letterhead's logo) from the same document.
    for (MarkdownView* part : {&e.header, &e.footer}) {
        if (!part->onImage)
            part->onImage = [this, &e](const std::string& source) { return image(e, e.images, source); };
        part->emoji = emoji_;
        part->paper = true;
    }
    // A Word document is drawn as it is printed: dark text on a white page, in its own colours (a
    // copy of the palette with a paper's colours: no allocation).
    Palette paper = palette_;
    paper.text = IM_COL32(32, 33, 36, 255);
    paper.textDim = IM_COL32(95, 99, 104, 255);
    paper.textFaint = IM_COL32(140, 144, 150, 255);
    paper.iconBlue = IM_COL32(5, 99, 193, 255);
    paper.border = IM_COL32(200, 203, 208, 255);
    paper.synString = IM_COL32(163, 21, 21, 255);
    if (!e.reflow && !d.flow.printed.empty()) {
        wordPages(e, d, size, paper);
        return;
    }
    panel::beginChild("##viewerFlow", size, IM_COL32(128, 128, 128, 70), ImGuiChildFlags_None, ImGuiWindowFlags_NoNav);
    touch::zoomView(e.pinch, 0.3f, 5.0f, e.pendingScroll); // a pinch: the text larger or smaller (it reflows)
    const float viewW = ImGui::GetContentRegionAvail().x;
    const float zoom = vm_.zoom.get() * e.pinch;
    const float pad = 24.0f * s_, margin = 36.0f * s_ * zoom;
    const float column = std::min(std::max(1.0f, viewW - pad * 2.0f - margin * 2.0f), 820.0f * s_ * zoom);
    const ImVec2 page(std::max(pad, (viewW - column) * 0.5f - margin), pad);
    ImGui::SetCursorPos(ImVec2(page.x + margin, page.y + margin));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->ChannelsSplit(2); // the page goes under the text, once its height is known
    dl->ChannelsSetCurrent(1);
    const ImVec2 top = ImGui::GetCursorScreenPos();
    e.flow.renderBlocks(d.flow.blocks, e.generation, column, fonts_, paper, Fonts::kUiSize * 1.08f * zoom, s_, &d.flow.fonts);
    const float bottom = ImGui::GetCursorScreenPos().y;
    dl->ChannelsSetCurrent(0);
    dl->AddRectFilled(ImVec2(top.x - margin + 3.0f * s_, top.y - margin + 3.0f * s_), ImVec2(top.x + column + margin + 3.0f * s_, bottom + margin + 3.0f * s_),
                      IM_COL32(0, 0, 0, 40)); // its shadow
    dl->AddRectFilled(ImVec2(top.x - margin, top.y - margin), ImVec2(top.x + column + margin, bottom + margin), IM_COL32(255, 255, 255, 255));
    dl->ChannelsMerge();
    ImGui::Dummy(ImVec2(1.0f, margin + size.y * 0.4f));
    e.scroll = ImVec2(ImGui::GetScrollX(), ImGui::GetScrollY());
    ImGui::EndChild();
}

// Word's pages: the body laid out at the page's text width and cut into pages where it fills them
// (or Word breaks them), each page with its header, footer and footnotes, as printed.
void ViewerView::wordPages(Entry& e, const mv::ViewerDocument& d, ImVec2 size, const Palette& paper)
{
    const mv::FlowDocument::PageSetup& ps = d.flow.page;
    const float zoom = vm_.zoom.get() * e.pinch;
    const float fontSize = Fonts::kUiSize * 1.08f * zoom;
    ImGui::PushFont(fonts_.ui, fontSize);
    const float ppp = ImGui::GetFontSize() / 11.0f; // the renderer's pixels per point (11 points: its body size)
    ImGui::PopFont();
    const float pageW = ps.width * ppp, pageH = ps.height * ppp;
    const float textW = std::max(20.0f, (ps.width - ps.left - ps.right) * ppp);
    const float textH = std::max(20.0f, (ps.height - ps.top - ps.bottom) * ppp);
    // Header and footer: measured once at the text width, drawn on every page. When one reaches
    // past its margin (a letterhead's logo) Word moves the body clear of it: down from the header,
    // up from the footer.
    e.header.measureBlocks(d.flow.header, e.generation, textW, fonts_, paper, fontSize, s_, &d.flow.fonts);
    e.footer.measureBlocks(d.flow.footer, e.generation, textW, fonts_, paper, fontSize, s_, &d.flow.fonts);
    float headerH = 0.0f, footerH = 0.0f;
    for (std::size_t i = 0; i < d.flow.header.size(); ++i)
        headerH += e.header.blockGap(i) + e.header.blockHeight(i);
    for (std::size_t i = 0; i < d.flow.footer.size(); ++i)
        footerH += e.footer.blockGap(i) + e.footer.blockHeight(i);
    const float bodyShift = std::max(0.0f, ps.header * ppp + headerH - ps.top * ppp);
    const float bodyH = std::max(20.0f, textH - bodyShift - std::max(0.0f, ps.footer * ppp + footerH - ps.bottom * ppp));
    e.flow.measureBlocks(d.flow.blocks, e.generation, textW, fonts_, paper, fontSize, s_, &d.flow.fonts);
    if (e.wordScale != ppp || e.wordGeneration != e.generation || e.wordPages.empty())
        paginate(e, d, textW, bodyH, ppp, paper, fontSize);
    e.wordScale = ppp;
    e.wordGeneration = e.generation;
    const int count = int(e.wordPages.size());

    panel::beginChild("##viewerFlow", size, IM_COL32(128, 128, 128, 70), ImGuiChildFlags_None, ImGuiWindowFlags_NoNav | ImGuiWindowFlags_HorizontalScrollbar);
    touch::zoomView(e.pinch, 0.3f, 5.0f, e.pendingScroll); // a pinch: the pages larger or smaller
    const float gap = 16.0f * s_;
    const ImVec2 view = ImGui::GetWindowSize();
    const float total = gap + float(count) * (pageH + gap);
    ImGui::Dummy(ImVec2(pageW + gap * 2.0f, total));
    if (e.goToPage >= 0) {
        ImGui::SetScrollY(float(e.goToPage) * (pageH + gap));
        e.goToPage = -1;
    }
    const ImVec2 win = ImGui::GetWindowPos();
    const float scrollY = ImGui::GetScrollY(), scrollX = ImGui::GetScrollX();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float left = win.x + std::max(gap, (view.x - pageW) * 0.5f) - scrollX;
    // Header and footer: measured once at the text width, drawn on every page.
    e.currentPage = std::clamp(int((scrollY + view.y / 3.0f) / (pageH + gap)), 0, std::max(0, count - 1));
    for (int p = 0; p < count; ++p) {
        const float top = win.y + gap + float(p) * (pageH + gap) - scrollY;
        if (top + pageH < win.y || top > win.y + view.y)
            continue;
        const Entry::WordPage& page = e.wordPages[size_t(p)];
        dl->AddRectFilled(ImVec2(left + 3.0f * s_, top + 3.0f * s_), ImVec2(left + pageW + 3.0f * s_, top + pageH + 3.0f * s_), IM_COL32(0, 0, 0, 40));
        dl->AddRectFilled(ImVec2(left, top), ImVec2(left + pageW, top + pageH), IM_COL32(255, 255, 255, 255));
        const ImVec2 text(left + ps.left * ppp, top + ps.top * ppp);
        // A float: its picture, or its drawing (a group of shapes), in its box.
        auto drawFloat = [&](const mv::FlowDocument::Float& f, float at) {
            const ImVec4 b = floatBox(f, ps, ppp, at);
            if (!f.drawing.items.empty()) {
                EntryImageSource images(*this, e);
                painter_.draw(f.drawing, ImVec2(text.x + b.x, text.y + b.y), ppp, fonts_, &images);
                return;
            }
            const MarkdownImage* img = image(e, e.images, f.media);
            if (img && img->texture)
                dl->AddImage(img->texture->GetTexRef(), ImVec2(text.x + b.x, text.y + b.y), ImVec2(text.x + b.z, text.y + b.w));
        };
        // The page's border: its space in from the page's edge, or out from the text's.
        for (int i = 0; i < 4; ++i) {
            const model::markdown::CellEdge& b = ps.borders[i];
            if (b.width <= 0.0f)
                continue;
            const float space = ps.borderSpace[i] * ppp, w = std::max(1.0f, b.width * ppp);
            const ImVec2 lo = ps.bordersFromPage ? ImVec2(left + space, top + space) : ImVec2(text.x - space, text.y - space);
            const ImVec2 hi = ps.bordersFromPage ? ImVec2(left + pageW - space, top + pageH - space)
                                                 : ImVec2(text.x + textW + space, text.y + textH + space);
            const ImU32 color = b.color ? ImU32(b.color) : IM_COL32(0, 0, 0, 255);
            if (i == 0)
                dl->AddLine(ImVec2(lo.x, lo.y), ImVec2(hi.x, lo.y), color, w);
            else if (i == 1)
                dl->AddLine(ImVec2(lo.x, lo.y), ImVec2(lo.x, hi.y), color, w);
            else if (i == 2)
                dl->AddLine(ImVec2(lo.x, hi.y), ImVec2(hi.x, hi.y), color, w);
            else
                dl->AddLine(ImVec2(hi.x, lo.y), ImVec2(hi.x, hi.y), color, w);
        }
        // The header's floats, behind everything on the page: placed from the header's top.
        if (!d.flow.headerFloats.empty()) {
            dl->PushClipRect(ImVec2(left, top), ImVec2(left + pageW, top + pageH), true);
            for (const mv::FlowDocument::Float& f : d.flow.headerFloats)
                drawFloat(f, (ps.header - ps.top) * ppp);
            dl->PopClipRect();
        }
        // The header from its distance below the page's top edge, the footer up to its distance above the bottom.
        float y = top + ps.header * ppp;
        for (std::size_t i = 0; i < d.flow.header.size(); ++i) {
            y += e.header.blockGap(i);
            e.header.drawBlockAt(i, ImVec2(text.x, y), textW, fonts_, paper, fontSize, s_, &d.flow.fonts, p + 1, count);
            y += e.header.blockHeight(i);
        }
        y = top + pageH - ps.footer * ppp - footerH;
        for (std::size_t i = 0; i < d.flow.footer.size(); ++i) {
            y += e.footer.blockGap(i);
            e.footer.drawBlockAt(i, ImVec2(text.x, y), textW, fonts_, paper, fontSize, s_, &d.flow.fonts, p + 1, count);
            y += e.footer.blockHeight(i);
        }
        // Floating pictures where the document places them on the page: behind the text, or over it.
        auto floats = [&](bool behind) {
            dl->PushClipRect(ImVec2(left, top), ImVec2(left + pageW, top + pageH), true);
            for (const auto& [index, at] : page.floats) {
                const mv::FlowDocument::Float& f = d.flow.floats[index];
                if (f.behind == behind)
                    drawFloat(f, at + bodyShift);
            }
            dl->PopClipRect();
        };
        floats(true);
        // The body inside the margins; a block continued from the page before is cut at its top.
        const float bodyY = text.y + bodyShift; // the body's top: the margin, or below a header reaching past it
        dl->PushClipRect(ImVec2(left, bodyY), ImVec2(left + pageW, bodyY + bodyH), true); // across the page: tables may sit out in the margin
        for (const Entry::WordPage::Placed& b : page.blocks) {
            if (bodyY + b.top >= win.y + view.y || bodyY + b.top + b.height <= win.y)
                continue;
            if (b.clipBottom > 0.0f) // a table's rows on this page only
                dl->PushClipRect(ImVec2(left, bodyY + b.top + b.clipTop), ImVec2(left + pageW, bodyY + b.top + b.clipBottom), true);
            e.flow.drawBlockAt(b.block, ImVec2(text.x + b.inset, bodyY + b.top), b.width > 0.0f ? b.width : textW, fonts_, paper, fontSize, s_,
                               &d.flow.fonts, p + 1, count);
            if (b.clipBottom > 0.0f)
                dl->PopClipRect();
        }
        dl->PopClipRect();
        floats(false);
        // Footnotes at the foot of the text area, under a short rule.
        if (!page.notes.empty()) {
            float ny = bodyY + bodyH - page.notesH;
            dl->AddLine(ImVec2(text.x, ny - 6.0f * ppp), ImVec2(text.x + textW / 3.0f, ny - 6.0f * ppp), IM_COL32(0, 0, 0, 255), std::max(1.0f, 0.5f * ppp));
            for (std::size_t note : page.notes) {
                e.flow.drawBlockAt(note, ImVec2(text.x, ny), textW, fonts_, paper, fontSize, s_, &d.flow.fonts, p + 1, count);
                ny += e.flow.blockHeight(note) + 2.0f * ppp;
            }
        }
    }
    e.scroll = ImVec2(scrollX, scrollY);
    ImGui::EndChild();
}

void ViewerView::paginate(Entry& e, const mv::ViewerDocument& d, float textWidth, float textHeight, float pxPerPoint, const Palette& paper,
                          float fontSize)
{
    os::mem::release(e.wordPages);
    const std::vector<model::markdown::Block>& blocks = d.flow.blocks;
    const float rule = 14.0f * pxPerPoint; // the footnotes' rule and the room above it
    auto notesOf = [&](std::size_t block, std::vector<std::size_t>* into) {
        float h = 0.0f;
        for (const auto& [note, ref] : d.flow.footnotes)
            if (ref == block && note < blocks.size()) {
                h += e.flow.blockHeight(note) + 2.0f * pxPerPoint;
                if (into)
                    into->push_back(note);
            }
        return h;
    };
    // The page's floating pictures that the text goes round: their boxes in the text area, widened
    // by the distance they keep from the text.
    struct Keep {
        ImVec4 box;
        bool below = false; // the text goes on under it (else beside it)
    };
    std::vector<Keep> keeps;
    Entry::WordPage page;
    std::vector<float> rows; // a table's rows' bottoms, when it is broken between pages
    auto anchor = [&](std::size_t block, float top) {
        for (std::size_t f = 0; f < d.flow.floats.size(); ++f) {
            const mv::FlowDocument::Float& fl = d.flow.floats[f];
            if (fl.block != block)
                continue;
            page.floats.emplace_back(f, top);
            if (fl.wrap == 0)
                continue;
            const ImVec4 b = floatBox(fl, d.flow.page, pxPerPoint, top);
            keeps.push_back({ImVec4(b.x - fl.dist[0] * pxPerPoint, b.y - fl.dist[1] * pxPerPoint, b.z + fl.dist[2] * pxPerPoint,
                                    b.w + fl.dist[3] * pxPerPoint),
                             fl.wrap == 2});
        }
    };
    auto unanchor = [&](std::size_t block, std::size_t keepsFrom) {
        keeps.erase(keeps.begin() + std::ptrdiff_t(keepsFrom), keeps.end());
        page.floats.erase(std::remove_if(page.floats.begin(), page.floats.end(),
                                         [&](const std::pair<std::size_t, float>& f) { return d.flow.floats[f.first].block == block; }),
                          page.floats.end());
    };
    // A block at `top`: narrowed beside the pictures it would run into (on their wider side), or
    // under them (one the text goes under, or no room beside it).
    auto place = [&](std::size_t i, float top) {
        Entry::WordPage::Placed p{i, top, e.flow.blockHeight(i), 0.0f, 0.0f};
        auto meets = [&](const Keep& k) { return k.box.y < p.top + p.height && k.box.w > p.top; };
        for (const Keep& k : keeps)
            if (k.below && meets(k))
                p.top = k.box.w;
        float l = 0.0f, r = textWidth;
        for (const Keep& k : keeps) {
            if (k.below || !meets(k) || k.box.z <= l || k.box.x >= r)
                continue;
            if (k.box.x - l >= r - k.box.z)
                r = k.box.x; // the text on its left
            else
                l = k.box.z;
        }
        if (r - l < 72.0f * pxPerPoint) { // no room beside them: under them
            for (const Keep& k : keeps)
                if (meets(k))
                    p.top = std::max(p.top, k.box.w);
            l = 0.0f;
            r = textWidth;
        }
        if (l > 0.0f || r < textWidth) {
            p.inset = l;
            p.width = r - l;
            p.height = e.flow.measureBlockAt(i, p.width, fonts_, paper, fontSize, s_, &d.flow.fonts);
        }
        return p;
    };
    float y = 0.0f;
    for (std::size_t k = 0; k < d.flow.printed.size(); ++k) {
        const std::size_t i = d.flow.printed[k];
        if (i >= blocks.size())
            continue;
        const float notes = notesOf(i, nullptr);
        const float room = [&]() {
            const float all = page.notesH + notes;
            return textHeight - (all > 0.0f ? all + rule : 0.0f);
        }();
        float g = page.blocks.empty() ? 0.0f : e.flow.blockGap(i);
        const std::size_t keepsFrom = keeps.size();
        anchor(i, y + g);
        Entry::WordPage::Placed placed = place(i, y + g);
        const bool forced = blocks[i].doc().pageBreakBefore && !page.blocks.empty();
        // A document table runs on from page to page, broken between its rows (as Word breaks it):
        // it is not moved whole to the next page.
        const bool table = blocks[i].kind == model::markdown::BlockKind::Table && blocks[i].layout.p && !blocks[i].doc().looks.empty() && placed.width == 0.0f;
        if (!page.blocks.empty() && (forced || (placed.top + placed.height > room && !table))) {
            unanchor(i, keepsFrom);
            // A paragraph kept with the next one goes on with it (when it is not alone on its page).
            Entry::WordPage::Placed carried{0, -1.0f};
            if (!forced && page.blocks.size() > 1) {
                const std::size_t last = page.blocks.back().block;
                if (blocks[last].doc().keepNext && notesOf(last, nullptr) == 0.0f) {
                    carried = page.blocks.back();
                    page.blocks.pop_back();
                    unanchor(last, keeps.size());
                }
            }
            e.wordPages.push_back(std::move(page));
            page = Entry::WordPage();
            keeps.clear();
            y = 0.0f;
            g = 0.0f;
            if (carried.top >= 0.0f) {
                anchor(carried.block, 0.0f);
                carried = place(carried.block, 0.0f);
                page.blocks.push_back(carried);
                y = carried.top + carried.height;
                g = e.flow.blockGap(i);
            }
            anchor(i, y + g);
            placed = place(i, y + g);
        }
        if (table && placed.top + placed.height > room) {
            e.flow.tableRows(i, textWidth, fonts_, paper, fontSize, s_, &d.flow.fonts, rows);
            if (!rows.empty()) {
                const float total = placed.height;
                float top = placed.top, offset = 0.0f, avail = room;
                for (int pieces = 0; pieces < 1000; ++pieces) {
                    float cut = offset; // the last row ending on this page
                    for (float bottom : rows) {
                        if (bottom > offset + (avail - top) + 0.5f)
                            break;
                        cut = bottom;
                    }
                    if (cut <= offset + 0.5f) {
                        if (top > 0.5f) { // not even its next row fits here: on to the next page
                            e.wordPages.push_back(std::move(page));
                            page = Entry::WordPage();
                            keeps.clear();
                            top = 0.0f;
                            avail = textHeight;
                            continue;
                        }
                        cut = std::min(total, offset + avail); // a row taller than a page: cut inside it
                    }
                    Entry::WordPage::Placed piece = placed;
                    piece.top = top - offset;
                    piece.clipTop = offset;
                    piece.clipBottom = cut;
                    page.blocks.push_back(piece);
                    if (cut >= total - 0.5f) {
                        y = top + (total - offset);
                        break;
                    }
                    e.wordPages.push_back(std::move(page));
                    page = Entry::WordPage();
                    keeps.clear();
                    top = 0.0f;
                    avail = textHeight;
                    offset = cut;
                }
                page.notesH += notesOf(i, &page.notes);
                continue;
            }
        }
        page.blocks.push_back(placed);
        y = placed.top + placed.height;
        page.notesH += notesOf(i, &page.notes);
        // A block taller than the room left on an empty page: continued on the next pages.
        float avail = textHeight - (page.notesH > 0.0f ? page.notesH + rule : 0.0f);
        while (y > avail + 0.5f && page.blocks.size() == 1 && avail > 20.0f) {
            e.wordPages.push_back(std::move(page));
            page = Entry::WordPage();
            keeps.clear();
            placed.top -= avail;
            page.blocks.push_back(placed);
            y = placed.top + placed.height;
            avail = textHeight;
        }
    }
    if (!page.blocks.empty() || e.wordPages.empty())
        e.wordPages.push_back(std::move(page));
}

// ---- Excel: a grid that draws only the visible cells ---------------------------------------------

void ViewerView::sheet(Entry& e, const mv::ViewerDocument& d, ImVec2 size)
{
    const mv::Workbook& book = d.book;
    if (book.sheets.empty())
        return;
    if (e.sheet < 0) { // the first sheet not hidden
        e.sheet = 0;
        for (std::size_t i = 0; i < book.sheets.size(); ++i)
            if (!book.sheets[i].hidden) {
                e.sheet = int(i);
                break;
            }
    }
    e.sheet = std::clamp(e.sheet, 0, int(book.sheets.size()) - 1);
    const float pad = 8.0f * s_;
    // The cell inspector, then the grid, then the sheet tabs along the bottom (as in Excel).
    float top = ImGui::GetCursorPosY();
    const float tabsH = std::floor(ImGui::GetFontSize() * 1.45f);
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
    const ImVec2 gridSize(size.x, std::max(1.0f, size.y - (top - ImGui::GetCursorStartPos().y) - tabsH));

    // Geometry: Excel's column width is in characters of the default font (7 pixels each at 96
    // dpi, plus 5 pixels of padding); row heights are in points.
    const float zoom = vm_.zoom.get() * e.pinch;
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

    // The cells on white paper with Excel's grey grid and black automatic text, as Excel shows them.
    const ImU32 paperInk = IM_COL32(0, 0, 0, 255);
    panel::beginChild("##viewerSheet", gridSize, IM_COL32(255, 255, 255, 255), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoNav);
    touch::zoomView(e.pinch, 0.3f, 5.0f, e.pendingScroll); // a pinch: the cells larger or smaller
    ImGui::Dummy(ImVec2(totalW, totalH));
    const float scrollX = ImGui::GetScrollX(), scrollY = ImGui::GetScrollY();
    e.scroll = ImVec2(scrollX, scrollY);
    const ImVec2 win = ImGui::GetWindowPos();
    const ImVec2 view = ImGui::GetWindowSize();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const std::uint32_t fr = std::min(sh.frozenRows, rows), fc = std::min(sh.frozenColumns, columns);
    const float frozenW = e.columnX[fc], frozenH = rowTop(fr);
    const float bodyX = win.x + headerW + frozenW, bodyY = win.y + headerH + frozenH;
    const ImU32 grid = IM_COL32(212, 212, 212, 255);
    const ImU32 headerBg = IM_COL32(128, 128, 128, 30);

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

    // A cell's borders, on the grid lines around it (automatic colour: the text colour), and its diagonals.
    auto edges = [&](const mv::Border& border, ImVec2 a, ImVec2 b) {
        if (border.style[4] != mv::Border::None && (border.diagonalUp || border.diagonalDown)) {
            const ImU32 color = border.color[4] ? border.color[4] : paperInk;
            const float w = (border.style[4] == mv::Border::Medium ? 2.0f : border.style[4] == mv::Border::Thick ? 3.0f : 1.0f) * std::max(1.0f, s_);
            if (border.diagonalDown)
                dl->AddLine(ImVec2(a.x, a.y), ImVec2(b.x - 1.0f, b.y - 1.0f), color, w);
            if (border.diagonalUp)
                dl->AddLine(ImVec2(a.x, b.y - 1.0f), ImVec2(b.x - 1.0f, a.y), color, w);
        }
        const ImVec2 ends[4][2] = {{ImVec2(a.x - 0.5f, a.y - 0.5f), ImVec2(a.x - 0.5f, b.y - 0.5f)},
                                   {ImVec2(b.x - 0.5f, a.y - 0.5f), ImVec2(b.x - 0.5f, b.y - 0.5f)},
                                   {ImVec2(a.x - 0.5f, a.y - 0.5f), ImVec2(b.x - 0.5f, a.y - 0.5f)},
                                   {ImVec2(a.x - 0.5f, b.y - 0.5f), ImVec2(b.x - 0.5f, b.y - 0.5f)}};
        for (int i = 0; i < 4; ++i) {
            const std::uint8_t style = border.style[i];
            if (style == mv::Border::None)
                continue;
            ImU32 color = border.color[i] ? border.color[i] : paperInk;
            if (style == mv::Border::Hair)
                color = (color & 0x00FFFFFFu) | 0x90000000u;
            const float w = (style == mv::Border::Medium ? 2.0f : style == mv::Border::Thick ? 3.0f : 1.0f) * std::max(1.0f, s_);
            const ImVec2 p = ends[i][0], q = ends[i][1];
            if (style == mv::Border::Double) {
                const ImVec2 off = i < 2 ? ImVec2(1.0f * s_, 0.0f) : ImVec2(0.0f, 1.0f * s_);
                dl->AddLine(ImVec2(p.x - off.x, p.y - off.y), ImVec2(q.x - off.x, q.y - off.y), color, std::max(1.0f, s_));
                dl->AddLine(ImVec2(p.x + off.x, p.y + off.y), ImVec2(q.x + off.x, q.y + off.y), color, std::max(1.0f, s_));
            } else if (style == mv::Border::Dashed || style == mv::Border::Dotted) {
                const float dash = (style == mv::Border::Dotted ? 1.0f : 3.0f) * w, gap = (style == mv::Border::Dotted ? 1.0f : 2.0f) * w;
                const float len = i < 2 ? q.y - p.y : q.x - p.x;
                for (float t = 0.0f; t < len; t += dash + gap) {
                    const float t1 = std::min(len, t + dash);
                    dl->AddLine(i < 2 ? ImVec2(p.x, p.y + t) : ImVec2(p.x + t, p.y), i < 2 ? ImVec2(p.x, p.y + t1) : ImVec2(p.x + t1, p.y), color, w);
                }
            } else {
                dl->AddLine(p, q, color, w);
            }
        }
    };

    // A cell's text laid out in lines of pieces, each piece in one face and size (rich text runs,
    // the cells' own fonts), wrapped to the cell when it wraps, then turned when it is rotated. The
    // scratch is this frame's.
    struct Ink {
        ImFont* font = nullptr;
        bool fakeBold = false, slant = false; // the face lacks the weight or the slant: drawn twice, or sheared
        float emScale = 1.0f;                 // a document face: ImGui's size for an em
    };
    struct Piece {
        const char* begin;
        const char* end;
        float x, px; // within its line; the font size
        ImU32 color;
        std::uint8_t flags;
        Ink ink;
    };
    struct Line {
        std::size_t first = 0, last = 0;
        float ink = 0.0f, width = 0.0f, height = 0.0f; // ink: without the trailing spaces
    };
    std::vector<Piece> pieces;
    std::vector<Line> lines;
    // The face of a font the workbook names (0: its default font), in a weight and slant.
    auto inkFor = [&](std::uint8_t font, std::uint8_t flags) {
        const bool bold = (flags & mv::Cell::Bold) != 0, italic = (flags & mv::Cell::Italic) != 0;
        Ink ink{fonts_.ui, bold, italic};
        if (fonts_.documents) {
            const std::string& name = font > 0 && font <= book.fontNames.size() ? book.fontNames[font - 1u] : book.defaultFont;
            const DocumentFonts::Face face = fonts_.documents->face(name, bold, italic);
            if (face.font)
                ink = Ink{face.font, bold && !face.bold, italic && !face.italic, face.emScale};
        }
        return ink;
    };
    auto layout = [&](const mv::Cell& cell, float limit, float scale) {
        pieces.clear();
        lines.clear();
        const float basePx = (cell.halfPoints ? cell.halfPoints * 0.5f : book.defaultFontSize) * pxPerPoint * scale;
        const ImU32 baseColor = cell.color ? cell.color : paperInk;
        const bool wrap = (cell.flags & mv::Cell::Wrap) != 0, stacked = cell.rotation == 255;
        Line line;
        line.height = basePx;
        auto endLine = [&]() {
            line.last = pieces.size();
            lines.push_back(line);
            line = Line();
            line.first = pieces.size();
            line.height = basePx;
        };
        auto add = [&](const char* p, const char* end, float px, ImU32 color, std::uint8_t flags, const Ink& face) {
            while (p < end) {
                if (*p == '\n') { // a line break shows only in wrapped text
                    if (wrap || stacked)
                        endLine();
                    else if (!pieces.empty() || line.width > 0.0f)
                        line.width += face.font->CalcTextSizeA(px * face.emScale, FLT_MAX, 0.0f, " ").x;
                    ++p;
                    continue;
                }
                const char* w = p; // the next word with its spaces, or one letter when stacked
                if (stacked) {
                    unsigned int c = 0;
                    w += std::max(1, ImTextCharFromUtf8(&c, p, end));
                } else {
                    while (w < end && *w != ' ' && *w != '\n')
                        ++w;
                    while (w < end && *w == ' ')
                        ++w;
                }
                const char* inkEnd = w;
                while (inkEnd > p && inkEnd[-1] == ' ')
                    --inkEnd;
                Ink use = face;
                if (use.font != fonts_.ui && !DocumentFonts::covers(use.font, p, inkEnd))
                    use = Ink{fonts_.ui, (flags & mv::Cell::Bold) != 0, (flags & mv::Cell::Italic) != 0, 1.0f};
                const float inkW = use.font->CalcTextSizeA(px * use.emScale, FLT_MAX, 0.0f, p, inkEnd).x;
                const float fullW = inkEnd == w ? inkW : use.font->CalcTextSizeA(px * use.emScale, FLT_MAX, 0.0f, p, w).x;
                if (wrap && pieces.size() > line.first && line.width + inkW > limit)
                    endLine();
                Piece* last = pieces.size() > line.first ? &pieces.back() : nullptr;
                if (last && last->end == p && last->px == px && last->color == color && last->flags == flags && last->ink.font == use.font)
                    last->end = w; // the same face: one piece
                else
                    pieces.push_back(Piece{p, w, line.width, px, color, flags, use});
                line.ink = line.width + inkW;
                line.width += fullW;
                line.height = std::max(line.height, px);
                if (stacked)
                    endLine();
                p = w;
            }
        };
        const char* text = cell.text.data();
        const std::uint32_t size = std::uint32_t(cell.text.size());
        const std::uint8_t baseFlags = cell.flags & (mv::Cell::Bold | mv::Cell::Italic | mv::Cell::Underline | mv::Cell::Strike);
        const Ink baseInk = inkFor(cell.font, baseFlags);
        if (cell.rich && cell.rich < book.richTexts.size()) {
            std::uint32_t at = 0;
            for (const mv::CellRun& run : book.richTexts[cell.rich]) {
                const std::uint32_t b = std::min(run.begin, size), e = std::min(run.end, size);
                if (b > at)
                    add(text + at, text + b, basePx, baseColor, baseFlags, baseInk);
                if (e > b) {
                    if (run.own)
                        add(text + b, text + e, run.halfPoints ? run.halfPoints * 0.5f * pxPerPoint * scale : basePx,
                            run.color ? run.color : paperInk, run.flags, inkFor(run.font ? run.font : cell.font, run.flags));
                    else
                        add(text + b, text + e, basePx, baseColor, baseFlags, baseInk);
                }
                at = std::max(at, e);
            }
            if (at < size)
                add(text + at, text + size, basePx, baseColor, baseFlags, baseInk);
        } else {
            add(text, text + size, basePx, baseColor, baseFlags, baseInk);
        }
        if (pieces.size() > line.first || lines.empty())
            endLine();
    };
    // Text from `a` to `b`, which may run on over the empty cells from `overflowL` to `overflowR`.
    auto cellText = [&](const mv::Cell& cell, ImVec2 a, ImVec2 b, float overflowL, float overflowR, ImVec4 clip) {
        const float inset = 2.0f * 0.75f * pxPerPoint;                // Excel's 2 pixels at 96 dpi
        const float indent = float(cell.indent) * 9.0f * 0.75f * pxPerPoint; // about 9 pixels a level
        const int align = cell.align >= 0 ? cell.align : (cell.flags & mv::Cell::Number) ? 2 : 0;
        const bool turned = cell.rotation != 0 && cell.rotation != 255;
        const bool upright = turned && std::abs(cell.rotation) >= 45; // laid out along the cell's height
        const ImVec2 size(b.x - a.x, b.y - a.y);
        const float room = std::max(1.0f, (upright ? size.y : size.x) - 2.0f * inset - (align == 1 ? 0.0f : indent));
        layout(cell, room, 1.0f);
        const bool oneLine = !turned && lines.size() == 1 && !(cell.flags & mv::Cell::Wrap);
        // Shrink to fit: smaller until it fits. A number too wide for its cell shows as #### across it.
        if (oneLine && (cell.flags & mv::Cell::Shrink) && lines[0].ink > room)
            layout(cell, room, std::max(0.1f, room / lines[0].ink));
        if (oneLine && (cell.flags & mv::Cell::Number) && lines[0].ink > room && !pieces.empty()) {
            static const char kHashes[] = "################################################################";
            const Piece first = pieces[0];
            const float hashW = std::max(1.0f, first.ink.font->CalcTextSizeA(first.px * first.ink.emScale, FLT_MAX, 0.0f, "#").x);
            const int n = std::clamp(int(room / hashW), 1, int(sizeof(kHashes) - 1));
            pieces.assign(1, Piece{kHashes, kHashes + n, 0.0f, first.px, first.color, std::uint8_t(first.flags & mv::Cell::Bold), first.ink});
            lines[0].first = 0;
            lines[0].last = 1;
            lines[0].ink = lines[0].width = hashW * float(n);
        }
        float total = 0.0f;
        for (const Line& l : lines)
            total += l.height;
        // The box the text is laid out in, before it is turned about the cell's centre.
        const ImVec2 centre((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
        const ImVec2 box = upright ? ImVec2(size.y, size.x) : size;
        const ImVec2 at = turned ? ImVec2(centre.x - box.x * 0.5f, centre.y - box.y * 0.5f) : a;
        float y = turned || cell.valign == 1 ? at.y + (box.y - total) * 0.5f : cell.valign == 0 ? at.y + 1.0f * s_ : at.y + box.y - total - 2.0f * s_;
        // Text wider than its cell runs on over the empty cells beside it: to the right when it is
        // aligned left, to the left when right, both ways when centred.
        float left = a.x, right = b.x;
        if (oneLine && lines[0].ink > room) {
            if (align == 0) {
                right = std::max(right, overflowR);
            } else if (align == 2) {
                left = std::min(left, overflowL);
            } else {
                const float extra = (lines[0].ink - room) * 0.5f + inset;
                left = std::max(overflowL, a.x - extra);
                right = std::min(overflowR, b.x + extra);
            }
        }
        const ImVec4 cellClip(std::max(left, clip.x), std::max(a.y, clip.y), std::min(right, clip.z), std::min(b.y, clip.w));
        if (cellClip.x >= cellClip.z || cellClip.y >= cellClip.w)
            return;
        const int firstVertex = dl->VtxBuffer.Size;
        if (turned)
            dl->PushClipRect(ImVec2(cellClip.x, cellClip.y), ImVec2(cellClip.z, cellClip.w), true);
        const ImVec4* fine = turned ? nullptr : &cellClip; // glyphs are clipped before they are turned
        for (const Line& l : lines) {
            const float x = align == 1 ? at.x + (box.x - l.ink) * 0.5f : align == 2 ? at.x + box.x - inset - indent - l.ink : at.x + inset + indent;
            for (std::size_t i = l.first; i < l.last; ++i) {
                const Piece& p = pieces[i];
                const ImVec2 pos(x + p.x, y + l.height - p.px * p.ink.emScale); // its glyph box (an em is drawn at px)
                const int pieceVertex = dl->VtxBuffer.Size;
                dl->AddText(p.ink.font, p.px * p.ink.emScale, pos, p.color, p.begin, p.end, 0.0f, fine);
                if (p.ink.fakeBold)
                    dl->AddText(p.ink.font, p.px * p.ink.emScale, ImVec2(pos.x + std::max(1.0f, p.px * 0.045f), pos.y), p.color, p.begin, p.end, 0.0f, fine);
                if (p.ink.slant) { // italic without an italic face: sheared about the baseline
                    const float baseline = pos.y + p.px * 0.8f;
                    for (int v = pieceVertex; v < dl->VtxBuffer.Size; ++v)
                        dl->VtxBuffer[v].pos.x += (baseline - dl->VtxBuffer[v].pos.y) * 0.2f;
                }
                if (p.flags & (mv::Cell::Underline | mv::Cell::Strike)) {
                    const float w = p.ink.font->CalcTextSizeA(p.px * p.ink.emScale, FLT_MAX, 0.0f, p.begin, p.end).x;
                    const float lineY = (p.flags & mv::Cell::Underline) ? pos.y + p.px * 0.95f : pos.y + p.px * 0.55f;
                    const float x0 = turned ? pos.x : std::max(pos.x, cellClip.x), x1 = turned ? pos.x + w : std::min(pos.x + w, cellClip.z);
                    if (x1 > x0 && (turned || (lineY >= cellClip.y && lineY <= cellClip.w)))
                        dl->AddLine(ImVec2(x0, lineY), ImVec2(x1, lineY), p.color, std::max(1.0f, p.px / 14.0f));
                }
            }
            y += l.height;
        }
        if (turned) {
            const float t = float(cell.rotation) * 3.14159265f / 180.0f, cs = std::cos(t), sn = std::sin(t);
            for (int i = firstVertex; i < dl->VtxBuffer.Size; ++i) {
                ImVec2& v = dl->VtxBuffer[i].pos;
                const float dx = v.x - centre.x, dy = v.y - centre.y;
                v = ImVec2(centre.x + dx * cs + dy * sn, centre.y - dx * sn + dy * cs);
            }
            dl->PopClipRect();
        }
    };

    // A background other than one colour: a hatch's lines over the cell's colour, or a gradient.
    auto styledFill = [&](const mv::FillStyle& f, ImVec2 a, ImVec2 b) {
        const float pixel = 0.75f * pxPerPoint; // one pixel at 96 dpi
        if (f.kind == mv::FillStyle::Hatch) {
            dl->PushClipRect(a, b, true);
            const float step = 4.0f * pixel, w = (f.dark ? 2.0f : 1.0f) * pixel;
            const float h = b.y - a.y;
            auto straight = [&](bool across) {
                for (float t = (across ? a.y : a.x) + step * 0.5f; t < (across ? b.y : b.x); t += step)
                    dl->AddLine(across ? ImVec2(a.x, t) : ImVec2(t, a.y), across ? ImVec2(b.x, t) : ImVec2(t, b.y), f.color, w);
            };
            auto slanted = [&](bool down) {
                for (float t = a.x - h; t < b.x + h; t += step * 1.41f)
                    dl->AddLine(ImVec2(t, a.y), ImVec2(t + (down ? h : -h), b.y), f.color, w);
            };
            if (f.lines == mv::FillStyle::Horizontal || f.lines == mv::FillStyle::Grid)
                straight(true);
            if (f.lines == mv::FillStyle::Vertical || f.lines == mv::FillStyle::Grid)
                straight(false);
            if (f.lines == mv::FillStyle::Down || f.lines == mv::FillStyle::Trellis)
                slanted(true);
            if (f.lines == mv::FillStyle::Up || f.lines == mv::FillStyle::Trellis)
                slanted(false);
            dl->PopClipRect();
            return;
        }
        if (f.stopCount == 0)
            return;
        auto colorAt = [&](float t) {
            if (t <= f.position[0] || f.stopCount == 1)
                return ImU32(f.stop[0]);
            for (std::uint8_t i = 1; i < f.stopCount; ++i)
                if (t <= f.position[i]) {
                    const float span = f.position[i] - f.position[i - 1];
                    const float k = span > 0.0f ? (t - f.position[i - 1]) / span : 1.0f;
                    const ImVec4 c0 = ImGui::ColorConvertU32ToFloat4(f.stop[i - 1]), c1 = ImGui::ColorConvertU32ToFloat4(f.stop[i]);
                    return ImGui::ColorConvertFloat4ToU32(ImLerp(c0, c1, k));
                }
            return ImU32(f.stop[f.stopCount - 1]);
        };
        if (f.kind == mv::FillStyle::Path) { // from the centre's colour out to the edges'
            const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
            const ImVec2 corners[4] = {a, ImVec2(b.x, a.y), b, ImVec2(a.x, b.y)};
            const ImVec2 uv = dl->_Data->TexUvWhitePixel;
            const ImU32 inner = colorAt(0.0f), outer = colorAt(1.0f);
            dl->PrimReserve(12, 5);
            const ImDrawIdx base = ImDrawIdx(dl->_VtxCurrentIdx);
            dl->PrimWriteVtx(c, uv, inner);
            for (const ImVec2& p : corners)
                dl->PrimWriteVtx(p, uv, outer);
            for (int i = 0; i < 4; ++i) {
                dl->PrimWriteIdx(base);
                dl->PrimWriteIdx(ImDrawIdx(base + 1 + i));
                dl->PrimWriteIdx(ImDrawIdx(base + 1 + (i + 1) % 4));
            }
            return;
        }
        // Linear: along its direction; straight across or down, in bands between the stops.
        const float rad = f.degree * 3.14159265f / 180.0f, dx = std::cos(rad), dy = std::sin(rad);
        const int quarter = int(std::lround(f.degree / 90.0f)) & 3;
        if (std::fabs(f.degree - float(std::lround(f.degree / 90.0f)) * 90.0f) < 0.5f) {
            const bool across = quarter == 0 || quarter == 2, reverse = quarter >= 2;
            const float from = across ? a.x : a.y, len = across ? b.x - a.x : b.y - a.y;
            float positions[6] = {0.0f};
            int count = 0;
            positions[count++] = 0.0f;
            for (std::uint8_t i = 0; i < f.stopCount; ++i)
                if (f.position[i] > 0.0f && f.position[i] < 1.0f)
                    positions[count++] = f.position[i];
            positions[count++] = 1.0f;
            for (int i = 0; i + 1 < count; ++i) {
                const float t0 = positions[i], t1 = positions[i + 1];
                const ImU32 c0 = colorAt(t0), c1 = colorAt(t1);
                const float p0 = from + (reverse ? 1.0f - t1 : t0) * len, p1 = from + (reverse ? 1.0f - t0 : t1) * len;
                const ImU32 lo = reverse ? c1 : c0, hi = reverse ? c0 : c1;
                if (across)
                    dl->AddRectFilledMultiColor(ImVec2(p0, a.y), ImVec2(p1, b.y), lo, hi, hi, lo);
                else
                    dl->AddRectFilledMultiColor(ImVec2(a.x, p0), ImVec2(b.x, p1), lo, lo, hi, hi);
            }
            return;
        }
        // Slanted: each corner's colour from where it falls along the direction.
        const ImVec2 corners[4] = {a, ImVec2(b.x, a.y), b, ImVec2(a.x, b.y)};
        float proj[4], lo = FLT_MAX, hi = -FLT_MAX;
        for (int i = 0; i < 4; ++i) {
            proj[i] = corners[i].x * dx + corners[i].y * dy;
            lo = std::min(lo, proj[i]);
            hi = std::max(hi, proj[i]);
        }
        auto at = [&](int i) { return colorAt(hi > lo ? (proj[i] - lo) / (hi - lo) : 0.0f); };
        dl->AddRectFilledMultiColor(a, b, at(0), at(1), at(2), at(3));
    };

    // What conditional formatting draws in a cell: a data bar (Excel 2007's, fading to the right)
    // and an icon at the cell's left, about 16 pixels (at 96 dpi) high.
    auto mark = [&](const mv::CellMark& m, ImVec2 a, ImVec2 b) {
        const float pixel = 0.75f * pxPerPoint;
        if (m.bar >= 0.0f) {
            const ImVec2 p0(a.x + pixel, a.y + 2.0f * pixel), p1(a.x + pixel + (b.x - a.x - 3.0f * pixel) * m.bar, b.y - 3.0f * pixel);
            if (p1.x > p0.x && p1.y > p0.y) {
                const ImVec4 c = ImGui::ColorConvertU32ToFloat4(m.barColor);
                const ImU32 light = ImGui::ColorConvertFloat4ToU32(ImLerp(c, ImVec4(1.0f, 1.0f, 1.0f, 1.0f), 0.85f));
                dl->AddRectFilledMultiColor(p0, p1, m.barColor, light, light, m.barColor);
            }
        }
        if (m.icon == mv::CellMark::NoIcon)
            return;
        const float r = std::max(2.0f, std::min(8.0f * pixel, (b.y - a.y) * 0.5f - 2.0f * pixel));
        const ImVec2 c(a.x + 2.0f * pixel + r, (a.y + b.y) * 0.5f);
        const ImU32 col = m.iconColor, off = IM_COL32(210, 210, 210, 255), white = IM_COL32(255, 255, 255, 255);
        auto at = [&](float x, float y, float scale = 1.0f) { return ImVec2(c.x + x * r * scale, c.y + y * r * scale); };
        // The symbols, also drawn white inside a circle (scale < 1).
        auto check = [&](ImU32 ink, float k) {
            const ImVec2 pts[3] = {at(-0.7f, 0.0f, k), at(-0.2f, 0.5f, k), at(0.75f, -0.6f, k)};
            dl->AddPolyline(pts, 3, ink, r * 0.35f * k, ImDrawFlags_None);
        };
        auto cross = [&](ImU32 ink, float k) {
            dl->AddLine(at(-0.6f, -0.6f, k), at(0.6f, 0.6f, k), ink, r * 0.35f * k);
            dl->AddLine(at(-0.6f, 0.6f, k), at(0.6f, -0.6f, k), ink, r * 0.35f * k);
        };
        auto exclamation = [&](ImU32 ink, float k) {
            dl->AddRectFilled(at(-0.15f, -0.9f, k), at(0.15f, 0.3f, k), ink);
            dl->AddCircleFilled(at(0.0f, 0.7f, k), r * 0.18f * k, ink);
        };
        switch (m.icon) {
        case mv::CellMark::ArrowUp:
        case mv::CellMark::ArrowUpRight:
        case mv::CellMark::ArrowRight:
        case mv::CellMark::ArrowDownRight:
        case mv::CellMark::ArrowDown: {
            const float angle = float(int(m.icon) - int(mv::CellMark::ArrowRight)) * 0.785398f; // -90 up .. 90 down
            const ImVec2 d(std::cos(angle), std::sin(angle)), n(-d.y, d.x);
            const ImVec2 tip = at(d.x, d.y), base = at(d.x * 0.05f, d.y * 0.05f), tail = at(-d.x * 0.9f, -d.y * 0.9f);
            dl->AddLine(tail, base, col, r * 0.5f);
            dl->AddTriangleFilled(tip, ImVec2(base.x + n.x * r * 0.85f, base.y + n.y * r * 0.85f), ImVec2(base.x - n.x * r * 0.85f, base.y - n.y * r * 0.85f), col);
            break;
        }
        case mv::CellMark::Circle: dl->AddCircleFilled(c, r * 0.9f, col); break;
        case mv::CellMark::Flag:
            dl->AddLine(at(-0.6f, -0.95f), at(-0.6f, 0.95f), IM_COL32(90, 90, 90, 255), std::max(1.0f, r * 0.15f));
            dl->AddRectFilled(at(-0.55f, -0.95f), at(0.8f, 0.05f), col);
            break;
        case mv::CellMark::Check: check(col, 1.0f); break;
        case mv::CellMark::Cross: cross(col, 1.0f); break;
        case mv::CellMark::Exclamation: exclamation(col, 1.0f); break;
        case mv::CellMark::CheckCircled:
        case mv::CellMark::CrossCircled:
        case mv::CellMark::ExclamationCircled:
            dl->AddCircleFilled(c, r * 0.95f, col);
            if (m.icon == mv::CellMark::CheckCircled)
                check(white, 0.6f);
            else if (m.icon == mv::CellMark::CrossCircled)
                cross(white, 0.6f);
            else
                exclamation(white, 0.6f);
            break;
        case mv::CellMark::Diamond: dl->AddQuadFilled(at(0.0f, -0.95f), at(0.95f, 0.0f), at(0.0f, 0.95f), at(-0.95f, 0.0f), col); break;
        case mv::CellMark::TriangleUp: dl->AddTriangleFilled(at(0.0f, -0.85f), at(0.9f, 0.75f), at(-0.9f, 0.75f), col); break;
        case mv::CellMark::TriangleDown: dl->AddTriangleFilled(at(-0.9f, -0.75f), at(0.9f, -0.75f), at(0.0f, 0.85f), col); break;
        case mv::CellMark::Dash: dl->AddRectFilled(at(-0.8f, -0.25f), at(0.8f, 0.25f), col); break;
        case mv::CellMark::Star: {
            ImVec2 pts[10];
            for (int i = 0; i < 10; ++i) {
                const float t = -1.5707963f + float(i) * 0.6283185f, k = i % 2 ? 0.42f : 1.0f;
                pts[i] = at(std::cos(t) * k, std::sin(t) * k);
            }
            if (m.level > 0) { // full, or its left half
                if (m.level == 1)
                    dl->PushClipRect(ImVec2(c.x - r, c.y - r), ImVec2(c.x, c.y + r), true);
                for (int i = 0; i < 10; ++i)
                    dl->AddTriangleFilled(c, pts[i], pts[(i + 1) % 10], col);
                if (m.level == 1)
                    dl->PopClipRect();
            }
            dl->AddPolyline(pts, 10, col, std::max(1.0f, r * 0.12f), ImDrawFlags_Closed);
            break;
        }
        case mv::CellMark::Rating: // four bars rising left to right, `level` of them lit
            for (int i = 0; i < 4; ++i) {
                const float x0 = -0.95f + float(i) * 0.5f, top = 0.8f - float(i + 1) * 0.42f;
                dl->AddRectFilled(at(x0, top), at(x0 + 0.38f, 0.85f), i < m.level ? col : off);
            }
            break;
        case mv::CellMark::Quarters: // a circle filled a quarter at a time, clockwise from the top
            dl->AddCircleFilled(c, r * 0.9f, white);
            for (int q = 0; q < m.level && q < 4; ++q) {
                dl->PathLineTo(c);
                dl->PathArcTo(c, r * 0.9f, -1.5707963f + float(q) * 1.5707963f, float(q) * 1.5707963f, 8);
                dl->PathFillConvex(col);
            }
            dl->AddCircle(c, r * 0.9f, col, 0, std::max(1.0f, r * 0.12f));
            break;
        case mv::CellMark::Boxes: // four small boxes, `level` of them filled
            for (int i = 0; i < 4; ++i) {
                const float x0 = i % 2 ? 0.08f : -0.9f, y0 = i < 2 ? -0.9f : 0.08f;
                dl->AddRectFilled(at(x0, y0), at(x0 + 0.82f, y0 + 0.82f), i < m.level ? col : off);
            }
            break;
        default: break;
        }
    };

    // One region of cells: rows [r0, r1), columns [c0, c1), placed so that row `rBase` / column
    // `cBase` is at (x0, y0), clipped to `clip`.
    auto region = [&](std::uint32_t r0, std::uint32_t r1, std::uint32_t c0, std::uint32_t c1, std::uint32_t rBase, std::uint32_t cBase, float x0,
                      float y0, ImVec4 clip) {
        if (r0 >= r1 || c0 >= c1)
            return;
        dl->PushClipRect(ImVec2(clip.x, clip.y), ImVec2(clip.z, clip.w), true);
        const float baseTop = rowTop(rBase);
        if (sh.showGrid) { // under the cells: filled cells and merges hide them, as in Excel
            for (std::uint32_t c = c0; c <= c1 && c <= columns; ++c) {
                const float x = x0 + e.columnX[c] - e.columnX[cBase] - 0.5f;
                if (x > clip.z)
                    break;
                dl->AddLine(ImVec2(x, clip.y), ImVec2(x, clip.w), grid);
            }
            float y = y0 + rowTop(r0) - baseTop;
            for (std::uint32_t r = r0; r < r1 && y <= clip.w; ++r) {
                const float h = rowHeight(r);
                if (h > 0.5f)
                    dl->AddLine(ImVec2(clip.x, y + h - 0.5f), ImVec2(clip.z, y + h - 0.5f), grid);
                y += h;
            }
        }
        // A cell at row r (its top at y, the row h high) through row cr1 / column cc1 when merged;
        // text may run on from column `from` to column `stop` (after the cell before it with
        // something in it, up to the next).
        auto drawCell = [&](const mv::Cell& cell, std::uint32_t r, std::uint32_t cr1, std::uint32_t cc1, float y, float h, std::uint32_t from,
                            std::uint32_t stop) {
            const float cx = x0 + e.columnX[cell.column] - e.columnX[cBase];
            const float cw = e.columnX[std::min(cc1 + 1, columns)] - e.columnX[cell.column];
            const float ch = cr1 > r ? rowTop(cr1 + 1) - rowTop(r) : h;
            const ImVec2 a(cx, y), b(cx + cw, y + ch);
            if (cell.fill)
                dl->AddRectFilled(a, ImVec2(b.x - 1.0f, b.y - 1.0f), cell.fill);
            if (cell.fillStyle && cell.fillStyle < book.fills.size())
                styledFill(book.fills[cell.fillStyle], a, ImVec2(b.x - 1.0f, b.y - 1.0f));
            if (!sh.marks.empty())
                if (const mv::CellMark* m = sh.mark(r, cell.column))
                    mark(*m, a, b);
            if (cell.border && cell.border < book.borders.size())
                edges(book.borders[cell.border], a, b);
            if (cell.text.empty() || cw < 2.0f)
                return;
            // Text wider than its cell runs on over the empty cells beside it, as in Excel (text
            // that is not in a merge; numbers never do).
            float overflowL = a.x, overflowR = b.x;
            if (cc1 == cell.column && cr1 == r && !(cell.flags & mv::Cell::Number)) {
                overflowL = x0 + e.columnX[std::min(from, columns)] - e.columnX[cBase];
                overflowR = x0 + e.columnX[std::min(stop, columns)] - e.columnX[cBase];
            }
            cellText(cell, a, b, overflowL, overflowR, clip);
        };
        // Merges reaching into this region from outside it (from frozen rows or columns, or
        // scrolled past): their first cell is not among the region's.
        if (!sh.merges.empty() && sh.merges.size() <= 512)
            for (const mv::Merge& m : sh.merges) {
                if ((m.row0 >= r0 && m.col0 >= c0) || m.row1 < r0 || m.row0 >= r1 || m.col1 < c0 || m.col0 >= c1)
                    continue;
                if (const mv::Cell* cell = sh.cell(m.row0, m.col0))
                    drawCell(*cell, m.row0, m.row1, m.col1, y0 + rowTop(m.row0) - baseTop, rowHeight(m.row0), 0, columns);
            }
        float y = y0 + rowTop(r0) - baseTop;
        for (std::uint32_t r = r0; r < r1; ++r) {
            const mv::Row* row = sh.row(r);
            const float h = row && row->height > 0.0f ? row->height * pxPerPoint : defaultH;
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
                    const auto next = std::next(it);
                    const std::uint32_t from = it != row->cells.begin() ? std::prev(it)->column + 1 : 0;
                    drawCell(cell, r, cr1, cc1, y, h, from, next != row->cells.end() ? next->column : columns);
                }
            }
            y += h;
            if (y > clip.w)
                break;
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
    const float sx = bodyX - scrollX; // x of column fc in the scrolled part
    const float sy = bodyY - scrollY; // y of row fr
    region(r0, r1, c0, c1, fr, fc, sx, sy, ImVec4(bodyX, bodyY, viewRight, viewBottom));
    // Pictures and charts over the cells, placed by their anchor cells (in the scrolled part).
    if (!sh.drawings.empty()) {
        EntryImageSource images(*this, e);
        dl->PushClipRect(ImVec2(bodyX, bodyY), ImVec2(viewRight, viewBottom), true);
        for (const mv::SheetDrawing& drawing : sh.drawings) {
            const std::uint32_t col = std::min(drawing.col0, columns);
            const ImVec2 at(sx + e.columnX[col] - e.columnX[fc] + drawing.dx0 * pxPerPoint,
                            sy + rowTop(drawing.row0) - rowTop(fr) + drawing.dy0 * pxPerPoint);
            const ImVec2 end(at.x + drawing.page.width * pxPerPoint, at.y + drawing.page.height * pxPerPoint);
            if (end.x < bodyX || end.y < bodyY || at.x > viewRight || at.y > viewBottom)
                continue;
            painter_.draw(drawing.page, at, pxPerPoint, fonts_, &images);
        }
        dl->PopClipRect();
    }
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
    sheetTabs(e, book, ImVec2(size.x, tabsH));
}

// A thin strip of sheet tabs under the grid: the shown sheet lit like a page tab, the others plain;
// it scrolls sideways (wheel, or shift+wheel) when the names do not fit.
void ViewerView::sheetTabs(Entry& e, const mv::Workbook& book, ImVec2 size)
{
    panel::beginChild("##viewerSheetTabs", size, palette_.panelBg, ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoNav);
    if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0.0f) // the plain wheel scrolls the tabs sideways
        ImGui::SetScrollX(ImGui::GetScrollX() - ImGui::GetIO().MouseWheel * 40.0f * s_);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float padX = 10.0f * s_, h = size.y;
    ImGui::SetCursorPos(ImVec2(6.0f * s_, 0.0f));
    for (std::size_t i = 0; i < book.sheets.size(); ++i) {
        if (book.sheets[i].hidden)
            continue;
        const char* name = book.sheets[i].name.empty() ? "(sheet)" : book.sheets[i].name.c_str();
        const ImVec2 text = ImGui::CalcTextSize(name);
        ImGui::PushID(int(i));
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const bool current = int(i) == e.sheet;
        if (ImGui::InvisibleButton("##sheet", ImVec2(text.x + padX * 2.0f, h)))
            e.sheet = int(i);
        const bool hovered = ImGui::IsItemHovered();
        registerLastItem("viewer:sheet:" + std::to_string(i), "tab", std::string("Sheet ") + name, [&e, i]() { e.sheet = int(i); }, true, false);
        const ImVec2 end(at.x + text.x + padX * 2.0f, at.y + h);
        if (current) {
            dl->AddRectFilled(at, end, palette_.editorBg);
            dl->AddLine(ImVec2(at.x, end.y - 1.5f * s_), ImVec2(end.x, end.y - 1.5f * s_), palette_.accent, 2.0f * s_);
        } else if (hovered) {
            dl->AddRectFilled(at, end, palette_.hover);
        }
        dl->AddText(ImVec2(at.x + padX, at.y + (h - text.y) * 0.5f), current ? palette_.text : palette_.textDim, name);
        dl->AddLine(ImVec2(end.x, at.y + h * 0.25f), ImVec2(end.x, at.y + h * 0.75f), palette_.border, 1.0f); // between tabs
        ImGui::PopID();
        ImGui::SameLine(0.0f, 0.0f);
    }
    ImGui::EndChild();
}

// ---- PowerPoint and PDF: pages drawn by the page painter -----------------------------------------

void ViewerView::pages(Entry& e, const mv::ViewerDocument& d, ImVec2 size)
{
    const mv::PageDocument& pd = d.pages;
    const int n = int(pd.pages.size());
    const bool slides = d.format != "PDF document";
    panel::beginChild("##viewerPages", size, IM_COL32(110, 110, 110, 60), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoNav | ImGuiWindowFlags_HorizontalScrollbar);
    {
        float z = e.zoom > 0.0f ? e.zoom : 1.0f; // a pinch: from the fit (0) or the zoom set
        if (touch::zoomView(z, 0.25f, 8.0f, e.pendingScroll))
            e.zoom = z;
    }
    const float viewW = ImGui::GetContentRegionAvail().x;
    const float pad = 16.0f * s_, gap = 14.0f * s_;
    float widest = 1.0f;
    for (const mv::Page& p : pd.pages)
        widest = std::max(widest, p.width);
    float fit = std::max(0.05f, (viewW - pad * 2.0f) / widest);
    if (slides) { // a slide is seen whole: fit its height too
        float tallest = 1.0f;
        for (const mv::Page& p : pd.pages)
            tallest = std::max(tallest, p.height);
        fit = std::min(fit, std::max(0.05f, (ImGui::GetWindowSize().y - pad * 2.0f) / tallest));
    }
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
    if (e.goToPage >= 0 && e.goToPage < n) { // the toolbar's arrows
        ImGui::SetScrollY(std::max(0.0f, e.pageTop[size_t(e.goToPage)] - pad));
        e.currentPage = e.goToPage;
        e.goToPage = -1;
    }
    const float scrollY = ImGui::GetScrollY();
    // How fast the pages move: while they fly past (a fling) faster than about two screens a
    // second, no page is asked for: the ones passed would only be cancelled again. A jump (Home,
    // the page arrows, another document shown) is not movement: the page landed on is read at once.
    const float dt = std::max(1.0f / 240.0f, ImGui::GetIO().DeltaTime);
    const float moved = std::fabs(scrollY - e.scroll.y);
    e.scrollSpeed = moved > ImGui::GetWindowSize().y ? 0.0f : e.scrollSpeed * 0.5f + moved / dt * 0.5f;
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
    // The current page: the one under the view's upper third (not one whose bottom edge still shows).
    e.currentPage = first;
    for (int i = first; i <= last && i < n; ++i)
        if (e.pageTop[size_t(i)] <= scrollY + viewH * 0.35f)
            e.currentPage = i;
    // The view-model reads the pages in view (and their neighbours) through the reader service,
    // and drops pages far away; the pictures of a page no longer read go here.
    if (n > 0) {
        const bool flying = e.scrollSpeed > ImGui::GetWindowSize().y * 2.0f;
        vm_.viewer().showPages(e.id, size_t(first), size_t(std::max(first, last)), size_t(e.currentPage), !flying);
    }
    for (auto it = e.pageImages.begin(); it != e.pageImages.end();) {
        if (it->first < pd.pages.size() && pd.pages[it->first].loaded) {
            ++it;
            continue;
        }
        releaseImages(it->second);
        it = e.pageImages.erase(it);
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
        e.scripts |= page.scripts;
        painter_.draw(page, at, scale, fonts_, &source);
        if (!page.loaded) // being read by the reader service
            dl->AddText(ImVec2(at.x + 12.0f * s_, at.y + 10.0f * s_), IM_COL32(120, 120, 120, 255), ICON_FA_HOURGLASS_HALF "  Reading the page...");
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
        if (ImGui::IsKeyPressed(ImGuiKey_Home))
            e.goToPage = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_End))
            e.goToPage = n - 1;
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown) && first + 1 < n)
            ImGui::SetScrollY(e.pageTop[size_t(first + 1)] - pad);
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp))
            ImGui::SetScrollY(e.pageTop[size_t(std::max(0, first - (scrollY > e.pageTop[size_t(first)] - pad + 1.0f ? 0 : 1)))] - pad);
    }
    ImGui::EndChild();
}

} // namespace enp::view
