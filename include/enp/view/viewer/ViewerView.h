// Shows documents read by model::viewer in the editor area: Word documents reflowed by the
// Markdown renderer, Excel workbooks as a cell grid, PowerPoint slides and PDF pages drawn by the
// page painter. For now it lives in the Code workspace (EditorView draws it for FileKind::Document);
// it depends only on the document, the view-model's file reading and the shared image cache, so it
// can move to a workspace of its own.
//
// Memory (docs/MEMORY.md): a document's parsed form, its view state and the images it acquired
// are held only while it is open: released when it closes (releaseClosed), when it is opened as
// text (release), and the memory goes back to the OS (the view-model's trim request). The views
// are three child windows shared by every document; a document's scroll is kept in its entry. PDF
// pages are read as they come into view and dropped again when far away, with their images.
#pragma once

#include "enp/model/Document.h"
#include "enp/model/viewer/Viewer.h"
#include "enp/view/Fonts.h"
#include "enp/view/MarkdownImages.h"
#include "enp/view/MarkdownView.h"
#include "enp/view/Theme.h"
#include "enp/view/viewer/PagePainter.h"
#include "enp/viewmodel/NotepadViewModel.h"

#include "imgui.h"

#include <cstdint>
#include <map>
#include <memory>
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

    void release(int documentId); // opened as text: everything it held goes at once
    void releaseClosed();         // every entry whose document has closed
    bool holds(int documentId) const { return entries_.count(documentId) != 0; }
    std::size_t entryCount() const { return entries_.size(); }
    std::size_t heldImageCount() const; // for the tests: images acquired by open documents

private:
    struct HeldImage {
        std::string key; // in the image cache
        const MarkdownImage* image = nullptr;
    };
    struct Entry {
        int id = -1;
        model::viewer::ViewerDocument doc;
        std::string keyPrefix;               // the file's path + '#', for image cache keys
        std::map<std::string, HeldImage> images; // by media key: Flow and Sheet documents, or per page below
        std::map<std::size_t, std::map<std::string, HeldImage>> pageImages; // Pages: by page, released with it
        MarkdownView flow;
        ImVec2 scroll{0.0f, 0.0f};
        // Sheet
        int sheet = 0;
        std::uint32_t selRow = 0, selCol = 0;
        std::vector<float> columnX; // the shown sheet's column edges, at `columnScale`
        float columnScale = 0.0f;
        int columnSheet = -1;
        // Pages
        float zoom = 0.0f;          // 0: fit to the width
        std::vector<float> pageTop; // at `pageScale`
        float pageScale = 0.0f;
        int currentPage = 0;
    };

    // A page's pictures, acquired for that page (and released with it).
    class PageImageSource final : public PageImages {
    public:
        PageImageSource(ViewerView& view, Entry& entry, std::size_t page) : view_(view), entry_(entry), page_(page) {}
        const MarkdownImage* image(const std::string& media) override
        {
            return view_.image(entry_, entry_.pageImages[page_], media, entry_.doc.pages.media);
        }

    private:
        ViewerView& view_;
        Entry& entry_;
        std::size_t page_;
    };

    Entry& entryFor(model::Document& doc);
    void releaseEntry(Entry& e);
    const MarkdownImage* image(Entry& e, std::map<std::string, HeldImage>& held, const std::string& media,
                               std::map<std::string, std::string>& bytes);
    void toolbar(model::Document& doc, Entry& e, float width);
    void flow(model::Document& doc, Entry& e, ImVec2 size);
    void sheet(Entry& e, ImVec2 size);
    void pages(Entry& e, ImVec2 size);
    void unreadable(model::Document& doc, Entry& e, ImVec2 size);

    viewmodel::NotepadViewModel& vm_;
    const Fonts& fonts_;
    const Palette& palette_;
    float s_;
    MarkdownImages& images_;
    EmojiCache*& emoji_;
    PagePainter painter_;
    std::map<int, Entry> entries_; // by document id: open documents shown here only
    int shownDoc_ = -1;            // the document the shared windows showed last
    char label_[512] = {};         // scratch for labels drawn this frame (no allocation per frame)
};

} // namespace enp::view
