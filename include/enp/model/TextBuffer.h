// Line-based UTF-8 text storage with position arithmetic. Columns are byte offsets.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace enp::model {

struct TextPos {
    int line = 0;
    int column = 0; // byte offset into the line

    bool operator==(const TextPos& o) const { return line == o.line && column == o.column; }
    bool operator!=(const TextPos& o) const { return !(*this == o); }
    bool operator<(const TextPos& o) const { return line < o.line || (line == o.line && column < o.column); }
    bool operator<=(const TextPos& o) const { return !(o < *this); }
};

class TextBuffer {
public:
    TextBuffer();
    explicit TextBuffer(std::string_view text);

    // Accepts "\n", "\r\n" and "\r" line endings.
    void setText(std::string_view text);
    std::string text(std::string_view eol = "\n") const;
    std::string text(TextPos from, TextPos to, std::string_view eol = "\n") const;

    int lineCount() const { return int(lines_.size()); }
    const std::string& line(int index) const { return lines_[size_t(index)]; }
    void setLine(int index, std::string text);
    void insertLine(int index, std::string text);
    void eraseLine(int index);
    // Number of characters (code points), counting each line break as `eolLength`.
    size_t characterCount(int eolLength = 1) const;

    TextPos clamp(TextPos pos) const;
    TextPos end() const;
    // Inserts text (which may contain line breaks) and returns the position after it.
    TextPos insert(TextPos at, std::string_view text);
    // Erases [from, to) and returns `from`.
    TextPos erase(TextPos from, TextPos to);

    // Previous / next code point boundary, crossing line breaks.
    TextPos prev(TextPos pos) const;
    TextPos next(TextPos pos) const;
    // Start / end of the word around `pos` (identifier characters).
    TextPos wordStart(TextPos pos) const;
    TextPos wordEnd(TextPos pos) const;

private:
    std::vector<std::string> lines_;
};

bool isUtf8Continuation(char c);
int codePointCount(std::string_view s);

} // namespace enp::model
