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
//
// Search (PDF, slides): one document at a time, the active one while the find bar is open
// (search()). Every page's text is read once (the pages not read yet through the reader service,
// one at a time, after the pages being shown) and kept while the search lasts: a new query is
// counted again at once. The texts go when the search ends, moves to another document, or the
// document closes.
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
        // A PDF protected by a password (doc.needsPassword): the last one typed did not open it.
        bool passwordRefused = false;
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
            bool failed = false; // read, and it cannot be (its bytes are empty)
        };
        std::map<std::string, Picture> pictures;
        // Pictures read and shrunk, kept (a copy) for when their page is read again: a page
        // scrolled back to shows them at once. The least recently used go first past
        // kPictureCacheBytes; all go with the document.
        struct Cached {
            std::string bytes;
            std::uint64_t used = 0;
        };
        std::map<std::string, Cached> pictureCache;
        std::size_t pictureCacheBytes = 0;
        std::uint64_t pictureClock = 0;
        // Where each view showing it is (two columns can show one document at different pages):
        // the pages it shows, the one being read, and which way the reader goes (+1 down, -1 up:
        // the next page that way is read ahead). A page is kept while it is near any of them.
        struct Shown {
            std::size_t first = 0, last = 0, from = 0, to = 0; // shown, and read (with those around)
            std::size_t current = 0;
            int direction = 1;
        };
        std::map<const void*, Shown> shown;

        // The text search in its pages (search()).
        struct Search {
            bool active = false;
            std::string query;
            bool matchCase = false;
            std::vector<std::string> texts; // by page: its text (model::viewer::pageText), once read
            std::vector<int> counts;        // by page: its matches; -1 while its text is not read
            int total = 0;                  // on the pages searched so far
            std::size_t searched = 0;       // pages whose text is read
            interfaces::IDocumentReaderService::Ticket ticket = 0; // the text being read
            std::size_t from = 0;           // the page the reader is on: texts are read from there on
            // The current match: the `ordinal`-th on page `page`; page -1: none chosen yet.
            int page = -1;
            int ordinal = 0;
            int step = 0; // a move to the next (+1) or previous (-1) match waiting for pages not searched yet
            int first = -1; // the page whose text is read first (the move waits for it), -1 none
            std::uint64_t reveal = 0; // changes each time the current match is to be brought into view
        };
        Search search;
    };

    // The search, as the find bar shows it.
    struct SearchStatus {
        bool active = false;
        int total = 0;       // matches found so far
        int current = -1;    // the current one among them (0-based), -1 none
        bool searching = false; // pages still to search (or the document still being read)
        bool supported = true;  // false: a document whose text is not searched (Word, Excel)
    };

    // Shrunk pictures kept per document (Entry::pictureCache).
    static constexpr std::size_t kPictureCacheBytes = std::size_t(32) << 20;

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
    // Reads document `id` again with `password`: a PDF that needs one (doc.needsPassword). The
    // password goes with the read; nothing here keeps it.
    void unlock(int id, std::string password);
    const Entry* find(int id) const;
    // A picture's size: shrunk to this many pixels for each point of the page it fills (a page
    // seen at about twice its size on a high-density screen), and never more than kMaxPictureSide.
    static constexpr float kPicturePixelsPerPoint = 3.0f;
    static constexpr int kMaxPictureSide = 2048;

    // The view shows pages [first, last], `current` the one being read: those and kPagesAround on
    // each side are read (content, then pictures), nearest `current` first; reads further away are
    // cancelled, and pages more than kPagesKept away dropped (their content and images). `request`
    // false (the content flying past): nothing new is asked for, only what is not wanted cancelled.
    // `view`: which view shows them (each keeps its own place; a page near any of them is kept).
    void showPages(int id, std::size_t first, std::size_t last, std::size_t current, bool request = true, const void* view = nullptr);
    // The view no longer shows document `id`: the pages near it are not kept for it any more.
    void hidePages(int id, const void* view);
    // The picture `key` of a PDF page is still being read (or not asked for yet): the view draws a
    // placeholder and does not take its bytes.
    bool pictureWaiting(int id, const std::string& key) const;
    // The picture `key` was read and cannot be shown (the view draws it crossed out).
    bool pictureFailed(int id, const std::string& key) const;
    // Its bytes are here, not taken by the view yet.
    bool pictureBytesHere(int id, const std::string& key) const;
    // The view has no picture for `key`: it was read, but its bytes went before the view took them
    // (another page that shows it took them and let go). It is forgotten, so it is read again.
    void pictureLost(int id, const std::string& key);
    // An image's encoded bytes, moved out (the view decodes them once; a page read again puts them
    // back). Empty when there are none.
    std::string takeMedia(int id, const std::string& key);
    // Searches document `id` (PDF, slides) for `query`; the search of any other document ends. id
    // -1 or an empty query: no search. A document not read yet is searched once it is.
    void search(int id, const std::string& query, bool matchCase);
    // The page the reader is on in document `id` (the view with the keyboard): a search reads from
    // there on, and a move without a current match starts there.
    void searchFrom(int id, std::size_t page);
    // The next (+1) or previous (-1) match becomes the current one, wrapping round; when pages on
    // the way are not searched yet, once they are. False when there is none (all searched).
    bool searchNext(int id, int direction);
    SearchStatus searchStatus(int id) const;

    // Everything about document `id` goes: its model, its images' bytes, its reads in flight.
    void release(int id);
    void releaseAll();

    std::size_t entryCount() const { return entries_.size(); }
    std::size_t loadingCount() const; // documents, pages and pictures being read

private:
    void read(Entry& e, std::string password);
    void readPage(Entry& e, std::size_t index);
    void readPictures(Entry& e, std::size_t index);
    static void cachePicture(Entry& e, const std::string& key, const std::string& bytes);
    void dropPage(Entry& e, std::size_t index);
    void applySearch(Entry& e);  // starts, changes or ends e's search to match the one wanted
    void stopSearch(Entry& e);   // its texts go, its read is cancelled
    void continueSearch(Entry& e); // texts of loaded pages taken, the next page's asked for
    void pageText(Entry& e, std::size_t index, std::string text); // a page's text is here
    bool stepSearch(Entry& e, int direction);

    interfaces::IDocumentReaderService* reader_;
    std::function<void()> requestTrim_;
    std::map<int, Entry> entries_;
    std::uint64_t nextGeneration_ = 1;
    // The search wanted (search()): document id (-1 none), query and case.
    int searchId_ = -1;
    std::string searchQuery_;
    bool searchCase_ = false;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true); // callbacks hold a weak_ptr
};

} // namespace enp::viewmodel
