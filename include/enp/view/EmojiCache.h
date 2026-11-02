// Emoji pictures for rendered Markdown (editor and agent chat), shared: decoded from the emoji pack
// (services::EmojiPack) into textures when first drawn, and let go when not drawn for a while,
// when the app trims memory after a close (releaseUnused), or when more than kMaxHeld are held.
// With nothing held, the pack's index goes too. Nothing grows with use: at most kMaxHeld textures.
#pragma once

#include "enp/services/EmojiPack.h"
#include "enp/view/MarkdownImages.h"

#include <map>
#include <string>

namespace enp::view {

class EmojiCache {
public:
    static constexpr double kIdleSeconds = 30.0; // a picture not drawn for this long is let go
    static constexpr size_t kMaxHeld = 512;      // 72x72 RGBA: about 10 MB at most

    explicit EmojiCache(std::string pack = "emoji/noto-emoji-72.zip", bool isFile = false);

    // Whether the pack has a picture for `key` (reads its index the first time; nothing decoded).
    bool has(const std::string& key);
    // The picture's texture, decoded the first time; null when there is none. Marks it in use.
    ImTextureData* texture(const std::string& key);

    // Once a frame, before drawing.
    void frame(double now);
    // Lets go of every picture not drawn in the last frame (after a document or folder closes).
    void releaseUnused();

    size_t heldCount() const { return held_.size(); }
    size_t textureCount() const { return images_.liveCount() + images_.pendingCount(); }
    bool indexLoaded() const { return pack_.loaded(); }

private:
    struct Held {
        int lastFrame = 0;
        double lastTime = 0.0;
        const MarkdownImage* image = nullptr;
    };
    void release(std::map<std::string, Held>::iterator it);

    services::EmojiPack pack_;
    MarkdownImages images_;
    std::map<std::string, Held> held_;
    int frame_ = 0;
    double now_ = 0.0;
};

} // namespace enp::view
