#include "enp/model/Markdown.h"

#include <algorithm>
#include <cctype>

namespace enp::model::markdown {

namespace {

bool isSpace(char c) { return c == ' ' || c == '\t'; }
bool isPunct(char c) { return std::ispunct(static_cast<unsigned char>(c)) != 0; }
bool isAlnum(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || static_cast<unsigned char>(c) >= 0x80; }

std::string_view trim(std::string_view s)
{
    while (!s.empty() && isSpace(s.front()))
        s.remove_prefix(1);
    while (!s.empty() && (isSpace(s.back()) || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

int indentOf(std::string_view line)
{
    int n = 0;
    for (char c : line) {
        if (c == ' ')
            ++n;
        else if (c == '\t')
            n += 4 - n % 4;
        else
            break;
    }
    return n;
}

// ---- Inline -------------------------------------------------------------------------------------

class InlineParser {
public:
    explicit InlineParser(std::string_view text) : text_(text) {}

    Spans run()
    {
        for (size_t i = 0; i < text_.size();) {
            const char c = text_[i];
            if (c == '\\' && i + 1 < text_.size() && isPunct(text_[i + 1])) {
                buffer_ += text_[i + 1];
                i += 2;
            } else if (c == '`') {
                i = codeSpan(i);
            } else if (c == '!' && i + 1 < text_.size() && text_[i + 1] == '[') {
                const size_t next = link(i + 1);
                i = next == i + 1 ? (buffer_ += c, i + 1) : next; // an image shows as a link to it
            } else if (c == '[') {
                const size_t next = link(i);
                i = next == i ? (buffer_ += c, i + 1) : next;
            } else if (c == '<') {
                i = autolink(i);
            } else if ((c == 'h' || c == 'w') && (i == 0 || !isAlnum(text_[i - 1]))) {
                i = bareUrl(i);
            } else if (c == '*' || c == '_' || (c == '~' && i + 1 < text_.size() && text_[i + 1] == '~')) {
                i = emphasis(i);
            } else {
                buffer_ += c;
                ++i;
            }
        }
        flush();
        return std::move(spans_);
    }

private:
    std::string_view text_;
    Spans spans_;
    std::string buffer_;
    unsigned style_ = Plain;

    void flush()
    {
        if (buffer_.empty())
            return;
        if (!spans_.empty() && spans_.back().style == style_ && spans_.back().url.empty())
            spans_.back().text += buffer_;
        else
            spans_.push_back({buffer_, style_, {}});
        buffer_.clear();
    }

    size_t codeSpan(size_t i)
    {
        size_t n = 0;
        while (i + n < text_.size() && text_[i + n] == '`')
            ++n;
        for (size_t j = i + n; j < text_.size();) {
            if (text_[j] != '`') {
                ++j;
                continue;
            }
            size_t m = 0;
            while (j + m < text_.size() && text_[j + m] == '`')
                ++m;
            if (m == n) {
                std::string code(text_.substr(i + n, j - i - n));
                if (code.size() >= 2 && code.front() == ' ' && code.back() == ' ')
                    code = code.substr(1, code.size() - 2);
                flush();
                spans_.push_back({code, style_ | Code, {}});
                return j + m;
            }
            j += m;
        }
        buffer_.append(text_.substr(i, n)); // no closing run: literal backticks
        return i + n;
    }

    // [text](url "title"); returns `i` when it is not a link.
    size_t link(size_t i)
    {
        int depth = 0;
        size_t close = std::string_view::npos;
        for (size_t j = i; j < text_.size(); ++j) {
            if (text_[j] == '\\') {
                ++j;
                continue;
            }
            if (text_[j] == '[')
                ++depth;
            else if (text_[j] == ']' && --depth == 0) {
                close = j;
                break;
            }
        }
        if (close == std::string_view::npos || close + 1 >= text_.size() || text_[close + 1] != '(')
            return i;
        const size_t end = text_.find(')', close + 2);
        if (end == std::string_view::npos)
            return i;
        std::string_view target = trim(text_.substr(close + 2, end - close - 2));
        if (const size_t space = target.find(' '); space != std::string_view::npos)
            target = target.substr(0, space); // drop a "title"
        if (target.size() >= 2 && target.front() == '<' && target.back() == '>')
            target = target.substr(1, target.size() - 2);
        flush();
        Spans inner = InlineParser(text_.substr(i + 1, close - i - 1)).run();
        if (inner.empty())
            inner.push_back({std::string(target), Plain, {}});
        for (Span& span : inner) {
            span.style |= style_ | Link;
            span.url = std::string(target);
            spans_.push_back(std::move(span));
        }
        return end + 1;
    }

    size_t autolink(size_t i)
    {
        const size_t end = text_.find('>', i);
        if (end != std::string_view::npos) {
            const std::string_view inside = text_.substr(i + 1, end - i - 1);
            if (inside.find(' ') == std::string_view::npos &&
                (inside.rfind("http://", 0) == 0 || inside.rfind("https://", 0) == 0 || inside.rfind("mailto:", 0) == 0)) {
                flush();
                spans_.push_back({std::string(inside), style_ | Link, std::string(inside)});
                return end + 1;
            }
        }
        buffer_ += '<';
        return i + 1;
    }

    size_t bareUrl(size_t i)
    {
        const std::string_view rest = text_.substr(i);
        if (rest.rfind("https://", 0) != 0 && rest.rfind("http://", 0) != 0 && rest.rfind("www.", 0) != 0) {
            buffer_ += text_[i];
            return i + 1;
        }
        size_t end = i;
        while (end < text_.size() && !isSpace(text_[end]) && text_[end] != '<' && text_[end] != '`')
            ++end;
        while (end > i && std::string_view(".,;:!?)'\"*_").find(text_[end - 1]) != std::string_view::npos)
            --end; // sentence punctuation is not part of it
        const std::string url(text_.substr(i, end - i));
        flush();
        spans_.push_back({url, style_ | Link, url.rfind("www.", 0) == 0 ? "https://" + url : url});
        return end;
    }

    // Does a closing run `run` follow `from` (preceded by non-space)?
    bool closes(std::string_view run, size_t from) const
    {
        for (size_t j = text_.find(run, from); j != std::string_view::npos; j = text_.find(run, j + 1)) {
            if (j > from && !isSpace(text_[j - 1]) && (run[0] != '_' || j + run.size() >= text_.size() || !isAlnum(text_[j + run.size()])))
                return true;
        }
        return false;
    }

    size_t emphasis(size_t i)
    {
        const char c = text_[i];
        size_t n = 0;
        while (i + n < text_.size() && text_[i + n] == c)
            ++n;
        if (c == '~' && n != 2) {
            buffer_.append(text_.substr(i, n));
            return i + n;
        }
        const size_t length = c == '~' ? 2 : std::min<size_t>(n, 3);
        const std::string_view run = text_.substr(i, length);
        const unsigned flag = c == '~' ? Strike : length == 3 ? (Bold | Italic) : length == 2 ? Bold : Italic;
        const bool before = i > 0 && !isSpace(text_[i - 1]);
        const bool after = i + length < text_.size() && !isSpace(text_[i + length]);
        // Underscores inside words (snake_case) are text.
        if (c == '_' && ((i > 0 && isAlnum(text_[i - 1])) && (i + length < text_.size() && isAlnum(text_[i + length])))) {
            buffer_.append(run);
            return i + length;
        }
        if ((style_ & flag) == flag && before) { // closing
            flush();
            style_ &= ~flag;
            return i + length;
        }
        if ((style_ & flag) == 0 && after && closes(run, i + length)) { // opening
            flush();
            style_ |= flag;
            return i + length;
        }
        buffer_.append(run);
        return i + length;
    }
};

// ---- Blocks -------------------------------------------------------------------------------------

struct ListMarker {
    bool ordered = false;
    std::string marker;
    int indent = 0;   // of the marker
    size_t content = 0; // where the item's text starts in the line
};

bool listMarker(std::string_view line, ListMarker* out)
{
    const int indent = indentOf(line);
    size_t i = 0;
    while (i < line.size() && isSpace(line[i]))
        ++i;
    if (i >= line.size())
        return false;
    ListMarker m;
    m.indent = indent;
    if (line[i] == '-' || line[i] == '*' || line[i] == '+') {
        m.marker = std::string(1, line[i]);
        ++i;
    } else if (std::isdigit(static_cast<unsigned char>(line[i]))) {
        size_t j = i;
        while (j < line.size() && j - i < 9 && std::isdigit(static_cast<unsigned char>(line[j])))
            ++j;
        if (j >= line.size() || (line[j] != '.' && line[j] != ')'))
            return false;
        m.ordered = true;
        m.marker = std::string(line.substr(i, j - i)) + ".";
        i = j + 1;
    } else {
        return false;
    }
    if (i < line.size() && !isSpace(line[i]))
        return false;
    while (i < line.size() && isSpace(line[i]))
        ++i;
    m.content = i;
    if (out)
        *out = m;
    return true;
}

bool isRule(std::string_view t)
{
    if (t.size() < 3)
        return false;
    const char c = t[0];
    if (c != '-' && c != '*' && c != '_')
        return false;
    int count = 0;
    for (char ch : t) {
        if (ch == c)
            ++count;
        else if (!isSpace(ch))
            return false;
    }
    return count >= 3;
}

int headingLevel(std::string_view t)
{
    int n = 0;
    while (n < int(t.size()) && t[size_t(n)] == '#')
        ++n;
    if (n < 1 || n > 6 || (n < int(t.size()) && !isSpace(t[size_t(n)])))
        return 0;
    return n;
}

bool isFence(std::string_view t, char* fenceChar, size_t* fenceLength)
{
    if (t.size() < 3 || (t[0] != '`' && t[0] != '~'))
        return false;
    size_t n = 0;
    while (n < t.size() && t[n] == t[0])
        ++n;
    if (n < 3)
        return false;
    if (t[0] == '`' && t.substr(n).find('`') != std::string_view::npos)
        return false;
    *fenceChar = t[0];
    *fenceLength = n;
    return true;
}

// The cells of a table row: split at '|' outside code spans.
std::vector<std::string> cells(std::string_view line)
{
    std::string_view t = trim(line);
    if (!t.empty() && t.front() == '|')
        t.remove_prefix(1);
    if (!t.empty() && t.back() == '|' && (t.size() < 2 || t[t.size() - 2] != '\\'))
        t.remove_suffix(1);
    std::vector<std::string> out;
    std::string cell;
    bool code = false;
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == '\\' && i + 1 < t.size() && t[i + 1] == '|') {
            cell += '|';
            ++i;
        } else if (t[i] == '`') {
            code = !code;
            cell += '`';
        } else if (t[i] == '|' && !code) {
            out.emplace_back(trim(cell));
            cell.clear();
        } else {
            cell += t[i];
        }
    }
    out.emplace_back(trim(cell));
    return out;
}

bool isTableSeparator(std::string_view line, std::vector<int>* align)
{
    const std::string_view t = trim(line);
    if (t.find('-') == std::string_view::npos || t.find('|') == std::string_view::npos)
        return false;
    std::vector<int> result;
    for (const std::string& cell : cells(t)) {
        if (cell.empty())
            return false;
        const bool left = cell.front() == ':';
        const bool right = cell.back() == ':';
        for (char c : cell)
            if (c != '-' && c != ':')
                return false;
        result.push_back(left && right ? 0 : right ? 1 : -1);
    }
    if (align)
        *align = result;
    return true;
}

class BlockParser {
public:
    explicit BlockParser(std::vector<std::string_view> lines, int firstLine, int quote)
        : lines_(std::move(lines)), firstLine_(firstLine), quote_(quote)
    {
    }

    std::vector<Block> run()
    {
        while (i_ < lines_.size()) {
            const std::string_view line = lines_[i_];
            const std::string_view t = trim(line);
            if (t.empty()) {
                ++i_;
                listIndents_.clear();
                continue;
            }
            char fenceChar = 0;
            size_t fenceLength = 0;
            if (indentOf(line) < 4 && isFence(t, &fenceChar, &fenceLength))
                code(fenceChar, fenceLength, indentOf(line));
            else if (t[0] == '>')
                quote();
            else if (const int level = headingLevel(t); level > 0 && indentOf(line) < 4)
                heading(level, t);
            else if (isRule(t) && indentOf(line) < 4)
                add(BlockKind::Rule, i_++);
            else if (ListMarker m; listMarker(line, &m))
                item(m);
            else if (i_ + 1 < lines_.size() && line.find('|') != std::string_view::npos && isTableSeparator(lines_[i_ + 1], nullptr))
                table();
            else
                paragraph();
            if (!out_.empty() && out_.back().kind != BlockKind::Bullet && out_.back().kind != BlockKind::Numbered &&
                out_.back().kind != BlockKind::Task)
                listIndents_.clear();
        }
        return std::move(out_);
    }

private:
    std::vector<std::string_view> lines_;
    int firstLine_;
    int quote_;
    size_t i_ = 0;
    std::vector<Block> out_;
    std::vector<int> listIndents_; // marker indents of the open list levels

    Block& add(BlockKind kind, size_t line)
    {
        Block b;
        b.kind = kind;
        b.quote = quote_;
        b.line = firstLine_ + int(line);
        out_.push_back(std::move(b));
        return out_.back();
    }

    // Does this line start a block other than a paragraph?
    bool startsBlock(size_t index) const
    {
        const std::string_view line = lines_[index];
        const std::string_view t = trim(line);
        char c = 0;
        size_t n = 0;
        return t.empty() || t[0] == '>' || headingLevel(t) > 0 || isRule(t) || isFence(t, &c, &n) || listMarker(line, nullptr) ||
               (index + 1 < lines_.size() && line.find('|') != std::string_view::npos && isTableSeparator(lines_[index + 1], nullptr));
    }

    // Joins lines into inline text: a line ending in two spaces or a backslash breaks the line.
    static void join(std::string& text, std::string_view line)
    {
        if (!text.empty()) {
            if (text.size() >= 2 && text.compare(text.size() - 2, 2, "  ") == 0) {
                while (!text.empty() && text.back() == ' ')
                    text.pop_back();
                text += '\n';
            } else if (text.back() == '\\') {
                text.back() = '\n';
            } else {
                while (!text.empty() && text.back() == ' ')
                    text.pop_back();
                text += ' ';
            }
        }
        std::string_view l = line;
        while (!l.empty() && isSpace(l.front()))
            l.remove_prefix(1);
        while (!l.empty() && l.back() == '\r')
            l.remove_suffix(1);
        text.append(l);
    }

    void code(char fenceChar, size_t fenceLength, int indent)
    {
        Block& b = add(BlockKind::Code, i_);
        std::string_view info = trim(trim(lines_[i_]).substr(fenceLength));
        b.language = std::string(info.substr(0, info.find_first_of(" \t{")));
        ++i_;
        bool first = true;
        while (i_ < lines_.size()) { // an unclosed fence runs to the end (an answer still streaming)
            const std::string_view line = lines_[i_];
            const std::string_view t = trim(line);
            if (t.size() >= fenceLength && t.find_first_not_of(fenceChar) == std::string_view::npos) {
                ++i_;
                break;
            }
            std::string_view content = line;
            for (int k = 0; k < indent && !content.empty() && content.front() == ' '; ++k)
                content.remove_prefix(1);
            while (!content.empty() && content.back() == '\r')
                content.remove_suffix(1);
            if (!first)
                b.code += '\n';
            b.code.append(content);
            first = false;
            ++i_;
        }
    }

    void quote()
    {
        const size_t start = i_;
        std::vector<std::string_view> inner;
        while (i_ < lines_.size()) {
            std::string_view t = trim(lines_[i_]);
            if (t.empty() || (t[0] != '>' && (startsBlock(i_) || inner.empty())))
                break;
            if (t[0] == '>') {
                t.remove_prefix(1);
                if (!t.empty() && t.front() == ' ')
                    t.remove_prefix(1);
            }
            inner.push_back(t);
            ++i_;
        }
        for (Block& b : BlockParser(std::move(inner), firstLine_ + int(start), quote_ + 1).run())
            out_.push_back(std::move(b));
    }

    void heading(int level, std::string_view t)
    {
        std::string_view text = trim(t.substr(size_t(level)));
        // Closing #s ("## Title ##") are not part of it.
        const size_t hashes = text.find_last_not_of('#');
        if (hashes != std::string_view::npos && hashes + 1 < text.size() && isSpace(text[hashes]))
            text = trim(text.substr(0, hashes));
        else if (hashes == std::string_view::npos)
            text = {};
        Block& b = add(BlockKind::Heading, i_++);
        b.level = level;
        b.spans = parseInline(text);
    }

    void item(const ListMarker& m)
    {
        while (!listIndents_.empty() && listIndents_.back() > m.indent)
            listIndents_.pop_back();
        if (listIndents_.empty() || listIndents_.back() < m.indent)
            listIndents_.push_back(m.indent);
        std::string text(lines_[i_].substr(m.content));
        BlockKind kind = m.ordered ? BlockKind::Numbered : BlockKind::Bullet;
        bool checked = false;
        if (!m.ordered && text.size() >= 3 && text[0] == '[' && text[2] == ']' && (text.size() == 3 || text[3] == ' ') &&
            (text[1] == ' ' || text[1] == 'x' || text[1] == 'X')) {
            kind = BlockKind::Task;
            checked = text[1] != ' ';
            text = text.size() > 4 ? text.substr(4) : std::string();
        }
        const size_t start = i_++;
        // Continuation lines: indented or lazy text that does not start a block.
        while (i_ < lines_.size() && !startsBlock(i_))
            join(text, lines_[i_++]);
        Block& b = add(kind, start);
        b.level = int(listIndents_.size()) - 1;
        b.marker = m.ordered ? m.marker : std::string();
        b.checked = checked;
        b.spans = parseInline(text);
    }

    void table()
    {
        Block& b = add(BlockKind::Table, i_);
        isTableSeparator(lines_[i_ + 1], &b.align);
        std::vector<std::string> header = cells(lines_[i_]);
        auto addRow = [&b](const std::vector<std::string>& texts) {
            std::vector<Spans> row;
            for (size_t c = 0; c < std::max(texts.size(), b.align.size()); ++c)
                row.push_back(c < texts.size() ? parseInline(texts[c]) : Spans());
            b.rows.push_back(std::move(row));
        };
        addRow(header);
        i_ += 2;
        while (i_ < lines_.size() && !trim(lines_[i_]).empty() && lines_[i_].find('|') != std::string_view::npos) {
            addRow(cells(lines_[i_]));
            ++i_;
        }
        const size_t columns = b.align.size();
        for (std::vector<Spans>& row : b.rows)
            row.resize(std::max(columns, row.size()));
        b.align.resize(b.rows.empty() ? columns : b.rows[0].size(), -1);
    }

    void paragraph()
    {
        const size_t start = i_;
        std::string text;
        join(text, lines_[i_++]);
        while (i_ < lines_.size()) {
            const std::string_view t = trim(lines_[i_]);
            // Setext heading: the paragraph underlined with === or ---.
            if (!t.empty() && t.find_first_not_of('=') == std::string_view::npos) {
                Block& b = add(BlockKind::Heading, start);
                b.level = 1;
                b.spans = parseInline(text);
                ++i_;
                return;
            }
            if (!t.empty() && t.size() >= 2 && t.find_first_not_of('-') == std::string_view::npos) {
                Block& b = add(BlockKind::Heading, start);
                b.level = 2;
                b.spans = parseInline(text);
                ++i_;
                return;
            }
            if (startsBlock(i_))
                break;
            join(text, lines_[i_++]);
        }
        add(BlockKind::Paragraph, start).spans = parseInline(text);
    }
};

} // namespace

Spans parseInline(std::string_view text)
{
    return InlineParser(text).run();
}

std::vector<Block> parse(std::string_view text)
{
    std::vector<std::string_view> lines;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = text.find('\n', start);
        std::string_view line = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        lines.push_back(line);
        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }
    return BlockParser(std::move(lines), 0, 0).run();
}

std::string plainText(const Spans& spans)
{
    std::string text;
    for (const Span& span : spans)
        text += span.text;
    return text;
}

} // namespace enp::model::markdown
