// Font loading (bundled TTFs read through the OS layer, Font Awesome icons merged in).
#pragma once

#include "enp/os/Fs.h"

#include "imgui.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace enp::view {

class DocumentFonts;

// True when ImGui's font loader (stb_truetype) can read the font at `data`: the tables it needs in
// the file's first face, with TrueType (glyf) or CFF outlines. ImGui stops the app (an assertion)
// on a font it cannot read, so every font is checked before it is added; one that fails is left
// out (another candidate, or the UI font's '?' for its letters). A variable font with CFF2
// outlines is one it cannot read (Android 15 and later: Samsung's NotoSansCJK-Regular.ttc).
bool fontReadable(const void* data, std::size_t size);

struct Fonts {
    ImFont* ui = nullptr;   // proportional UI text + icons
    ImFont* mono = nullptr; // editor and diff text + icons
    bool hasIcons = false;
    // The fonts documents are set in, loaded as they are drawn (owned by the Application; null in
    // tests without one: documents then use the UI font).
    DocumentFonts* documents = nullptr;

    static constexpr float kUiSize = 15.0f;
    static constexpr float kEditorSize = 15.0f;
    static constexpr float kRibbonIconSize = 22.0f;

    // Must be called after ImGui::CreateContext() and before the first frame.
    static Fonts load();
};

// Fonts for the scripts the bundled ones lack (Chinese, Japanese, Korean: model::viewer::Script
// bits), from the system's, merged into a font while a document that uses them is shown and
// removed after: such a font is 4 to 20 MB, mapped (the file cache's pages, not the heap) only
// while it is merged. Without a system font ImGui can read, a bundled one with the common letters
// (2.5 MB, read into the `fonts` scope while merged).
class ScriptFonts {
public:
    ScriptFonts() = default;
    ~ScriptFonts(); // removes what is merged (while the ImGui context is still there)
    ScriptFonts(const ScriptFonts&) = delete;
    ScriptFonts& operator=(const ScriptFonts&) = delete;

    // Between frames (the atlas is not in use): merges into `font` a font for each script in
    // `wanted` that has none yet, and removes the ones no longer wanted.
    void update(ImFont* font, std::uint8_t wanted);
    std::uint8_t merged() const; // the scripts a merged font covers
    std::size_t mappedBytes() const;

private:
    struct Source {
        std::uint8_t scripts = 0; // what it covers; 0: this slot is free
        os::fs::MappedFile file;  // a system font file: the atlas reads it in place (it does not own it)
        std::string bytes;        // or the bundled font (an app asset, read): released with the slot
        const void* data() const { return file.size() ? file.data() : bytes.data(); }
        std::size_t size() const { return file.size() ? file.size() : bytes.size(); }
    };
    void remove(Source& source);

    ImFont* font_ = nullptr;
    Source sources_[2];        // at most one for Han and one for Hangul (often one covers both)
    std::uint8_t missing_ = 0; // scripts no installed font covers: not looked for again
};

} // namespace enp::view
