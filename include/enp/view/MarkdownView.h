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
    // `blocks` must stay alive and unchanged while `generation` is the same.
    void renderBlocks(const std::vector<model::markdown::Block>& blocks, std::uint64_t generation, float width, const Fonts& fonts,
                      const Palette& palette, float fontSize, float uiScale);

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
    int hoveredLine_ = -1;
    std::map<std::string, model::math::Node> math_; // parsed formulas by source (cleared with the text)

    float drawBlock(const model::markdown::Block& block, ImVec2 at, float width, const Style& style, bool draw);
    void styleFor(Style& st, const Fonts& fonts, const Palette& palette, float fontSize, float uiScale);
    void layoutAndDraw(const Style& st, float width); // shown_, measured when needed
};

} // namespace enp::view
