// IDocumentReaderService on one background thread kept for the service's lifetime
// (os::dispatch::Worker: the same thread and malloc arena every time, docs/MEMORY.md rule 10).
// Files are read through the file service; what is read is counted in the `viewer` scope.
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
    void cancel(Ticket ticket) override;
    // Tickets the service still tracks (requests running or queued): 0 once everything is over.
    std::size_t pendingTickets() const;

private:
    struct Shared; // the cancelled tickets, shared with the tasks in flight
    interfaces::IFileService* files_;
    std::shared_ptr<Shared> shared_;
    os::dispatch::Worker worker_; // last: destroyed first, waiting for the task running then
};

} // namespace enp::services
