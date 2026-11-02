#include "enp/model/TextBuffer.h"

#include <algorithm>
#include <cctype>

namespace enp::model {

namespace {

bool isWordChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || (static_cast<unsigned char>(c) & 0x80) != 0;
}

} // namespace

bool isUtf8Continuation(char c)
{
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

int codePointCount(std::string_view s)
{
    int count = 0;
    for (char c : s)
        if (!isUtf8Continuation(c))
            ++count;
    return count;
}

TextBuffer::TextBuffer() : lines_(1) {}

TextBuffer::TextBuffer(std::string_view text)
{
    setText(text);
}

void TextBuffer::setText(std::string_view text)
{
    touched(0);
    const std::string before = journaling_ ? this->text() : std::string();
    lines_.clear();
    std::string current;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\r' || c == '\n') {
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
                ++i;
            lines_.push_back(std::move(current));
            current.clear();
        } else {
            current += c;
        }
    }
    lines_.push_back(std::move(current));
    if (journaling_)
        record({0, 0}, before, this->text());
}

std::string TextBuffer::text(std::string_view eol) const
{
    return text(TextPos{0, 0}, end(), eol);
}

std::string TextBuffer::text(TextPos from, TextPos to, std::string_view eol) const
{
    from = clamp(from);
    to = clamp(to);
    if (to < from)
        std::swap(from, to);
    if (from.line == to.line)
        return lines_[size_t(from.line)].substr(size_t(from.column), size_t(to.column - from.column));
    // The exact size first: one allocation, not one per doubling as the text grows.
    size_t size = lines_[size_t(from.line)].size() - size_t(from.column) + size_t(to.column);
    for (int l = from.line + 1; l < to.line; ++l)
        size += lines_[size_t(l)].size();
    size += eol.size() * size_t(to.line - from.line);
    std::string out;
    out.reserve(size);
    out.append(lines_[size_t(from.line)], size_t(from.column), std::string::npos);
    for (int l = from.line + 1; l < to.line; ++l) {
        out += eol;
        out += lines_[size_t(l)];
    }
    out += eol;
    out.append(lines_[size_t(to.line)], 0, size_t(to.column));
    return out;
}

void TextBuffer::setLine(int index, std::string text)
{
    touched(index);
    if (journaling_)
        record({index, 0}, lines_[size_t(index)], text);
    lines_[size_t(index)] = std::move(text);
}

void TextBuffer::insertLine(int index, std::string text)
{
    touched(index);
    if (journaling_) {
        // As text: "line\n" before an existing line, or "\nline" after the last one.
        if (index < lineCount())
            record({std::max(0, index), 0}, {}, text + "\n");
        else
            record({lineCount() - 1, int(lines_.back().size())}, {}, "\n" + text);
    }
    lines_.insert(lines_.begin() + std::clamp(index, 0, lineCount()), std::move(text));
}

void TextBuffer::eraseLine(int index)
{
    touched(index);
    if (journaling_) {
        if (lineCount() == 1)
            record({0, 0}, lines_[0], {});
        else if (index < lineCount() - 1)
            record({index, 0}, lines_[size_t(index)] + "\n", {});
        else
            record({index - 1, int(lines_[size_t(index - 1)].size())}, "\n" + lines_[size_t(index)], {});
    }
    if (lineCount() == 1) {
        lines_[0].clear();
        return;
    }
    lines_.erase(lines_.begin() + index);
}

size_t TextBuffer::characterCount(int eolLength) const
{
    size_t count = 0;
    for (const std::string& l : lines_)
        count += size_t(codePointCount(l));
    return count + (lines_.size() - 1) * size_t(eolLength);
}

TextPos TextBuffer::clamp(TextPos pos) const
{
    pos.line = std::clamp(pos.line, 0, lineCount() - 1);
    const std::string& l = lines_[size_t(pos.line)];
    pos.column = std::clamp(pos.column, 0, int(l.size()));
    while (pos.column > 0 && pos.column < int(l.size()) && isUtf8Continuation(l[size_t(pos.column)]))
        --pos.column;
    return pos;
}

TextPos TextBuffer::end() const
{
    return TextPos{lineCount() - 1, int(lines_.back().size())};
}

TextPos TextBuffer::insert(TextPos at, std::string_view text)
{
    touched(at.line);
    at = clamp(at);
    std::string& first = lines_[size_t(at.line)];
    const std::string tail = first.substr(size_t(at.column));
    first.erase(size_t(at.column));

    TextPos pos = at;
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        const bool atEnd = i == text.size();
        if (!atEnd && text[i] != '\n' && text[i] != '\r')
            continue;
        lines_[size_t(pos.line)].append(text.substr(start, i - start));
        pos.column = int(lines_[size_t(pos.line)].size());
        if (atEnd)
            break;
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
            ++i;
        lines_.insert(lines_.begin() + pos.line + 1, std::string());
        ++pos.line;
        pos.column = 0;
        start = i + 1;
    }
    lines_[size_t(pos.line)] += tail;
    if (journaling_)
        record(at, {}, this->text(at, pos));
    return pos;
}

TextPos TextBuffer::erase(TextPos from, TextPos to)
{
    touched(std::min(from.line, to.line));
    from = clamp(from);
    to = clamp(to);
    if (to < from)
        std::swap(from, to);
    if (from == to)
        return from;
    if (journaling_)
        record(from, this->text(from, to), {});
    std::string& first = lines_[size_t(from.line)];
    const std::string tail = lines_[size_t(to.line)].substr(size_t(to.column));
    first.erase(size_t(from.column));
    first += tail;
    lines_.erase(lines_.begin() + from.line + 1, lines_.begin() + to.line + 1);
    return from;
}

TextPos TextBuffer::prev(TextPos pos) const
{
    pos = clamp(pos);
    if (pos.column == 0) {
        if (pos.line == 0)
            return pos;
        return TextPos{pos.line - 1, int(lines_[size_t(pos.line - 1)].size())};
    }
    const std::string& l = lines_[size_t(pos.line)];
    do
        --pos.column;
    while (pos.column > 0 && isUtf8Continuation(l[size_t(pos.column)]));
    return pos;
}

TextPos TextBuffer::next(TextPos pos) const
{
    pos = clamp(pos);
    const std::string& l = lines_[size_t(pos.line)];
    if (pos.column >= int(l.size())) {
        if (pos.line + 1 >= lineCount())
            return pos;
        return TextPos{pos.line + 1, 0};
    }
    do
        ++pos.column;
    while (pos.column < int(l.size()) && isUtf8Continuation(l[size_t(pos.column)]));
    return pos;
}

TextPos TextBuffer::wordStart(TextPos pos) const
{
    pos = clamp(pos);
    const std::string& l = lines_[size_t(pos.line)];
    while (pos.column > 0 && isWordChar(l[size_t(pos.column - 1)]))
        --pos.column;
    return pos;
}

TextPos TextBuffer::wordEnd(TextPos pos) const
{
    pos = clamp(pos);
    const std::string& l = lines_[size_t(pos.line)];
    while (pos.column < int(l.size()) && isWordChar(l[size_t(pos.column)]))
        ++pos.column;
    return pos;
}

void TextBuffer::touched(int line)
{
    ++version_;
    changes_.emplace_back(version_, std::max(0, line));
    if (changes_.size() > 256)
        changes_.pop_front();
}

int TextBuffer::firstLineChangedSince(uint64_t version) const
{
    if (version >= version_)
        return lineCount();
    if (changes_.empty() || changes_.front().first > version + 1)
        return 0; // older than the log: everything may have changed
    int first = lineCount();
    for (auto it = changes_.rbegin(); it != changes_.rend() && it->first > version; ++it)
        first = std::min(first, it->second);
    return first;
}

void TextBuffer::record(TextPos at, std::string removed, std::string inserted)
{
    journal_.push_back({at, std::move(removed), std::move(inserted)});
}

TextPos TextBuffer::endOf(TextPos at, const std::string& text)
{
    const size_t lastBreak = text.rfind('\n');
    if (lastBreak == std::string::npos)
        return {at.line, at.column + int(text.size())};
    return {at.line + int(std::count(text.begin(), text.end(), '\n')), int(text.size() - lastBreak - 1)};
}

void TextBuffer::beginJournal()
{
    journaling_ = true;
    journal_.clear();
}

std::vector<TextChange> TextBuffer::endJournal()
{
    journaling_ = false;
    return std::move(journal_);
}

void TextBuffer::revert(const std::vector<TextChange>& changes)
{
    const bool was = journaling_;
    journaling_ = false;
    for (auto it = changes.rbegin(); it != changes.rend(); ++it) {
        erase(it->at, endOf(it->at, it->inserted));
        insert(it->at, it->removed);
    }
    journaling_ = was;
}

void TextBuffer::reapply(const std::vector<TextChange>& changes)
{
    const bool was = journaling_;
    journaling_ = false;
    for (const TextChange& c : changes) {
        erase(c.at, endOf(c.at, c.removed));
        insert(c.at, c.inserted);
    }
    journaling_ = was;
}

bool TextBuffer::equals(std::string_view text) const
{
    size_t at = 0;
    for (size_t l = 0; l < lines_.size(); ++l) {
        const std::string& line = lines_[l];
        if (text.compare(at, line.size(), line) != 0 || at + line.size() > text.size())
            return false;
        at += line.size();
        if (l + 1 < lines_.size()) {
            if (at >= text.size() || text[at] != '\n')
                return false;
            ++at;
        }
    }
    return at == text.size();
}

} // namespace enp::model
