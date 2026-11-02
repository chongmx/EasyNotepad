#include "enp/view/EmojiCache.h"

#include "enp/os/Memory.h"

#include <algorithm>

namespace enp::view {

EmojiCache::EmojiCache(std::string pack, bool isFile) : pack_(std::move(pack), isFile) {}

bool EmojiCache::has(const std::string& key)
{
    return held_.count(key) != 0 || pack_.contains(key);
}

ImTextureData* EmojiCache::texture(const std::string& key)
{
    auto it = held_.find(key);
    if (it == held_.end()) {
        os::mem::Scope scope("emoji");
        std::optional<std::string> png = pack_.read(key);
        if (!png)
            return nullptr;
        Held held;
        held.image = images_.acquire("emoji:" + key, *png);
        it = held_.emplace(key, held).first;
    }
    it->second.lastFrame = frame_;
    it->second.lastTime = now_;
    return it->second.image ? it->second.image->texture : nullptr;
}

void EmojiCache::release(std::map<std::string, Held>::iterator it)
{
    images_.release("emoji:" + it->first);
    held_.erase(it);
}

void EmojiCache::frame(double now)
{
    ++frame_;
    now_ = now;
    images_.collect();
    for (auto it = held_.begin(); it != held_.end();) {
        auto next = std::next(it);
        if (now - it->second.lastTime > kIdleSeconds)
            release(it);
        it = next;
    }
    while (held_.size() > kMaxHeld) { // the least recently drawn first
        auto oldest = std::min_element(held_.begin(), held_.end(),
                                       [](const auto& a, const auto& b) { return a.second.lastFrame < b.second.lastFrame; });
        release(oldest);
    }
    if (held_.empty() && pack_.loaded())
        pack_.unload();
}

void EmojiCache::releaseUnused()
{
    for (auto it = held_.begin(); it != held_.end();) {
        auto next = std::next(it);
        if (it->second.lastFrame < frame_)
            release(it);
        it = next;
    }
    if (held_.empty() && pack_.loaded())
        pack_.unload();
}

} // namespace enp::view
