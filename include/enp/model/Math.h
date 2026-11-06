// A LaTeX formula read into a tree for drawing: $x^2$, \frac{a}{b}, \sqrt[n]{x}, \sum_{i=0}^{n},
// Greek letters and the common symbols, \left( \right), \text{...}, accents, and rows and columns
// (\\ and &, at the top level and in aligned, cases, the matrices and array). The subset that
// README files and notes use; what it does not know is shown as its text, never dropped.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace enp::model::math {

struct Node {
    enum class Kind {
        Row,     // children in a line
        Text,    // glyphs (a variable, a number, a symbol, \text{...})
        Frac,    // children: numerator, denominator
        Scripts, // children: base, superscript (Row, empty when absent), subscript (Row, empty when absent)
        Sqrt,    // children: radicand, index (Row, empty when absent)
        Op,      // a big operator (∑ ∫ lim): `text` is its glyph; limits come as Scripts around it
        Accent,  // children: base; `text` is the accent glyph ("^", "¯", "→", "~", "˙"); "‾" draws a line
        Space,   // `space` em of horizontal space
        Fenced,  // children: content; `text`/`text2` the left/right delimiters, stretched to it
        Table,   // children: rows (Row), each of cells; `text` the columns' alignment (l, c, r; the last repeats)
    };
    Kind kind = Kind::Row;
    std::string text;
    std::string text2;
    bool upright = false;     // Text: \mathrm, \text, function names
    bool bold = false;        // Text: \mathbf
    bool binary = false;      // Text: a binary operator or relation (space around it)
    bool relation = false;    // Text: a relation (= ≤ → ∈): a long formula breaks before one first
    bool limitsAbove = false; // Op: limits go above and below in display style (∑, ∏, lim), else beside (∫)
    bool pairs = false;       // Table: columns go in right-left pairs (align), with no gap inside a pair
    bool display = false;     // Table: cells in display style (aligned, gather), else text style (matrices, cases)
    float space = 0.0f;       // Space, in em
    std::vector<Node> children;
};

// Never fails: unbalanced braces close at the end, unknown commands become upright text.
Node parse(std::string_view latex);
// The formula as plain text ("a/b", "x^2", "∑_{i=0}^{n} i"): for tests and screen readers.
std::string plainText(const Node& node);

} // namespace enp::model::math
