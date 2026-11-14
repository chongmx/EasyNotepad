#include "enp/services/DocumentReaderService.h"

#include "enp/os/Memory.h"
#include "enp/services/ImageDecoder.h"

#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <tuple>

namespace enp::services {

struct DocumentReaderService::Request {
    enum class Kind { Document, Page, Image } kind = Kind::Page;
    std::string path;                                   // Document
    interfaces::IFileService* files = nullptr;          // Document
    std::shared_ptr<model::viewer::PageSource> source;  // Page, Image
    std::size_t page = 0;                               // Page, Image
    std::string key;                                    // Image
    int maxSide = 0;                                    // Image
    interfaces::IDocumentReaderService::DocumentDone documentDone;
    interfaces::IDocumentReaderService::PageDone pageDone;
    interfaces::IDocumentReaderService::ImageDone imageDone;
};

// Every ticket is in `inFlight` from its request until it is over (delivered, dropped or never
// started), and in `cancelled` only while it is in flight but no longer waiting (running, or its
// result on the way to the main thread): neither set ever holds more than the requests alive.
struct DocumentReaderService::Shared {
    mutable std::mutex mutex;
    std::map<Ticket, Request> waiting;
    std::set<Ticket> inFlight;
    std::set<Ticket> cancelled;
    std::atomic<Ticket> next{1};
    const model::viewer::PageSource* focusSource = nullptr;
    std::size_t focusPage = 0;

    Ticket add(Request r)
    {
        const Ticket t = next++;
        std::lock_guard<std::mutex> lock(mutex);
        inFlight.insert(t);
        waiting.emplace(t, std::move(r));
        return t;
    }
    // The waiting request to run now: the nearest the page in view (see IDocumentReaderService::focus).
    std::optional<std::pair<Ticket, Request>> take()
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto best = waiting.end();
        std::tuple<int, int, int, std::size_t, Ticket> bestRank{};
        for (auto it = waiting.begin(); it != waiting.end(); ++it) {
            const Request& r = it->second;
            const bool here = r.source.get() == focusSource;
            const std::size_t distance = !here ? 0 : r.page > focusPage ? r.page - focusPage : focusPage - r.page;
            const auto rank = std::make_tuple(r.kind == Request::Kind::Document ? 0 : 1, here ? 0 : 1, r.kind == Request::Kind::Image ? 1 : 0,
                                              distance, it->first);
            if (best == waiting.end() || rank < bestRank) {
                best = it;
                bestRank = rank;
            }
        }
        if (best == waiting.end())
            return std::nullopt;
        std::pair<Ticket, Request> out(best->first, std::move(best->second));
        waiting.erase(best);
        return out;
    }
    bool isCancelled(Ticket t) const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return cancelled.count(t) != 0;
    }
    // Returns the request still waiting (forgotten now: the caller lets it go outside the lock).
    std::optional<Request> cancel(Ticket t)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (const auto it = waiting.find(t); it != waiting.end()) {
            std::optional<Request> out(std::move(it->second));
            waiting.erase(it);
            inFlight.erase(t);
            return out;
        }
        if (inFlight.count(t))
            cancelled.insert(t);
        return std::nullopt;
    }
    // The request is over: its ticket is forgotten. True when it had been cancelled.
    bool finish(Ticket t)
    {
        std::lock_guard<std::mutex> lock(mutex);
        inFlight.erase(t);
        return cancelled.erase(t) != 0;
    }
    std::size_t pending() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return inFlight.size() + cancelled.size();
    }
};

DocumentReaderService::DocumentReaderService(interfaces::IFileService* files) : files_(files), shared_(std::make_shared<Shared>()) {}

DocumentReaderService::~DocumentReaderService() = default;

// One worker task per request: it runs whichever request is the nearest then (not necessarily its
// own); a task finding the list empty (its request was cancelled) does nothing.
//
// The task holds a plain pointer to the service: the service outlives its worker (destroyed first,
// it waits for the task running and drops the queued ones), and a pointer fits inside the task, so
// a task costs no allocation. (A shared_ptr would be a heap block per request, left in the
// worker's queue after its request was cancelled, until the worker reached it.)
os::dispatch::Task DocumentReaderService::nextTask()
{
    return [self = this]() {
        std::optional<std::pair<Ticket, Request>> job = self->shared_->take();
        if (job)
            run(self->shared_, job->first, std::move(job->second));
    };
}

void DocumentReaderService::run(const std::shared_ptr<Shared>& shared, Ticket ticket, Request r)
{
    using Kind = Request::Kind;
    // A request dropped here lets go of everything it holds (its source, its callback) before its
    // ticket is over: once pendingTickets() is 0, nothing of it is left anywhere.
    auto drop = [&shared, ticket](Request& gone) {
        { Request freed = std::move(gone); }
        shared->finish(ticket);
    };
    if (shared->isCancelled(ticket)) {
        drop(r);
        return;
    }
    if (r.kind == Kind::Document) {
        // Built here, handed over whole: the main thread moves it out (no copy of a document).
        auto result = std::make_shared<model::viewer::ViewerDocument>();
        {
            os::mem::Scope scope("viewer");
            std::string error;
            std::optional<std::string> bytes = r.files ? r.files->read(r.path, &error) : std::nullopt;
            if (!bytes)
                result->error = error.empty() ? "the file cannot be read" : error;
            else
                *result = model::viewer::readViewerDocument(r.path, std::move(*bytes));
        }
        // (Moved, not copied: the worker keeps no part of a result the main thread may free first.)
        os::dispatch::postToMain([shared, ticket, result = std::move(result), done = std::move(r.documentDone)]() {
            if (shared->finish(ticket))
                return; // cancelled while it was read: the document goes with this task
            done(std::move(*result));
        });
        return;
    }
    if (r.kind == Kind::Page) {
        auto page = std::make_shared<model::viewer::Page>();
        {
            os::mem::Scope scope("viewer");
            if (r.source)
                r.source->load(r.page, *page);
            page->scripts = model::viewer::scriptsIn(page->plainText()); // as PageDocument::load does
        }
        r.source.reset(); // the last holder of a closed document's file lets go here, not later
        os::dispatch::postToMain([shared, ticket, page = std::move(page), done = std::move(r.pageDone)]() {
            if (shared->finish(ticket))
                return;
            done(std::move(*page));
        });
        return;
    }
    // A picture: read, then (unless no longer wanted by then) shrunk to the size it is shown.
    auto bytes = std::make_shared<std::string>();
    {
        os::mem::Scope scope("viewer");
        if (r.source)
            *bytes = r.source->loadImage(r.key);
        r.source.reset();
        if (shared->isCancelled(ticket)) {
            bytes.reset();
            drop(r);
            return;
        }
        if (!bytes->empty())
            fitImage(*bytes, r.maxSide);
    }
    os::dispatch::postToMain([shared, ticket, bytes = std::move(bytes), done = std::move(r.imageDone)]() {
        if (shared->finish(ticket))
            return;
        done(std::move(*bytes));
    });
}

DocumentReaderService::Ticket DocumentReaderService::read(const std::string& path, DocumentDone done)
{
    Request r;
    r.kind = Request::Kind::Document;
    r.path = path;
    r.files = files_;
    r.documentDone = std::move(done);
    const Ticket ticket = shared_->add(std::move(r));
    worker_.post(nextTask());
    return ticket;
}

DocumentReaderService::Ticket DocumentReaderService::readPage(std::shared_ptr<model::viewer::PageSource> source, std::size_t index,
                                                              PageDone done)
{
    Request r;
    r.kind = Request::Kind::Page;
    r.source = std::move(source);
    r.page = index;
    r.pageDone = std::move(done);
    const Ticket ticket = shared_->add(std::move(r));
    worker_.post(nextTask());
    return ticket;
}

DocumentReaderService::Ticket DocumentReaderService::readImage(std::shared_ptr<model::viewer::PageSource> source, std::size_t page,
                                                               std::string key, int maxSide, ImageDone done)
{
    Request r;
    r.kind = Request::Kind::Image;
    r.source = std::move(source);
    r.page = page;
    r.key = std::move(key);
    r.maxSide = maxSide;
    r.imageDone = std::move(done);
    const Ticket ticket = shared_->add(std::move(r));
    worker_.post(nextTask());
    return ticket;
}

void DocumentReaderService::focus(const model::viewer::PageSource* source, std::size_t page)
{
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->focusSource = source;
    shared_->focusPage = page;
}

void DocumentReaderService::cancel(Ticket ticket)
{
    std::optional<Request> dropped = shared_->cancel(ticket);
    // `dropped` (its source and callback) goes here, outside the lock.
}

std::size_t DocumentReaderService::pendingTickets() const
{
    return shared_->pending();
}

std::size_t DocumentReaderService::waitingRequests() const
{
    std::lock_guard<std::mutex> lock(shared_->mutex);
    return shared_->waiting.size();
}

} // namespace enp::services
