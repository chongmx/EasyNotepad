// Reads documents for viewing (Word, Excel, PowerPoint, PDF: model::viewer) away from the UI
// thread: the file's bytes and the reader's work for a whole document, and a PDF's pages one by
// one when they are about to be shown. Results are handed over on the main thread.
#pragma once

#include "enp/model/viewer/Viewer.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace enp::interfaces {

class IDocumentReaderService {
public:
    using Ticket = std::uint64_t;
    using DocumentDone = std::function<void(model::viewer::ViewerDocument document)>;
    // A page's content and the images it uses (encoded, by key).
    using PageDone = std::function<void(model::viewer::Page page, std::map<std::string, std::string> media)>;

    virtual ~IDocumentReaderService() = default;

    // Reads the file at `path` (absolute) and parses it; `done` runs on the main thread with the
    // result (shape None and an error when it cannot be read), unless the ticket was cancelled.
    virtual Ticket read(const std::string& path, DocumentDone done) = 0;
    // Reads page `index` through the document's page source (requests are served in order, one
    // at a time: a source is never used by two threads).
    virtual Ticket readPage(std::shared_ptr<model::viewer::PageSource> source, std::size_t index, PageDone done) = 0;
    // The request's result is not wanted any more (its document closed): not started if it is
    // still queued, and dropped if it is already done.
    virtual void cancel(Ticket ticket) = 0;
};

} // namespace enp::interfaces
