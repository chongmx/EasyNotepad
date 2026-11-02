// Images shown in rendered Markdown: decoded once, uploaded through Dear ImGui's texture protocol
// (the backend creates and destroys the GPU texture), shared by every document that shows them,
// and destroyed when the last one lets go. Nothing here outlives its use: a released texture is
// handed back to the backend and freed as soon as it reports it destroyed (collect(), once a
// frame). Pixels stay in memory so a lost GL context (Android) can re-create the texture.
#pragma once

#include "imgui.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace enp::view {

struct MarkdownImage {
    ImTextureData* texture = nullptr; // null while failed
    int width = 0, height = 0;        // pixels, after any downscale
    int sourceWidth = 0, sourceHeight = 0;
    std::string format; // "PNG", "QOI", ...
    bool failed = false;
    std::string note; // why it failed ("not an image", "too large")
};

class MarkdownImages {
public:
    // Images wider or taller than this are box-downscaled by an integer factor when decoded.
    static constexpr int kMaxSide = 2048;
    // Decoded pixel bytes an instance holds at most; beyond it images fail with a note.
    static constexpr std::size_t kBudgetBytes = 96u << 20;

    MarkdownImages() = default;
    ~MarkdownImages();
    MarkdownImages(const MarkdownImages&) = delete;
    MarkdownImages& operator=(const MarkdownImages&) = delete;

    // The image for `key` (an absolute path or URL), decoding `bytes` the first time
    // (services::decodeImage: PNG, JPEG, GIF, BMP, TGA, PSD, PNM, HDR, QOI, ICO/CUR). Each acquire()
    // is matched by a release(). Returns a pointer that stays valid until the last release.
    const MarkdownImage* acquire(const std::string& key, const std::string& bytes);
    // An image that cannot be shown (file not found, a remote URL): a failed entry with `note`,
    // so the renderer draws a placeholder and nothing is tried again until the document changes.
    const MarkdownImage* acquireUnavailable(const std::string& key, const std::string& note);
    void release(const std::string& key);
    // Frees textures the backend has destroyed and re-creates ones a lost context dropped. Once
    // a frame, with an ImGui context current.
    void collect();

    std::size_t liveCount() const { return images_.size(); }
    std::size_t pendingCount() const { return graveyard_.size(); }
    std::size_t pixelBytes() const { return pixelBytes_; }

private:
    struct Entry {
        MarkdownImage image;
        std::unique_ptr<ImTextureData> texture;
        int refs = 0;
    };
    std::map<std::string, Entry> images_;
    std::vector<std::unique_ptr<ImTextureData>> graveyard_; // released, waiting for the backend
    std::size_t pixelBytes_ = 0;
};

} // namespace enp::view
