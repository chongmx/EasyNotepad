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

    // A link was clicked (its target).
    std::function<void(const std::string& url)> onLink;
    // Resolves an image's source (as written) to a decoded image; null, or a null result, shows
    // the alt text instead. Called while laying out, so it must be cheap after the first time.
    std::function<const MarkdownImage*(const std::string& source)> onImage;
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
    std::vector<model::markdown::Block> blocks_;
    std::vector<float> tops_;   // per block, relative to the start; measured at width_
    std::vector<float> heights_;
    float width_ = -1.0f;
    float fontSize_ = 0.0f;
    int hoveredLine_ = -1;
    std::map<std::string, model::math::Node> math_; // parsed formulas by source (cleared with the text)

    float drawBlock(const model::markdown::Block& block, ImVec2 at, float width, const Style& style, bool draw);
};

} // namespace enp::view
