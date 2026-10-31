// Line-based UTF-8 text storage with position arithmetic. Columns are byte offsets.
#pragma once

#include <cstdint>
#include <deque>
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

// One change to a buffer: at `at`, `removed` was replaced by `inserted` (line breaks as "\n").
struct TextChange {
    TextPos at;
    std::string removed;
    std::string inserted;
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

    // Bumped by every change. firstLineChangedSince(v) is the lowest line any change after
    // version v touched (lineCount() when nothing changed; 0 when v is too old to tell), so caches
    // of per-line data can keep everything above it.
    uint64_t version() const { return version_; }
    int firstLineChangedSince(uint64_t version) const;

    // Undo support: while a journal is open every change is recorded as a TextChange (what it
    // removed and inserted), so it can be replayed backwards (revert) or forwards (reapply).
    void beginJournal();
    std::vector<TextChange> endJournal();
    bool journaling() const { return journaling_; }
    void revert(const std::vector<TextChange>& changes);
    void reapply(const std::vector<TextChange>& changes);
    // Same text as `text` (line breaks "\n"), without building a copy of the buffer.
    bool equals(std::string_view text) const;
private:
    void touched(int line);
    std::vector<std::string> lines_;
    uint64_t version_ = 0;
    std::deque<std::pair<uint64_t, int>> changes_; // the last changes: version, first line touched
    bool journaling_ = false;
    std::vector<TextChange> journal_;
    void record(TextPos at, std::string removed, std::string inserted);
    static TextPos endOf(TextPos at, const std::string& text);
};

bool isUtf8Continuation(char c);
int codePointCount(std::string_view s);

} // namespace enp::model
