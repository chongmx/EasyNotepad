#include "enp/services/DocumentReaderService.h"

#include "enp/os/Memory.h"

#include <atomic>
#include <mutex>
#include <optional>
#include <set>

namespace enp::services {

// Every ticket is in `inFlight` from its request until it is over (delivered, dropped or never
// started), and in `cancelled` only while it is in flight too: a cancel that comes after the
// result was delivered is ignored, so neither set ever holds more than the requests running.
struct DocumentReaderService::Shared {
    std::mutex mutex;
    std::set<Ticket> inFlight;
    std::set<Ticket> cancelled;
    std::atomic<Ticket> next{1};

    Ticket start()
    {
        const Ticket t = next++;
        std::lock_guard<std::mutex> lock(mutex);
        inFlight.insert(t);
        return t;
    }
    bool isCancelled(Ticket t)
    {
        std::lock_guard<std::mutex> lock(mutex);
        return cancelled.count(t) != 0;
    }
    void cancel(Ticket t)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (inFlight.count(t))
            cancelled.insert(t);
    }
    // The request is over: its ticket is forgotten. True when it had been cancelled.
    bool finish(Ticket t)
    {
        std::lock_guard<std::mutex> lock(mutex);
        inFlight.erase(t);
        return cancelled.erase(t) != 0;
    }
    std::size_t pending()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return inFlight.size() + cancelled.size();
    }
};

DocumentReaderService::DocumentReaderService(interfaces::IFileService* files) : files_(files), shared_(std::make_shared<Shared>()) {}

DocumentReaderService::~DocumentReaderService() = default;

DocumentReaderService::Ticket DocumentReaderService::read(const std::string& path, DocumentDone done)
{
    const Ticket ticket = shared_->start();
    std::shared_ptr<Shared> shared = shared_;
    interfaces::IFileService* files = files_;
    worker_.post([shared, files, ticket, path, done = std::move(done)]() mutable {
        if (shared->isCancelled(ticket)) {
            shared->finish(ticket);
            return;
        }
        // Built here, handed over whole: the main thread moves it out (no copy of a document).
        auto result = std::make_shared<model::viewer::ViewerDocument>();
        {
            os::mem::Scope scope("viewer");
            std::string error;
            std::optional<std::string> bytes = files ? files->read(path, &error) : std::nullopt;
            if (!bytes) {
                result->error = error.empty() ? "the file cannot be read" : error;
            } else {
                *result = model::viewer::readViewerDocument(path, std::move(*bytes));
            }
        }
        os::dispatch::postToMain([shared, ticket, result, done = std::move(done)]() {
            if (shared->finish(ticket))
                return; // cancelled while it was read: the document goes with this task
            done(std::move(*result));
        });
    });
    return ticket;
}

DocumentReaderService::Ticket DocumentReaderService::readPage(std::shared_ptr<model::viewer::PageSource> source, std::size_t index,
                                                              PageDone done)
{
    const Ticket ticket = shared_->start();
    std::shared_ptr<Shared> shared = shared_;
    worker_.post([shared, source = std::move(source), index, ticket, done = std::move(done)]() mutable {
        if (shared->isCancelled(ticket) || !source) {
            shared->finish(ticket);
            return;
        }
        auto page = std::make_shared<model::viewer::Page>();
        auto media = std::make_shared<std::map<std::string, std::string>>();
        {
            os::mem::Scope scope("viewer");
            source->load(index, *page, *media);
        }
        source.reset(); // the last holder of a closed document's file lets go here, not later
        os::dispatch::postToMain([shared, ticket, page, media, done = std::move(done)]() {
            if (shared->finish(ticket))
                return;
            done(std::move(*page), std::move(*media));
        });
    });
    return ticket;
}

void DocumentReaderService::cancel(Ticket ticket)
{
    shared_->cancel(ticket);
}

std::size_t DocumentReaderService::pendingTickets() const
{
    return shared_->pending();
}

} // namespace enp::services
