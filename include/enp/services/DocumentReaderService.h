// IDocumentReaderService on one background thread kept for the service's lifetime
// (os::dispatch::Worker: the same thread and malloc arena every time, docs/MEMORY.md rule 10).
// Requests wait in the service's own list, not the worker's queue: each time the worker is free
// it takes the one nearest the page in view, so the page the reader stopped on never waits behind
// the pages scrolled past. Files are read through the file service; what is read is counted in
// the `viewer` scope.
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
    os::dispatch::Task nextTask();
    static void run(const std::shared_ptr<Shared>& shared, Ticket ticket, Request r);
    interfaces::IFileService* files_;
    std::shared_ptr<Shared> shared_;
    os::dispatch::Worker worker_; // last: destroyed first, waiting for the task running then
};

} // namespace enp::services
