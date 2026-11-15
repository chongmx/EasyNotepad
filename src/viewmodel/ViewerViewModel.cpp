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
    read(e, std::string());
    applySearch(e); // wanted before the document was opened: it starts once the document is read
}

void ViewerViewModel::unlock(int id, std::string password)
{
    const auto it = entries_.find(id);
    if (it == entries_.end() || !reader_ || it->second.state != State::Failed || !it->second.doc.needsPassword)
        return;
    os::mem::Scope scope("viewer");
    Entry& e = it->second;
    e.doc = mv::ViewerDocument();
    e.state = State::Loading;
    e.generation = nextGeneration_++;
    read(e, std::move(password));
}

void ViewerViewModel::read(Entry& e, std::string password)
{
    const std::weak_ptr<bool> alive = alive_;
    const int id = e.id;
    const std::uint64_t generation = e.generation;
    const bool typed = !password.empty();
    e.passwordRefused = false;
    e.ticket = reader_->read(e.path, std::move(password), [this, alive, id, generation, typed](mv::ViewerDocument doc) {
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
        entry.passwordRefused = typed && entry.doc.needsPassword;
        if (entry.search.active) { // wanted while it was read: its pages are searched now
            const int step = entry.search.step; // (a move asked for meanwhile goes on)
            stopSearch(entry);
            applySearch(entry);
            entry.search.step = step;
            continueSearch(entry);
        }
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
        n += (e.ticket ? 1 : 0) + e.pageTickets.size() + (e.search.ticket ? 1 : 0);
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
        Entry::Search& s = entry.search;
        if (s.active && index < s.counts.size() && s.counts[index] < 0) { // searched now, not read again for it
            pageText(entry, index, mv::pageText(target));
            continueSearch(entry); // a move waiting for it goes on (and the search, when it had stopped)
        }
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
        if (const auto cached = e.pictureCache.find(img.media); cached != e.pictureCache.end()) { // read before: at once
            os::mem::Scope scope("viewer");
            cached->second.used = ++e.pictureClock;
            pd.media.emplace(img.media, cached->second.bytes);
            Entry::Picture& picture = e.pictures[img.media];
            picture.page = index;
            picture.done = true;
            continue;
        }
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
            p->second.failed = bytes.empty();
            if (!bytes.empty()) {
                cachePicture(it->second, key, bytes);
                it->second.doc.pages.media.emplace(key, std::move(bytes));
            }
        });
    }
}

void ViewerViewModel::cachePicture(Entry& e, const std::string& key, const std::string& bytes)
{
    if (bytes.size() > kPictureCacheBytes / 2 || e.pictureCache.count(key))
        return;
    os::mem::Scope scope("viewer");
    Entry::Cached& c = e.pictureCache[key];
    c.bytes = bytes;
    c.used = ++e.pictureClock;
    e.pictureCacheBytes += bytes.size();
    while (e.pictureCacheBytes > kPictureCacheBytes) { // the least recently used go
        auto oldest = e.pictureCache.begin();
        for (auto it = e.pictureCache.begin(); it != e.pictureCache.end(); ++it)
            if (it->second.used < oldest->second.used)
                oldest = it;
        e.pictureCacheBytes -= oldest->second.bytes.size();
        e.pictureCache.erase(oldest);
    }
}

bool ViewerViewModel::pictureFailed(int id, const std::string& key) const
{
    const auto it = entries_.find(id);
    if (it == entries_.end())
        return false;
    const auto p = it->second.pictures.find(key);
    return p != it->second.pictures.end() && p->second.failed;
}

bool ViewerViewModel::pictureBytesHere(int id, const std::string& key) const
{
    const auto it = entries_.find(id);
    return it != entries_.end() && it->second.doc.pages.media.count(key) != 0;
}

void ViewerViewModel::pictureLost(int id, const std::string& key)
{
    const auto it = entries_.find(id);
    if (it == entries_.end())
        return;
    const auto p = it->second.pictures.find(key);
    if (p != it->second.pictures.end() && p->second.done && !p->second.failed)
        it->second.pictures.erase(p);
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

void ViewerViewModel::hidePages(int id, const void* view)
{
    if (const auto it = entries_.find(id); it != entries_.end())
        it->second.shown.erase(view);
}

void ViewerViewModel::showPages(int id, std::size_t first, std::size_t last, std::size_t current, bool request, const void* view)
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
    const bool known = e.shown.count(view) != 0;
    if (!known) {
        os::mem::Scope scope("viewer");
        e.shown[view];
    }
    Entry::Shown& v = e.shown[view];
    if (known && current != v.current)
        v.direction = current > v.current ? 1 : -1;
    v.current = current;
    v.first = first;
    v.last = last;
    // The pages in view, kPagesAround on each side, and one more the way the reader goes (read
    // last: the service serves the nearest first).
    v.from = first >= kPagesAround + (v.direction < 0 ? 1 : 0) ? first - kPagesAround - (v.direction < 0 ? 1 : 0) : 0;
    v.to = std::min(n - 1, last + kPagesAround + (v.direction > 0 ? 1 : 0));
    reader_->focus(e.doc.pages.source.get(), current); // the service serves the nearest first
    auto read = [&e](std::size_t page) { // near what a view shows: read
        for (const auto& [key, s] : e.shown)
            if (page >= s.from && page <= s.to)
                return true;
        return false;
    };
    auto keep = [&e](std::size_t page) { // not far from what a view shows: kept
        for (const auto& [key, s] : e.shown)
            if (page + kPagesKept >= s.first && page <= s.last + kPagesKept)
                return true;
        return false;
    };
    // Reads no longer near a view: cancelled (a page far away is dropped below as well).
    for (auto it = e.pageTickets.begin(); it != e.pageTickets.end();) {
        if (read(it->first)) {
            ++it;
            continue;
        }
        reader_->cancel(it->second);
        it = e.pageTickets.erase(it);
    }
    for (auto it = e.pictures.begin(); it != e.pictures.end();) {
        if (!it->second.ticket || read(it->second.page)) {
            ++it;
            continue;
        }
        reader_->cancel(it->second.ticket);
        it = e.pictures.erase(it);
    }
    if (request)
        for (std::size_t i = v.from; i <= v.to; ++i) {
            readPage(e, i);
            readPictures(e, i);
        }
    for (std::size_t i = 0; i < n; ++i)
        if (!keep(i) && (e.doc.pages.pages[i].loaded || e.pageTickets.count(i)))
            dropPage(e, i);
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

// ---- Search ---------------------------------------------------------------------------------------

void ViewerViewModel::search(int id, const std::string& query, bool matchCase)
{
    if (query.empty())
        id = -1;
    if (searchId_ == id && searchQuery_ == query && searchCase_ == matchCase)
        return;
    searchId_ = id;
    if (id < 0) {
        os::mem::release(searchQuery_);
    } else {
        os::mem::Scope scope("viewer");
        searchQuery_ = query;
    }
    searchCase_ = matchCase;
    for (auto& [entryId, e] : entries_)
        applySearch(e);
}

void ViewerViewModel::applySearch(Entry& e)
{
    Entry::Search& s = e.search;
    if (searchId_ != e.id) {
        if (s.active)
            stopSearch(e);
        return;
    }
    if (s.active && s.query == searchQuery_ && s.matchCase == searchCase_)
        return;
    os::mem::Scope scope("viewer");
    s.active = true;
    s.query = searchQuery_;
    s.matchCase = searchCase_;
    s.page = -1;
    s.ordinal = 0;
    s.step = 0;
    s.first = -1;
    if (e.state != State::Ready || e.doc.shape != mv::Shape::Pages)
        return; // being read: searched once it is (read()); not pages: nothing to search
    const std::size_t n = e.doc.pages.pages.size();
    if (s.counts.size() != n) {
        s.texts.resize(n);
        s.counts.assign(n, -1);
        s.searched = 0;
    }
    s.total = 0;
    for (std::size_t i = 0; i < n; ++i) // the texts read already: counted again at once
        if (s.counts[i] >= 0) {
            s.counts[i] = int(mv::findText(s.texts[i], s.query, s.matchCase).size());
            s.total += s.counts[i];
        }
    continueSearch(e);
}

void ViewerViewModel::stopSearch(Entry& e)
{
    Entry::Search& s = e.search;
    if (s.ticket && reader_)
        reader_->cancel(s.ticket);
    const std::uint64_t reveal = s.reveal; // goes on counting: a view compares it with the last it saw
    const bool large = s.texts.size() > 16;
    s = Entry::Search(); // (moved in: the texts' memory goes now)
    s.reveal = reveal;
    if (large && requestTrim_)
        requestTrim_();
}

void ViewerViewModel::pageText(Entry& e, std::size_t index, std::string text)
{
    Entry::Search& s = e.search;
    if (index >= s.counts.size() || s.counts[index] >= 0)
        return;
    os::mem::Scope scope("viewer");
    s.counts[index] = int(mv::findText(text, s.query, s.matchCase).size());
    s.total += s.counts[index];
    s.texts[index] = std::move(text);
    ++s.searched;
    if (s.first == int(index))
        s.first = -1;
}

void ViewerViewModel::continueSearch(Entry& e)
{
    Entry::Search& s = e.search;
    if (!s.active || e.state != State::Ready || e.doc.shape != mv::Shape::Pages)
        return;
    const mv::PageDocument& pd = e.doc.pages;
    const std::size_t n = std::min(pd.pages.size(), s.counts.size());
    for (std::size_t i = 0; i < n; ++i) // the pages read already: their text at once
        if (s.counts[i] < 0 && pd.pages[i].loaded)
            pageText(e, i, mv::pageText(pd.pages[i]));
    if (s.step)
        stepSearch(e, s.step);
    if (s.ticket || s.searched >= n)
        return; // one text read at a time
    // The next page not searched: the one a move waits for, else from the reader's page on.
    std::size_t next = n;
    if (s.first >= 0 && std::size_t(s.first) < n && s.counts[std::size_t(s.first)] < 0)
        next = std::size_t(s.first);
    for (std::size_t k = 0; k < n && next == n; ++k)
        if (const std::size_t i = (std::min(s.from, n - 1) + k) % n; s.counts[i] < 0)
            next = i;
    if (next == n)
        return;
    if (!reader_ || !pd.source) { // nothing to read it with: no text
        pageText(e, next, std::string());
        continueSearch(e);
        return;
    }
    const std::weak_ptr<bool> alive = alive_;
    const int id = e.id;
    const std::uint64_t generation = e.generation;
    os::mem::Scope scope("viewer");
    s.ticket = reader_->readText(pd.source, next, [this, alive, id, generation, next](std::string text) {
        if (alive.expired())
            return;
        const auto it = entries_.find(id);
        if (it == entries_.end() || it->second.generation != generation)
            return;
        Entry& entry = it->second;
        entry.search.ticket = 0;
        if (!entry.search.active)
            return;
        pageText(entry, next, std::move(text));
        continueSearch(entry);
    });
}

bool ViewerViewModel::stepSearch(Entry& e, int direction)
{
    Entry::Search& s = e.search;
    const int n = int(s.counts.size());
    s.step = 0;
    if (!s.active)
        return false;
    if (n == 0) { // the document is still being read
        s.step = direction;
        return false;
    }
    auto choose = [&s](int page, int ordinal) {
        s.page = page;
        s.ordinal = ordinal;
        s.first = -1;
        ++s.reveal;
        return true;
    };
    int start = 0, from = 0, steps = n;
    if (s.page >= 0 && s.page < n) {
        if (direction > 0 && s.ordinal + 1 < s.counts[size_t(s.page)])
            return choose(s.page, s.ordinal + 1);
        if (direction < 0 && s.ordinal > 0 && s.ordinal - 1 < s.counts[size_t(s.page)])
            return choose(s.page, s.ordinal - 1);
        start = s.page;
        from = 1; // the pages after it, round to itself again
        steps = n + 1;
    } else {
        start = int(std::min<std::size_t>(s.from, std::size_t(n - 1)));
    }
    for (int k = from; k < steps; ++k) {
        const int q = ((start + direction * k) % n + n) % n;
        const int count = s.counts[size_t(q)];
        if (count < 0) { // not searched yet: the move waits for it, read first
            s.step = direction;
            s.first = q;
            return false;
        }
        if (count > 0)
            return choose(q, direction > 0 ? 0 : count - 1);
    }
    return false;
}

void ViewerViewModel::searchFrom(int id, std::size_t page)
{
    if (const auto it = entries_.find(id); it != entries_.end())
        it->second.search.from = page;
}

bool ViewerViewModel::searchNext(int id, int direction)
{
    const auto it = entries_.find(id);
    if (it == entries_.end() || !it->second.search.active)
        return false;
    Entry& e = it->second;
    const bool found = stepSearch(e, direction);
    if (!found && e.search.step)
        continueSearch(e); // the page it waits for is read next
    return found;
}

ViewerViewModel::SearchStatus ViewerViewModel::searchStatus(int id) const
{
    SearchStatus status;
    if (searchId_ != id)
        return status;
    status.active = true;
    const auto it = entries_.find(id);
    if (it == entries_.end() || it->second.state == State::Loading) {
        status.searching = true;
        return status;
    }
    const Entry& e = it->second;
    if (e.state == State::Failed)
        return status;
    if (e.doc.shape != mv::Shape::Pages) {
        status.supported = false;
        return status;
    }
    const Entry::Search& s = e.search;
    status.total = s.total;
    status.searching = s.searched < s.counts.size();
    if (s.page >= 0 && std::size_t(s.page) < s.counts.size()) {
        int before = 0;
        for (int q = 0; q < s.page; ++q)
            before += std::max(0, s.counts[size_t(q)]);
        status.current = before + s.ordinal;
    }
    return status;
}

void ViewerViewModel::release(int id)
{
    const auto it = entries_.find(id);
    if (it == entries_.end())
        return;
    Entry& e = it->second;
    if (reader_) {
        if (e.search.ticket)
            reader_->cancel(e.search.ticket);
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
