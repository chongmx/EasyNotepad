#include "enp/view/MarkdownImages.h"

#include "enp/os/Memory.h"
#include "enp/services/ImageDecoder.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cstring>

namespace enp::view {

MarkdownImages::~MarkdownImages()
{
    // Shutting down: the backend destroys every registered texture with the context; what we
    // own goes now.
    if (ImGui::GetCurrentContext()) {
        for (auto& [key, entry] : images_)
            if (entry.texture)
                ImGui::UnregisterUserTexture(entry.texture.get());
        for (auto& tex : graveyard_)
            ImGui::UnregisterUserTexture(tex.get());
    }
}

const MarkdownImage* MarkdownImages::acquire(const std::string& key, const std::string& bytes)
{
    os::mem::Scope scope("images");
    auto it = images_.find(key);
    if (it != images_.end()) {
        ++it->second.refs;
        return &it->second.image;
    }
    Entry& entry = images_[key];
    entry.refs = 1;
    services::DecodedImage decoded;
    std::string note;
    if (!services::decodeImage(bytes, decoded, note, kMaxSide)) {
        entry.image.failed = true;
        entry.image.note = note;
        return &entry.image;
    }
    std::vector<unsigned char>& rgba = decoded.rgba;
    const int w = decoded.width, h = decoded.height;
    entry.image.sourceWidth = decoded.sourceWidth;
    entry.image.sourceHeight = decoded.sourceHeight;
    entry.image.format = decoded.format;
    if (pixelBytes_ + rgba.size() > kBudgetBytes) {
        entry.image.failed = true;
        entry.image.note = "image budget of " + std::to_string(kBudgetBytes >> 20) + " MB reached";
        return &entry.image;
    }
    entry.texture = std::make_unique<ImTextureData>();
    entry.texture->Create(ImTextureFormat_RGBA32, w, h);
    std::memcpy(entry.texture->GetPixels(), rgba.data(), rgba.size());
    entry.texture->Status = ImTextureStatus_WantCreate;
    if (ImGui::GetCurrentContext())
        ImGui::RegisterUserTexture(entry.texture.get());
    pixelBytes_ += rgba.size();
    entry.image.texture = entry.texture.get();
    entry.image.width = w;
    entry.image.height = h;
    return &entry.image;
}

const MarkdownImage* MarkdownImages::acquireUnavailable(const std::string& key, const std::string& note)
{
    auto it = images_.find(key);
    if (it != images_.end()) {
        ++it->second.refs;
        return &it->second.image;
    }
    Entry& entry = images_[key];
    entry.refs = 1;
    entry.image.failed = true;
    entry.image.note = note;
    return &entry.image;
}

void MarkdownImages::release(const std::string& key)
{
    auto it = images_.find(key);
    if (it == images_.end())
        return;
    if (--it->second.refs > 0)
        return;
    if (it->second.texture) {
        pixelBytes_ -= size_t(it->second.image.width) * size_t(it->second.image.height) * 4;
        // The backend destroys it next frame (ImGui turns the flag into WantDestroy); the object
        // is freed in collect() once it reports Destroyed.
        it->second.texture->WantDestroyNextFrame = true;
        graveyard_.push_back(std::move(it->second.texture));
    }
    images_.erase(it);
}

void MarkdownImages::collect()
{
    if (!ImGui::GetCurrentContext())
        return;
    for (auto it = graveyard_.begin(); it != graveyard_.end();) {
        ImTextureData* tex = it->get();
        if (tex->Status == ImTextureStatus_Destroyed || tex->RefCount == 0) {
            if (tex->RefCount > 0)
                ImGui::UnregisterUserTexture(tex);
            it = graveyard_.erase(it);
        } else {
            ++it;
        }
    }
    // A texture the backend dropped (a lost context) comes back from the pixels we kept.
    for (auto& [key, entry] : images_)
        if (entry.texture && entry.texture->Status == ImTextureStatus_Destroyed && entry.texture->Pixels)
            entry.texture->Status = ImTextureStatus_WantCreate;
}

} // namespace enp::view
