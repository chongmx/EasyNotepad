// Draws Markdown formatted: headings, emphasis, code (highlighted), lists, quotes, tables, links,
// images, HTML entities and LaTeX math (inline and display). Used for the agent's answers and for
// .md files shown rendered in the editor.
#pragma once

#include "enp/model/Markdown.h"
#include "enp/model/Math.h"
#include "enp/view/EmojiCache.h"
#include "enp/view/Fonts.h"
#include "enp/view/MarkdownImages.h"
#include "enp/view/Theme.h"

#include "imgui.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace enp::view {

class MarkdownView {
public:
    // Draws `text` at the cursor, `width` wide, and moves the cursor below it. Parses again only
    // when the text changes; blocks outside the visible area are skipped once measured.
    void render(const std::string& text, float width, const Fonts& fonts, const Palette& palette, float fontSize, float uiScale);
    // The same for blocks read elsewhere (a Word document's: model::viewer). They are not copied:
    // `blocks` must stay alive and unchanged while `generation` is the same. `fontNames`: the
    // document's font families its spans' `font` numbers name (drawn in those fonts when the
    // fonts have DocumentFonts), and the same while `generation` is.
    void renderBlocks(const std::vector<model::markdown::Block>& blocks, std::uint64_t generation, float width, const Fonts& fonts,
                      const Palette& palette, float fontSize, float uiScale, const std::vector<std::string>* fontNames = nullptr);

    // For a caller that places the blocks itself (a document's pages): measures `blocks` at
    // `width` (once per width, size and generation, as renderBlocks does), after which each
    // block's height and the gap before it (after the block before it in `blocks`) are known; then
    // draws one block where the caller puts it, on page `pageNumber` of `pageCount` (its page-number
    // fields; 0: as saved).
    void measureBlocks(const std::vector<model::markdown::Block>& blocks, std::uint64_t generation, float width, const Fonts& fonts,
                       const Palette& palette, float fontSize, float uiScale, const std::vector<std::string>* fontNames = nullptr);
    float blockHeight(std::size_t index) const { return index < heights_.size() ? heights_[index] : 0.0f; }
    float blockGap(std::size_t index) const;
    // The height of one of the measured blocks at another width (beside a floating picture).
    float measureBlockAt(std::size_t index, float width, const Fonts& fonts, const Palette& palette, float fontSize, float uiScale,
                         const std::vector<std::string>* fontNames = nullptr);
    void drawBlockAt(std::size_t index, ImVec2 at, float width, const Fonts& fonts, const Palette& palette, float fontSize, float uiScale,
                     const std::vector<std::string>* fontNames = nullptr, int pageNumber = 0, int pageCount = 0);

    // A link was clicked (its target).
    std::function<void(const std::string& url)> onLink;
    // Resolves an image's source (as written) to a decoded image; null, or a null result, shows
    // the alt text instead. Called while laying out, so it must be cheap after the first time.
    std::function<const MarkdownImage*(const std::string& source)> onImage;
    // Draws (when `draw`) or measures drawing block `index` at `at`, `width` wide; returns its height.
    // Unset: drawing blocks take no room.
    std::function<float(int index, ImVec2 at, float width, bool draw)> onDrawing;
    // Emoji pictures (shared by every view; owned by MainView). Null: emoji show as their text.
    EmojiCache* emoji = nullptr;
    // A document read for viewing (Word): its runs keep their own colours, italics are not dimmed.
    bool paper = false;
    // The source line of the block under the mouse in the last frame, or -1.
    int hoveredLine() const { return hoveredLine_; }
    // Top of the block that starts at or before `line`, relative to where the text is drawn (after
    // a render).
    float offsetOfLine(int line) const;

    struct Style; // drawing parameters (for the layout helpers)

private:
    std::string text_;
    std::vector<model::markdown::Block> blocks_; // parsed from text_ by render()
    // renderBlocks()'s blocks (the caller's), or null for blocks_. Not a pointer to blocks_: a
    // moved view would point into the old one.
    const std::vector<model::markdown::Block>* external_ = nullptr;
    std::uint64_t generation_ = 0;
    const std::vector<model::markdown::Block>& shown() const { return external_ ? *external_ : blocks_; }
    std::vector<float> tops_;   // per block, relative to the start; measured at width_
    std::vector<float> heights_;
    float width_ = -1.0f;
    float fontSize_ = 0.0f;
    bool measuredPages_ = false; // tops_ and heights_ are the page view's (floating pictures left out)
    int hoveredLine_ = -1;
    std::map<std::string, model::math::Node> math_; // parsed formulas by source (cleared with the text)

    float drawBlock(const model::markdown::Block& block, ImVec2 at, float width, const Style& style, bool draw);
    void styleFor(Style& st, const Fonts& fonts, const Palette& palette, float fontSize, float uiScale);
    void layoutAndDraw(const Style& st, float width); // shown_, measured when needed
    void measure(const Style& st, float width);       // tops_ and heights_ of shown(), when the layout changed
    void use(const std::vector<model::markdown::Block>& blocks, std::uint64_t generation); // shows the caller's blocks
};

} // namespace enp::view
