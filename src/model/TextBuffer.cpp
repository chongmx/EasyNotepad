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
    std::string out = lines_[size_t(from.line)].substr(size_t(from.column));
    for (int l = from.line + 1; l < to.line; ++l) {
        out += eol;
        out += lines_[size_t(l)];
    }
    out += eol;
    out += lines_[size_t(to.line)].substr(0, size_t(to.column));
    return out;
}

void TextBuffer::setLine(int index, std::string text)
{
    lines_[size_t(index)] = std::move(text);
}

void TextBuffer::insertLine(int index, std::string text)
{
    lines_.insert(lines_.begin() + std::clamp(index, 0, lineCount()), std::move(text));
}

void TextBuffer::eraseLine(int index)
{
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
    return pos;
}

TextPos TextBuffer::erase(TextPos from, TextPos to)
{
    from = clamp(from);
    to = clamp(to);
    if (to < from)
        std::swap(from, to);
    if (from == to)
        return from;
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

} // namespace enp::model
