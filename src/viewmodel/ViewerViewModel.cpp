#include "enp/viewmodel/ViewerViewModel.h"

#include "enp/os/Memory.h"

#include <algorithm>
#include <cmath>

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
    for (const auto& [id, e] : entries_) {
        n += (e.ticket ? 1 : 0) + e.pageTickets.size();
        for (const auto& [key, p] : e.pictures)
            n += p.ticket ? 1 : 0;
    }
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
    e.pageTickets[index] = reader_->readPage(pd.source, index, [this, alive, id, generation, index](mv::Page page) {
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
        target.clips = std::move(page.clips);
        target.scripts = page.scripts;
        target.loaded = true;
        // Its pictures are asked for by showPages (while the page is still near the view).
    });
}

void ViewerViewModel::readPictures(Entry& e, std::size_t index)
{
    mv::PageDocument& pd = e.doc.pages;
    if (!reader_ || !pd.source || index >= pd.pages.size() || !pd.pages[index].loaded)
        return;
    const mv::Page& page = pd.pages[index];
    for (const mv::ImageItem& img : page.images) {
        if (e.pictures.count(img.media) || pd.media.count(img.media))
            continue;
        // The largest the page shows it (a cropped picture shows a part: the whole is larger).
        float side = 0.0f;
        for (const mv::ImageItem& other : page.images)
            if (other.media == img.media)
                side = std::max(side, std::max(other.box.w / std::max(0.01f, other.crop.w), other.box.h / std::max(0.01f, other.crop.h)));
        const int maxSide = std::clamp(int(std::ceil(side * kPicturePixelsPerPoint)), 16, kMaxPictureSide);
        const std::weak_ptr<bool> alive = alive_;
        const int id = e.id;
        const std::uint64_t generation = e.generation;
        os::mem::Scope scope("viewer");
        Entry::Picture& picture = e.pictures[img.media];
        picture.page = index;
        picture.ticket = reader_->readImage(pd.source, index, img.media, maxSide, [this, alive, id, generation, key = img.media](std::string bytes) {
            if (alive.expired())
                return;
            const auto it = entries_.find(id);
            if (it == entries_.end() || it->second.generation != generation)
                return;
            const auto p = it->second.pictures.find(key);
            if (p == it->second.pictures.end())
                return; // forgotten (its page dropped): the service drops such results, but be sure
            os::mem::Scope scope("viewer");
            p->second.ticket = 0;
            p->second.done = true;
            if (!bytes.empty())
                it->second.doc.pages.media.emplace(key, std::move(bytes));
        });
    }
}

bool ViewerViewModel::pictureWaiting(int id, const std::string& key) const
{
    const auto it = entries_.find(id);
    if (it == entries_.end())
        return false;
    const Entry& e = it->second;
    if (e.doc.pages.media.count(key))
        return false;
    const auto p = e.pictures.find(key);
    return p == e.pictures.end() || !p->second.done;
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
    // Its pictures go with it: being read (cancelled), or read and not taken by the view yet
    // (shared ones too: a page read again asks for them again).
    for (const mv::ImageItem& img : pd.pages[index].images) {
        if (const auto m = pd.media.find(img.media); m != pd.media.end())
            pd.media.erase(m);
        if (const auto p = e.pictures.find(img.media); p != e.pictures.end()) {
            if (p->second.ticket)
                reader_->cancel(p->second.ticket);
            e.pictures.erase(p);
        }
    }
    pd.unload(index);
}

void ViewerViewModel::showPages(int id, std::size_t first, std::size_t last, std::size_t current, bool request)
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
    current = std::clamp(current, first, last);
    const std::size_t from = first >= kPagesAround ? first - kPagesAround : 0;
    const std::size_t to = std::min(n - 1, last + kPagesAround);
    reader_->focus(e.doc.pages.source.get(), current); // the service serves the nearest first
    // Reads no longer near the view: cancelled (a page far away is dropped below as well).
    for (auto it = e.pageTickets.begin(); it != e.pageTickets.end();) {
        if (it->first >= from && it->first <= to) {
            ++it;
            continue;
        }
        reader_->cancel(it->second);
        it = e.pageTickets.erase(it);
    }
    for (auto it = e.pictures.begin(); it != e.pictures.end();) {
        if (!it->second.ticket || (it->second.page >= from && it->second.page <= to)) {
            ++it;
            continue;
        }
        reader_->cancel(it->second.ticket);
        it = e.pictures.erase(it);
    }
    if (request)
        for (std::size_t i = from; i <= to; ++i) {
            readPage(e, i);
            readPictures(e, i);
        }
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
    std::map<std::string, std::string>& media = d.shape == mv::Shape::Flow ? d.flow.media : d.shape == mv::Shape::Sheet ? d.book.media : d.pages.media;
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
        for (const auto& [key, picture] : e.pictures)
            if (picture.ticket)
                reader_->cancel(picture.ticket);
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
