// Draws documents read for viewing (viewmodel::ViewerViewModel): Word documents reflowed by the
// Markdown renderer, Excel workbooks as a cell grid, PowerPoint slides and PDF pages drawn by the
// page painter, and "Loading" while the reader service reads them. For now it lives in the Code
// workspace (EditorView draws it for FileKind::Document); it can move to a workspace of its own.
//
// A view only: it neither reads files nor owns documents. It tells the view-model which pages are
// on screen (showPages) and keeps per document only what drawing needs: the rendered layout,
// scroll and selection, and the pictures it decoded (from the view-model's bytes, takeMedia).
//
// Memory (docs/MEMORY.md): an entry and its pictures go when its document closes, is opened as
// text, or is no longer in the view-model (releaseClosed); a page's pictures go when the page is
// dropped. The views are three child windows shared by every document; a document's scroll is
// kept in its entry. Nothing is allocated per frame (labels go through a fixed buffer).
#pragma once

#include "enp/model/Document.h"
#include "enp/view/Fonts.h"
#include "enp/view/MarkdownImages.h"
#include "enp/view/MarkdownView.h"
#include "enp/view/Theme.h"
#include "enp/view/viewer/PagePainter.h"
#include "enp/viewmodel/NotepadViewModel.h"

#include "imgui.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace enp::view {

class ViewerView {
public:
    ViewerView(viewmodel::NotepadViewModel& vm, const Fonts& fonts, const Palette& palette, float uiScale, MarkdownImages& images,
               EmojiCache*& emoji);
    ~ViewerView();
    ViewerView(const ViewerView&) = delete;
    ViewerView& operator=(const ViewerView&) = delete;

    // Draws the document at the cursor, filling `size`.
    void render(model::Document& doc, ImVec2 size);

    void release(int documentId); // opened as text: the view's caches go at once
    void releaseClosed();         // every entry whose document has closed
    bool holds(int documentId) const { return entries_.count(documentId) != 0; }
    std::size_t entryCount() const { return entries_.size(); }
    std::size_t heldImageCount() const; // for the tests: pictures decoded for open documents
    // The scripts (model::viewer::Script bits) of what the open documents have shown: the fonts
    // they need merged in (ScriptFonts).
    std::uint8_t scripts() const;

private:
    struct HeldImage {
        std::string key; // in the image cache
        const MarkdownImage* image = nullptr;
    };
    struct Entry {
        int id = -1;
        std::uint64_t generation = 0; // the view-model's: caches start over when it changes
        std::string keyPrefix;        // the file's path + '#', for image cache keys
        std::map<std::string, HeldImage> images;                            // Word and Excel documents
        std::map<std::size_t, std::map<std::string, HeldImage>> pageImages; // slides and pages: by page
        MarkdownView flow;
        ImVec2 scroll{0.0f, 0.0f};
        // Sheet
        int sheet = -1;
        std::uint32_t selRow = 0, selCol = 0;
        std::vector<float> columnX; // the shown sheet's column edges, at `columnScale`
        float columnScale = 0.0f;
        int columnSheet = -1;
        // Pages
        float zoom = 0.0f;          // 0: fit to the width
        std::vector<float> pageTop; // at `pageScale`
        float pageScale = 0.0f;
        int currentPage = 0;
        int goToPage = -1;          // scroll to this page's top next frame (the toolbar's arrows, Home / End)
        std::uint8_t scripts = 0;   // of the document and the pages it has shown
        // Word: the page view (else reflowed); its pages as laid out at `wordScale` for `wordGeneration`.
        bool reflow = false;
        struct WordPage {
            struct Placed {
                std::size_t block = 0;
                float top = 0.0f;    // in the page's text area (pixels; < 0: continued from the page before)
                float height = 0.0f; // at its width
                float inset = 0.0f;  // beside a floating picture: from the text area's left edge
                float width = 0.0f;  // and narrowed to (0: the text area's width)
            };
            std::vector<Placed> blocks;
            std::vector<std::pair<std::size_t, float>> floats; // floating pictures (into the document's), their paragraph's top
            std::vector<std::size_t> notes;                    // its footnotes' blocks
            float notesH = 0.0f;
        };
        std::vector<WordPage> wordPages;
        float wordScale = 0.0f;
        std::uint64_t wordGeneration = 0;
        MarkdownView header, footer; // every page's
    };
    // A page's pictures, decoded for that page (and released with it).
    class PageImageSource final : public PageImages {
    public:
        PageImageSource(ViewerView& view, Entry& entry, std::size_t page) : view_(view), entry_(entry), page_(page) {}
        const MarkdownImage* image(const std::string& media) override { return view_.image(entry_, entry_.pageImages[page_], media); }

    private:
        ViewerView& view_;
        Entry& entry_;
        std::size_t page_;
    };

    // The pictures of a Word or Excel document's drawings: held for the whole document.
    class EntryImageSource final : public PageImages {
    public:
        EntryImageSource(ViewerView& view, Entry& entry) : view_(view), entry_(entry) {}
        const MarkdownImage* image(const std::string& media) override { return view_.image(entry_, entry_.images, media); }

    private:
        ViewerView& view_;
        Entry& entry_;
    };

    Entry& entryFor(const model::Document& doc, const viewmodel::ViewerViewModel::Entry& model);
    void releaseImages(std::map<std::string, HeldImage>& held);
    void releaseEntry(Entry& e);
    const MarkdownImage* image(Entry& e, std::map<std::string, HeldImage>& held, const std::string& media);
    void toolbar(model::Document& doc, Entry& e, const model::viewer::ViewerDocument& d, float width);
    void flow(Entry& e, const model::viewer::ViewerDocument& d, ImVec2 size);
    void wordPages(Entry& e, const model::viewer::ViewerDocument& d, ImVec2 size, const Palette& paper);
    void paginate(Entry& e, const model::viewer::ViewerDocument& d, float textWidth, float textHeight, float pxPerPoint, const Palette& paper,
                  float fontSize);
    void sheet(Entry& e, const model::viewer::ViewerDocument& d, ImVec2 size);
    void sheetTabs(Entry& e, const model::viewer::Workbook& book, ImVec2 size);
    void pages(Entry& e, const model::viewer::ViewerDocument& d, ImVec2 size);
    void loading(model::Document& doc, ImVec2 size);
    void unreadable(model::Document& doc, const std::string& format, const std::string& error, ImVec2 size);

    viewmodel::NotepadViewModel& vm_;
    const Fonts& fonts_;
    const Palette& palette_;
    float s_;
    MarkdownImages& images_;
    EmojiCache*& emoji_;
    PagePainter painter_;
    std::map<int, Entry> entries_; // by document id: documents shown here only
    int shownDoc_ = -1;            // the document the shared windows showed last
    char label_[512] = {};         // scratch for labels drawn this frame (no allocation per frame)
};

} // namespace enp::view
