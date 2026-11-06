#include "enp/viewmodel/ViewerViewModel.h"

#include "enp/os/Memory.h"

#include <algorithm>

namespace enp::viewmodel {

namespace mv = model::viewer;

ViewerViewModel::ViewerViewModel(interfaces::IDocumentReaderService* reader, std::function<void()> requestTrim)
    : reader_(reader), requestTrim_(std::move(requestTrim))
{
}

ViewerViewModel::~ViewerViewModel()
{
    // The reads in flight are cancelled; no trim request: the owner is being destroyed.
    requestTrim_ = nullptr;
    releaseAll();
}

void ViewerViewModel::open(int id, const std::string& absolutePath)
{
    if (entries_.count(id))
        return;
    os::mem::Scope scope("viewer");
    Entry& e = entries_[id];
    e.id = id;
    e.path = absolutePath;
    e.generation = nextGeneration_++;
    if (!reader_) {
        e.state = State::Failed;
        e.doc.error = "no document reader in this build";
        return;
    }
    const std::weak_ptr<bool> alive = alive_;
    const std::uint64_t generation = e.generation;
    e.ticket = reader_->read(absolutePath, [this, alive, id, generation](mv::ViewerDocument doc) {
        if (alive.expired())
            return;
        const auto it = entries_.find(id);
        if (it == entries_.end() || it->second.generation != generation)
            return; // closed, or opened again since: this result is not wanted
        os::mem::Scope scope("viewer");
        Entry& entry = it->second;
        entry.ticket = 0;
        entry.doc = std::move(doc);
        entry.state = entry.doc.shape == mv::Shape::None ? State::Failed : State::Ready;
    });
}

const ViewerViewModel::Entry* ViewerViewModel::find(int id) const
{
    const auto it = entries_.find(id);
    return it == entries_.end() ? nullptr : &it->second;
}

std::size_t ViewerViewModel::loadingCount() const
{
    std::size_t n = 0;
    for (const auto& [id, e] : entries_)
        n += (e.ticket ? 1 : 0) + e.pageTickets.size();
    return n;
}

void ViewerViewModel::readPage(Entry& e, std::size_t index)
{
    mv::PageDocument& pd = e.doc.pages;
    if (!reader_ || !pd.source || index >= pd.pages.size() || pd.pages[index].loaded || e.pageTickets.count(index))
        return;
    const std::weak_ptr<bool> alive = alive_;
    const int id = e.id;
    const std::uint64_t generation = e.generation;
    os::mem::Scope scope("viewer");
    e.pageTickets[index] = reader_->readPage(pd.source, index, [this, alive, id, generation, index](mv::Page page,
                                                                                                   std::map<std::string, std::string> media) {
        if (alive.expired())
            return;
        const auto it = entries_.find(id);
        if (it == entries_.end() || it->second.generation != generation)
            return;
        Entry& entry = it->second;
        entry.pageTickets.erase(index);
        mv::PageDocument& doc = entry.doc.pages;
        if (index >= doc.pages.size())
            return;
        os::mem::Scope scope("viewer");
        mv::Page& target = doc.pages[index];
        // The page's size and title stay; its content arrives.
        target.items = std::move(page.items);
        target.paths = std::move(page.paths);
        target.glyphs = std::move(page.glyphs);
        target.boxes = std::move(page.boxes);
        target.images = std::move(page.images);
        target.loaded = true;
        for (auto& [key, bytes] : media)
            doc.media.emplace(key, std::move(bytes)); // an image already waiting keeps its bytes
    });
}

void ViewerViewModel::dropPage(Entry& e, std::size_t index)
{
    mv::PageDocument& pd = e.doc.pages;
    if (const auto t = e.pageTickets.find(index); t != e.pageTickets.end()) {
        reader_->cancel(t->second);
        e.pageTickets.erase(t);
    }
    if (index >= pd.pages.size() || !pd.pages[index].loaded)
        return;
    // Its images' bytes not decoded yet go with it (shared ones too: a page read again brings them back).
    for (const mv::ImageItem& img : pd.pages[index].images)
        if (const auto m = pd.media.find(img.media); m != pd.media.end())
            pd.media.erase(m);
    pd.unload(index);
}

void ViewerViewModel::showPages(int id, std::size_t first, std::size_t last)
{
    const auto it = entries_.find(id);
    if (it == entries_.end() || it->second.state != State::Ready || !it->second.doc.pages.source)
        return;
    Entry& e = it->second;
    const std::size_t n = e.doc.pages.pages.size();
    if (n == 0)
        return;
    last = std::min(last, n - 1);
    first = std::min(first, last);
    // The pages in view first, then their neighbours (requests are served in order).
    for (std::size_t i = first; i <= last; ++i)
        readPage(e, i);
    if (first >= kPagesAround)
        for (std::size_t i = first - kPagesAround; i < first; ++i)
            readPage(e, i);
    for (std::size_t i = last + 1; i <= std::min(n - 1, last + kPagesAround); ++i)
        readPage(e, i);
    for (std::size_t i = 0; i < n; ++i) {
        const bool far = i + kPagesKept < first || i > last + kPagesKept;
        if (far && (e.doc.pages.pages[i].loaded || e.pageTickets.count(i)))
            dropPage(e, i);
    }
}

std::string ViewerViewModel::takeMedia(int id, const std::string& key)
{
    const auto it = entries_.find(id);
    if (it == entries_.end())
        return {};
    mv::ViewerDocument& d = it->second.doc;
    std::map<std::string, std::string>& media = d.shape == mv::Shape::Flow ? d.flow.media : d.pages.media;
    const auto m = media.find(key);
    if (m == media.end())
        return {};
    std::string bytes = std::move(m->second);
    media.erase(m);
    return bytes;
}

void ViewerViewModel::release(int id)
{
    const auto it = entries_.find(id);
    if (it == entries_.end())
        return;
    Entry& e = it->second;
    if (reader_) {
        if (e.ticket)
            reader_->cancel(e.ticket);
        for (const auto& [page, ticket] : e.pageTickets)
            reader_->cancel(ticket);
    }
    entries_.erase(it);
    if (requestTrim_)
        requestTrim_(); // a document's model can be large: the pages go back to the system
}

void ViewerViewModel::releaseAll()
{
    while (!entries_.empty())
        release(entries_.begin()->first);
}

} // namespace enp::viewmodel
