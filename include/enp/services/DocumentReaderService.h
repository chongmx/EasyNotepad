// IDocumentReaderService on background threads kept for the service's lifetime
// (os::dispatch::Worker: the same threads and malloc arenas every time, docs/MEMORY.md rule 10):
// one for documents and pages' content, and a few for pictures (decoded and shrunk side by side),
// so a page's text never waits behind a picture. Requests wait in the service's own list, not the
// workers' queues: each time a worker is free it takes the one of its kind nearest the page in
// view, so the page the reader stopped on never waits behind the pages scrolled past. Files are
// read through the file service; what is read is counted in the `viewer` scope.
//
// Memory: a waiting request holds its document's page source (shared_ptr) and its callback; a
// cancel takes it out of the list at once, so a closed document's source goes with its last
// request, not when the worker would have reached it. The list is a map (nodes freed as requests
// end): nothing is kept once every request is over.
#pragma once

#include "enp/interfaces/IDocumentReaderService.h"
#include "enp/interfaces/IFileService.h"
#include "enp/os/Dispatch.h"

#include <memory>
#include <vector>

namespace enp::services {

class DocumentReaderService final : public interfaces::IDocumentReaderService {
public:
    explicit DocumentReaderService(interfaces::IFileService* files);
    ~DocumentReaderService() override;

    Ticket read(const std::string& path, DocumentDone done) override;
    Ticket readPage(std::shared_ptr<model::viewer::PageSource> source, std::size_t index, PageDone done) override;
    Ticket readImage(std::shared_ptr<model::viewer::PageSource> source, std::size_t page, std::string key, int maxSide,
                     ImageDone done) override;
    void focus(const model::viewer::PageSource* source, std::size_t page) override;
    void cancel(Ticket ticket) override;
    // Tickets the service still tracks (requests waiting, running or being handed over): 0 once
    // everything is over.
    std::size_t pendingTickets() const;
    // Requests waiting for the worker (tests).
    std::size_t waitingRequests() const;

private:
    struct Request; // one waiting request: what to read, and the callback for its result
    struct Shared;  // the waiting requests and the tickets, shared with the worker's tasks
    os::dispatch::Task nextTask(bool pictures);
    static void run(const std::shared_ptr<Shared>& shared, Ticket ticket, Request r);
    interfaces::IFileService* files_;
    std::shared_ptr<Shared> shared_;
    // Last: destroyed first, each waiting for the task it runs then. Content (documents, pages),
    // then the picture workers: half the processor's cores, 1 to 3.
    os::dispatch::Worker content_;
    std::vector<std::unique_ptr<os::dispatch::Worker>> pictures_;
};

} // namespace enp::services
