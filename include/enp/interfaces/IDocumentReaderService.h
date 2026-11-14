// Reads documents for viewing (Word, Excel, PowerPoint, PDF: model::viewer) away from the UI
// thread: the file's bytes and the reader's work for a whole document, and a PDF's pages one by
// one when they are about to be shown, each in two steps: the page's content (text, outlines,
// fills: quick), then each of its pictures (decoded and shrunk: slow). Results are handed over on
// the main thread.
#pragma once

#include "enp/model/viewer/Viewer.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace enp::interfaces {

class IDocumentReaderService {
public:
    using Ticket = std::uint64_t;
    using DocumentDone = std::function<void(model::viewer::ViewerDocument document)>;
    // A page's content; its pictures are image items naming a key (readImage reads them).
    using PageDone = std::function<void(model::viewer::Page page)>;
    // A picture's bytes, ready for the view (QOI, at most the size asked); empty: it cannot be read.
    using ImageDone = std::function<void(std::string bytes)>;

    virtual ~IDocumentReaderService() = default;

    // Reads the file at `path` (absolute) and parses it; `done` runs on the main thread with the
    // result (shape None and an error when it cannot be read), unless the ticket was cancelled.
    virtual Ticket read(const std::string& path, DocumentDone done) = 0;
    // Reads page `index`'s content through the document's page source.
    virtual Ticket readPage(std::shared_ptr<model::viewer::PageSource> source, std::size_t index, PageDone done) = 0;
    // Reads the picture `key` that page `page` draws, shrunk to at most `maxSide` pixels a side.
    virtual Ticket readImage(std::shared_ptr<model::viewer::PageSource> source, std::size_t page, std::string key, int maxSide,
                             ImageDone done) = 0;
    // The page the reader is looking at in the document read through `source`. Requests are
    // served one at a time (a source is never used by two threads), the nearest first: a whole
    // document, then pages' content, then pictures, each nearest that page; another document's
    // requests after these.
    virtual void focus(const model::viewer::PageSource* source, std::size_t page) = 0;
    // The request's result is not wanted any more: forgotten at once if it is still waiting (with
    // what it holds), stopped where it can be if it is running, and dropped if it is done.
    virtual void cancel(Ticket ticket) = 0;
};

} // namespace enp::interfaces
