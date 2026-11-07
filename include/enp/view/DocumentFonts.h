// The fonts documents read for viewing are set in (Calibri, Cambria, Arial, Times New Roman,
// Courier New, ...), loaded when first drawn: the family itself when the system has it (Windows,
// macOS, Office), else a bundled font with the same metrics (resources/fonts/document-fonts.zip:
// Carlito, Caladea and Liberation, subset and renamed), so lines break where the author's do.
#pragma once

#include "enp/os/Fs.h"

#include "imgui.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace enp::view {

class DocumentFonts {
public:
    // A face of a family: `bold` / `italic` say whether the face itself is (else the caller fakes
    // them: a second copy, a slant). `font` null: no face for it (the caller keeps its own font).
    struct Face {
        ImFont* font = nullptr;
        bool bold = false, italic = false;
        float lineHeight = 1.17f; // single line spacing, in ems (the font's ascender, descender and line gap, as Word takes them)
    };

    DocumentFonts() = default;
    ~DocumentFonts(); // removes its fonts from the atlas (while the ImGui context is still there)
    DocumentFonts(const DocumentFonts&) = delete;
    DocumentFonts& operator=(const DocumentFonts&) = delete;

    // The face drawing `family` (as a document names it) in that weight and slant, loaded the first
    // time it is asked for (the atlas takes new fonts during a frame).
    Face face(std::string_view family, bool bold, bool italic);

    // Between frames: removes the faces not drawn for `idleSeconds` (0: not drawn this frame), and
    // gives back their files. Called each frame with a long delay, and with 0 after a close.
    void trim(double now, double idleSeconds);

    std::size_t loadedCount() const { return loaded_.size(); }

private:
    struct Loaded {
        std::string key;         // the source: a system path, or "pack:" + the bundled file's name
        ImFont* font = nullptr;  // in the atlas, which reads the bytes below in place
        os::fs::MappedFile file; // a system font
        std::string data;        // a bundled one, inflated from the pack
        double lastUsed = 0.0;
        float lineHeight = 1.17f;
    };
    // A family and style asked for, with what answers it: asked each frame for every run drawn, so
    // the answer is kept (by the document's own spelling: no allocation after the first time).
    struct Request {
        std::string family;
        int style = 0; // 1 bold, 2 italic
        Face face;
        Loaded* source = nullptr;
        bool stale = false; // its source was removed
    };
    Loaded* load(const std::string& key);
    std::vector<std::unique_ptr<Loaded>> loaded_;
    std::vector<Request> requests_;
    std::vector<std::string> missing_; // sources that failed to load: not tried again
};

} // namespace enp::view
