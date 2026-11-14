// Draws a fixed-layout page (model::viewer::Page: a slide or a PDF page) with Dear ImGui: paths,
// glyph runs at their PDF positions, text boxes wrapped in place, images. One painter for every
// format that has pages.
//
// Memory: nothing is allocated per frame except the point buffer below, which is reused and
// shrinks once a peak has passed (docs/MEMORY.md rule 6); text boxes are wrapped without copying
// their text.
#pragma once

#include "enp/model/viewer/Page.h"
#include "enp/view/Fonts.h"

#include "imgui.h"

#include <string>
#include <vector>

namespace enp::view {

struct MarkdownImage;

// The decoded image for a page's media key, or null (a light box is drawn instead). An interface,
// not a std::function: the caller passes one without building anything per frame.
class PageImages {
public:
    virtual ~PageImages() = default;
    virtual const MarkdownImage* image(const std::string& media) = 0;
    // The picture is still being read: its box shows an hourglass (a picture that failed shows
    // a crossed-out picture instead).
    virtual bool waiting(const std::string& media)
    {
        (void)media;
        return false;
    }
};

class PagePainter {
public:
    // Draws `page` with its top left at `origin`, `scale` pixels per point.
    void draw(const model::viewer::Page& page, ImVec2 origin, float scale, const Fonts& fonts, PageImages* images);
    // Gives back the point buffer's peak (after a document closes).
    void release();

private:
    void drawPath(ImDrawList* dl, const model::viewer::PathItem& p, ImVec2 origin, float scale);
    void drawBands(ImDrawList* dl, const model::viewer::PathItem& p, ImVec2 origin, float scale);
    void drawGlyphs(ImDrawList* dl, const model::viewer::GlyphRun& g, ImVec2 origin, float scale, const Fonts& fonts);
    void drawBox(ImDrawList* dl, const model::viewer::TextBox& b, ImVec2 origin, float scale, const Fonts& fonts);
    std::vector<ImVec2> points_;
    std::size_t peak_ = 0;          // the most points one path took this frame
    std::vector<ImDrawVert> tris_; // the corners of the triangles a gradient fill splits into smaller ones
};

} // namespace enp::view
