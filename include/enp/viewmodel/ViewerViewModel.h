// The documents read for viewing (Word, Excel, PowerPoint, PDF): their state and their models, by
// document id. Reading happens in the reader service (IDocumentReaderService, off the UI thread);
// this decides what to read and when to let go. A PDF's pages are read as they come near the view
// and dropped again when far away: first a page's content (quick: its text shows at once), then
// each of its pictures, shrunk to the size the page shows them. The service is told which page
// is in view, so it serves that page first; reads no longer near the view are cancelled.
//
// Memory (docs/MEMORY.md): a document's model, the encoded images not decoded yet, and the reads
// in flight are released when the document closes or is opened as text (release(): its reads are
// cancelled, a late result is dropped by the service), with a trim request. Callbacks hold a
// weak_ptr to this and the entry's generation: a result for a closed or reopened document is
// dropped. The `viewer` scope counts it all.
#pragma once

#include "enp/interfaces/IDocumentReaderService.h"
#include "enp/model/viewer/Viewer.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace enp::viewmodel {

class ViewerViewModel {
public:
    enum class State { Loading, Ready, Failed };

    struct Entry {
        int id = -1;
        std::string path;
        State state = State::Loading;
        model::viewer::ViewerDocument doc;
        std::uint64_t generation = 0; // changes when the document is read again: the views' caches start over
        // Reads in flight: the document's, then its pages' (page index -> ticket).
        interfaces::IDocumentReaderService::Ticket ticket = 0;
        std::map<std::size_t, interfaces::IDocumentReaderService::Ticket> pageTickets;
        // A PDF's pictures asked for, by media key: being read (ticket), or read (`done`: its bytes
        // in the document's media until the view takes them, or none when it cannot be read).
        // Forgotten with the page that asked (dropPage), or when a read is cancelled.
        struct Picture {
            interfaces::IDocumentReaderService::Ticket ticket = 0;
            std::size_t page = 0;
            bool done = false;
        };
        std::map<std::string, Picture> pictures;
    };

    // Pages kept read on each side of the ones shown; pages further away are dropped.
    static constexpr std::size_t kPagesAround = 1;
    static constexpr std::size_t kPagesKept = 6;

    // `requestTrim`: called after something large was released (the owner trims memory).
    ViewerViewModel(interfaces::IDocumentReaderService* reader, std::function<void()> requestTrim);
    ~ViewerViewModel();
    ViewerViewModel(const ViewerViewModel&) = delete;
    ViewerViewModel& operator=(const ViewerViewModel&) = delete;

    // Starts reading the document at `absolutePath` for document `id`, unless it is read or being read.
    void open(int id, const std::string& absolutePath);
    const Entry* find(int id) const;
    // A picture's size: shrunk to this many pixels for each point of the page it fills (a page
    // seen at about twice its size on a high-density screen), and never more than kMaxPictureSide.
    static constexpr float kPicturePixelsPerPoint = 3.0f;
    static constexpr int kMaxPictureSide = 2048;

    // The view shows pages [first, last], `current` the one being read: those and kPagesAround on
    // each side are read (content, then pictures), nearest `current` first; reads further away are
    // cancelled, and pages more than kPagesKept away dropped (their content and images). `request`
    // false (the content flying past): nothing new is asked for, only what is not wanted cancelled.
    void showPages(int id, std::size_t first, std::size_t last, std::size_t current, bool request = true);
    // The picture `key` of a PDF page is still being read (or not asked for yet): the view draws a
    // placeholder and does not take its bytes.
    bool pictureWaiting(int id, const std::string& key) const;
    // An image's encoded bytes, moved out (the view decodes them once; a page read again puts them
    // back). Empty when there are none.
    std::string takeMedia(int id, const std::string& key);
    // Everything about document `id` goes: its model, its images' bytes, its reads in flight.
    void release(int id);
    void releaseAll();

    std::size_t entryCount() const { return entries_.size(); }
    std::size_t loadingCount() const; // documents, pages and pictures being read

private:
    void readPage(Entry& e, std::size_t index);
    void readPictures(Entry& e, std::size_t index);
    void dropPage(Entry& e, std::size_t index);

    interfaces::IDocumentReaderService* reader_;
    std::function<void()> requestTrim_;
    std::map<int, Entry> entries_;
    std::uint64_t nextGeneration_ = 1;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true); // callbacks hold a weak_ptr
};

} // namespace enp::viewmodel
