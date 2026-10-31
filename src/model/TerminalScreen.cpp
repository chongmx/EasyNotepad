#include "enp/model/TerminalScreen.h"

namespace enp::model {

namespace termcolor {

uint32_t paletteRgb(int index, const uint32_t base16[16])
{
    if (index < 16)
        return base16[index];
    if (index < 232) { // 6x6x6 colour cube
        static const int levels[] = {0, 95, 135, 175, 215, 255};
        const int i = index - 16;
        return (uint32_t(levels[i / 36]) << 16) | (uint32_t(levels[(i / 6) % 6]) << 8) | uint32_t(levels[i % 6]);
    }
    const int v = 8 + (index - 232) * 10; // grayscale ramp
    return (uint32_t(v) << 16) | (uint32_t(v) << 8) | uint32_t(v);
}

} // namespace termcolor

namespace {

void appendUtf8(std::string& out, char32_t c)
{
    if (c < 0x80) {
        out += char(c);
    } else if (c < 0x800) {
        out += char(0xC0 | (c >> 6));
        out += char(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += char(0xE0 | (c >> 12));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    } else {
        out += char(0xF0 | (c >> 18));
        out += char(0x80 | ((c >> 12) & 0x3F));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    }
}

} // namespace

TerminalScreen::TerminalScreen(int columns, int rows)
    : columns_(std::max(1, columns)), rows_(std::max(1, rows))
{
    reset();
}

TermLine TerminalScreen::blankLine() const
{
    TermLine line;
    line.cells.assign(size_t(columns_), TermCell{U' ', TermAttr{termcolor::kDefault, attr_.bg, 0}});
    return line;
}

void TerminalScreen::reset()
{
    attr_ = TermAttr();
    main_.assign(size_t(rows_), blankLine());
    alt_.assign(size_t(rows_), blankLine());
    scrollback_.clear();
    altActive_ = false;
    cursorRow_ = cursorCol_ = 0;
    wrapPending_ = false;
    scrollTop_ = 0;
    scrollBottom_ = rows_ - 1;
    autoWrap_ = true;
    cursorVisible_ = true;
    appCursorKeys_ = false;
    bracketedPaste_ = false;
    originMode_ = false;
    saved_ = savedMain_ = Saved();
    state_ = State::Ground;
    title_.clear();
    changed();
}

void TerminalScreen::clearKeepingCursorLine()
{
    std::vector<TermLine>& lines = screen();
    TermLine keep = std::move(lines[size_t(cursorRow_)]);
    keep.wrapped = false;
    for (TermLine& line : lines)
        line = blankLine();
    lines[0] = std::move(keep);
    if (!altActive_)
        scrollback_.clear();
    cursorRow_ = 0;
    changed();
}

const TermLine& TerminalScreen::lineAt(int index) const
{
    if (index < int(scrollback_.size()))
        return scrollback_[size_t(index)];
    return screen()[size_t(std::clamp(index - int(scrollback_.size()), 0, rows_ - 1))];
}

std::string TerminalScreen::takeResponses()
{
    std::string out;
    out.swap(responses_);
    return out;
}

// ---- Resize -------------------------------------------------------------------------------------

void TerminalScreen::resize(int columns, int rows)
{
    columns = std::max(1, columns);
    rows = std::max(1, rows);
    if (columns == columns_ && rows == rows_)
        return;
    auto fit = [&](std::vector<TermLine>& lines, bool keepHistory) {
        for (TermLine& line : lines)
            line.cells.resize(size_t(columns), TermCell());
        if (rows < int(lines.size())) {
            // Keep the cursor on screen: rows above it move to the scrollback first.
            int excess = int(lines.size()) - rows;
            const int belowCursor = int(lines.size()) - 1 - cursorRow_;
            const int dropBottom = std::min(excess, belowCursor);
            lines.resize(lines.size() - size_t(dropBottom));
            excess -= dropBottom;
            for (int i = 0; i < excess; ++i) {
                if (keepHistory)
                    scrollback_.push_back(std::move(lines.front()));
                lines.erase(lines.begin());
            }
            if (&lines == &screen())
                cursorRow_ -= excess;
        }
        TermLine blank;
        blank.cells.assign(size_t(columns), TermCell());
        while (int(lines.size()) < rows)
            lines.push_back(blank);
    };
    fit(main_, true); // only the active screen's fit moves the cursor
    fit(alt_, false);
    for (TermLine& line : scrollback_)
        line.cells.resize(size_t(columns), TermCell());
    while (int(scrollback_.size()) > kMaxScrollback)
        scrollback_.pop_front();
    columns_ = columns;
    rows_ = rows;
    scrollTop_ = 0;
    scrollBottom_ = rows_ - 1;
    cursorRow_ = std::clamp(cursorRow_, 0, rows_ - 1);
    cursorCol_ = std::clamp(cursorCol_, 0, columns_ - 1);
    wrapPending_ = false;
    changed();
}

// ---- Parser -------------------------------------------------------------------------------------

void TerminalScreen::feed(std::string_view bytes)
{
    for (const char byte : bytes) {
        const unsigned char b = static_cast<unsigned char>(byte);
        switch (state_) {
        case State::Ground:
            if (utf8Remaining_ > 0) {
                if ((b & 0xC0) == 0x80) {
                    utf8Codepoint_ = (utf8Codepoint_ << 6) | (b & 0x3F);
                    if (--utf8Remaining_ == 0)
                        print(utf8Codepoint_);
                    continue;
                }
                utf8Remaining_ = 0;
                print(U'�'); // truncated sequence; reprocess this byte below
            }
            if (b == 0x1B) {
                state_ = State::Escape;
            } else if (b < 0x20 || b == 0x7F) {
                control(char(b));
            } else if (b < 0x80) {
                print(b);
            } else if ((b & 0xE0) == 0xC0) {
                utf8Codepoint_ = b & 0x1F;
                utf8Remaining_ = 1;
            } else if ((b & 0xF0) == 0xE0) {
                utf8Codepoint_ = b & 0x0F;
                utf8Remaining_ = 2;
            } else if ((b & 0xF8) == 0xF0) {
                utf8Codepoint_ = b & 0x07;
                utf8Remaining_ = 3;
            } else {
                print(U'�');
            }
            break;
        case State::Escape:
            if (b == '[') {
                state_ = State::Csi;
                params_.clear();
                paramStarted_ = false;
                privateMarker_ = 0;
                intermediates_.clear();
            } else if (b == ']') {
                state_ = State::Osc;
                oscBuffer_.clear();
            } else if (b == 'P' || b == 'X' || b == '^' || b == '_') {
                state_ = State::IgnoreString; // DCS, SOS, PM, APC
            } else if (b == '(' || b == ')' || b == '*' || b == '+') {
                state_ = State::Charset;
            } else if (b >= 0x20 && b <= 0x2F) {
                state_ = State::EscapeIntermediate;
            } else {
                state_ = State::Ground;
                escape(char(b));
            }
            break;
        case State::EscapeIntermediate:
            if (b >= 0x30)
                state_ = State::Ground; // e.g. ESC # 8; nothing to do
            break;
        case State::Charset:
            state_ = State::Ground; // character sets are not switched; UTF-8 only
            break;
        case State::Csi:
            if (b >= '0' && b <= '9') {
                if (!paramStarted_) {
                    params_.push_back(0);
                    paramStarted_ = true;
                }
                params_.back() = std::min(params_.back() * 10 + (b - '0'), 99999);
            } else if (b == ';' || b == ':') {
                if (!paramStarted_)
                    params_.push_back(0);
                paramStarted_ = false;
            } else if (b >= 0x3C && b <= 0x3F) { // < = > ?
                privateMarker_ = char(b);
            } else if (b >= 0x20 && b <= 0x2F) {
                intermediates_ += char(b);
            } else if (b >= 0x40 && b <= 0x7E) {
                if (!paramStarted_ && !params_.empty())
                    params_.push_back(0); // trailing ';'
                state_ = State::Ground;
                csi(char(b));
            } else if (b == 0x1B) {
                state_ = State::Escape;
            } else if (b < 0x20) {
                control(char(b)); // C0 controls execute inside CSI
            }
            break;
        case State::Osc:
            if (b == 0x07) {
                osc();
                state_ = State::Ground;
            } else if (b == 0x1B) {
                state_ = State::OscEscape;
            } else if (oscBuffer_.size() < 4096) {
                oscBuffer_ += char(b);
            }
            break;
        case State::OscEscape: // ESC \ terminates
            osc();
            state_ = State::Ground;
            break;
        case State::IgnoreString:
            if (b == 0x07)
                state_ = State::Ground;
            else if (b == 0x1B)
                state_ = State::IgnoreStringEscape;
            break;
        case State::IgnoreStringEscape:
            state_ = b == '\\' ? State::Ground : State::IgnoreString;
            break;
        }
    }
}

int TerminalScreen::param(size_t index, int fallback) const
{
    return index < params_.size() && params_[index] != 0 ? params_[index] : fallback;
}

void TerminalScreen::print(char32_t ch)
{
    std::vector<TermLine>& lines = screen();
    if (wrapPending_) {
        wrapPending_ = false;
        if (autoWrap_) {
            lines[size_t(cursorRow_)].wrapped = true;
            cursorCol_ = 0;
            lineFeed();
        }
    }
    TermCell& cell = lines[size_t(cursorRow_)].cells[size_t(cursorCol_)];
    cell.ch = ch;
    cell.attr = attr_;
    if (cursorCol_ == columns_ - 1)
        wrapPending_ = true;
    else
        ++cursorCol_;
    changed();
}

void TerminalScreen::control(char c)
{
    switch (c) {
    case '\r':
        cursorCol_ = 0;
        wrapPending_ = false;
        break;
    case '\n':
    case '\v':
    case '\f':
        lineFeed();
        break;
    case '\b':
        if (cursorCol_ > 0)
            --cursorCol_;
        wrapPending_ = false;
        break;
    case '\t':
        cursorCol_ = std::min(columns_ - 1, (cursorCol_ / 8 + 1) * 8);
        wrapPending_ = false;
        break;
    default:
        break; // BEL, SO/SI and the rest are ignored
    }
    changed();
}

void TerminalScreen::escape(char c)
{
    switch (c) {
    case '7':
        saved_ = {cursorRow_, cursorCol_, attr_, originMode_};
        break;
    case '8':
        cursorRow_ = std::clamp(saved_.row, 0, rows_ - 1);
        cursorCol_ = std::clamp(saved_.col, 0, columns_ - 1);
        attr_ = saved_.attr;
        originMode_ = saved_.originMode;
        wrapPending_ = false;
        break;
    case 'D':
        lineFeed();
        break;
    case 'E':
        cursorCol_ = 0;
        lineFeed();
        break;
    case 'M':
        reverseIndex();
        break;
    case 'c':
        reset();
        break;
    default:
        break; // '=' / '>' keypad modes and others
    }
    changed();
}

void TerminalScreen::osc()
{
    // "0;title" or "2;title" set the window title; other OSC commands are ignored.
    const size_t semicolon = oscBuffer_.find(';');
    if (semicolon == std::string::npos)
        return;
    const std::string command = oscBuffer_.substr(0, semicolon);
    if (command == "0" || command == "2")
        title_ = oscBuffer_.substr(semicolon + 1);
    changed();
}

void TerminalScreen::moveCursor(int row, int column)
{
    const int top = originMode_ ? scrollTop_ : 0;
    const int bottom = originMode_ ? scrollBottom_ : rows_ - 1;
    cursorRow_ = std::clamp(row + top, top, bottom);
    cursorCol_ = std::clamp(column, 0, columns_ - 1);
    wrapPending_ = false;
}

void TerminalScreen::csi(char final)
{
    const int n = param(0, 1);
    if (privateMarker_ == '?' && (final == 'h' || final == 'l')) {
        setMode(true, final == 'h');
        changed();
        return;
    }
    if (privateMarker_ != 0 && final != 'c')
        return; // other private sequences (e.g. CSI > ... m) are not needed
    switch (final) {
    case 'A': moveCursor(cursorRow_ - (originMode_ ? scrollTop_ : 0) - n, cursorCol_); break;
    case 'B':
    case 'e': moveCursor(cursorRow_ - (originMode_ ? scrollTop_ : 0) + n, cursorCol_); break;
    case 'C':
    case 'a': moveCursor(cursorRow_ - (originMode_ ? scrollTop_ : 0), cursorCol_ + n); break;
    case 'D': moveCursor(cursorRow_ - (originMode_ ? scrollTop_ : 0), cursorCol_ - n); break;
    case 'E': moveCursor(cursorRow_ - (originMode_ ? scrollTop_ : 0) + n, 0); break;
    case 'F': moveCursor(cursorRow_ - (originMode_ ? scrollTop_ : 0) - n, 0); break;
    case 'G':
    case '`': moveCursor(cursorRow_ - (originMode_ ? scrollTop_ : 0), n - 1); break;
    case 'd': moveCursor(n - 1, cursorCol_); break;
    case 'H':
    case 'f': moveCursor(param(0, 1) - 1, param(1, 1) - 1); break;
    case 'J': eraseInDisplay(params_.empty() ? 0 : params_[0]); break;
    case 'K': eraseInLine(params_.empty() ? 0 : params_[0]); break;
    case 'X': {
        TermLine& line = screen()[size_t(cursorRow_)];
        clearCells(line, cursorCol_, std::min(columns_, cursorCol_ + n));
        break;
    }
    case '@': { // insert blank characters
        auto& cells = screen()[size_t(cursorRow_)].cells;
        const int count = std::min(n, columns_ - cursorCol_);
        cells.insert(cells.begin() + cursorCol_, size_t(count), TermCell{U' ', TermAttr{0, attr_.bg, 0}});
        cells.resize(size_t(columns_));
        break;
    }
    case 'P': { // delete characters
        auto& cells = screen()[size_t(cursorRow_)].cells;
        const int count = std::min(n, columns_ - cursorCol_);
        cells.erase(cells.begin() + cursorCol_, cells.begin() + cursorCol_ + count);
        cells.resize(size_t(columns_), TermCell{U' ', TermAttr{0, attr_.bg, 0}});
        break;
    }
    case 'L':
        if (cursorRow_ >= scrollTop_ && cursorRow_ <= scrollBottom_)
            scrollDown(cursorRow_, scrollBottom_, n);
        cursorCol_ = 0;
        break;
    case 'M':
        if (cursorRow_ >= scrollTop_ && cursorRow_ <= scrollBottom_)
            scrollUp(cursorRow_, scrollBottom_, n);
        cursorCol_ = 0;
        break;
    case 'S': scrollUp(scrollTop_, scrollBottom_, n); break;
    case 'T': scrollDown(scrollTop_, scrollBottom_, n); break;
    case 'm': sgr(); break;
    case 'r': {
        const int top = param(0, 1) - 1;
        const int bottom = param(1, rows_) - 1;
        if (top < bottom && bottom < rows_) {
            scrollTop_ = top;
            scrollBottom_ = bottom;
            moveCursor(0, 0);
        }
        break;
    }
    case 's':
        saved_ = {cursorRow_, cursorCol_, attr_, originMode_};
        break;
    case 'u':
        cursorRow_ = std::clamp(saved_.row, 0, rows_ - 1);
        cursorCol_ = std::clamp(saved_.col, 0, columns_ - 1);
        wrapPending_ = false;
        break;
    case 'h':
    case 'l':
        setMode(false, final == 'h');
        break;
    case 'n':
        if (param(0, 0) == 5)
            responses_ += "\x1b[0n";
        else if (param(0, 0) == 6)
            responses_ += "\x1b[" + std::to_string(cursorRow_ + 1) + ";" + std::to_string(std::min(cursorCol_, columns_ - 1) + 1) + "R";
        break;
    case 'c':
        if (privateMarker_ == 0)
            responses_ += "\x1b[?1;2c"; // VT100 with advanced video
        else if (privateMarker_ == '>')
            responses_ += "\x1b[>0;10;1c";
        break;
    case 't':
        break; // window manipulation: ignored
    default:
        break;
    }
    changed();
}

void TerminalScreen::setMode(bool privateMode, bool enable)
{
    if (!privateMode)
        return; // ANSI modes (insert, newline) are not used by modern shells
    for (int mode : params_) {
        switch (mode) {
        case 1: appCursorKeys_ = enable; break;
        case 6:
            originMode_ = enable;
            moveCursor(0, 0);
            break;
        case 7: autoWrap_ = enable; break;
        case 25: cursorVisible_ = enable; break;
        case 47:
        case 1047: switchScreen(enable, false); break;
        case 1049: switchScreen(enable, true); break;
        case 2004: bracketedPaste_ = enable; break;
        default: break; // mouse reporting, focus events, win32-input-mode, ...
        }
    }
}

void TerminalScreen::switchScreen(bool alternate, bool saveCursor)
{
    if (alternate == altActive_)
        return;
    if (alternate) {
        if (saveCursor)
            savedMain_ = {cursorRow_, cursorCol_, attr_, originMode_};
        altActive_ = true;
        alt_.assign(size_t(rows_), blankLine());
    } else {
        altActive_ = false;
        if (saveCursor) {
            cursorRow_ = std::clamp(savedMain_.row, 0, rows_ - 1);
            cursorCol_ = std::clamp(savedMain_.col, 0, columns_ - 1);
            attr_ = savedMain_.attr;
        }
    }
    scrollTop_ = 0;
    scrollBottom_ = rows_ - 1;
    wrapPending_ = false;
}

void TerminalScreen::sgr()
{
    if (params_.empty()) {
        attr_ = TermAttr();
        return;
    }
    for (size_t i = 0; i < params_.size(); ++i) {
        const int p = params_[i];
        auto extendedColor = [&](uint32_t& target) {
            if (i + 1 < params_.size() && params_[i + 1] == 5 && i + 2 < params_.size()) {
                target = termcolor::indexed(params_[i + 2]);
                i += 2;
            } else if (i + 1 < params_.size() && params_[i + 1] == 2 && i + 4 < params_.size()) {
                target = termcolor::rgb(params_[i + 2], params_[i + 3], params_[i + 4]);
                i += 4;
            }
        };
        if (p == 0)
            attr_ = TermAttr();
        else if (p == 1)
            attr_.flags |= kBold;
        else if (p == 2)
            attr_.flags |= kDim;
        else if (p == 3)
            attr_.flags |= kItalic;
        else if (p == 4)
            attr_.flags |= kUnderline;
        else if (p == 7)
            attr_.flags |= kInverse;
        else if (p == 8)
            attr_.flags |= kHidden;
        else if (p == 9)
            attr_.flags |= kStrike;
        else if (p == 21 || p == 22)
            attr_.flags &= uint8_t(~(kBold | kDim));
        else if (p == 23)
            attr_.flags &= uint8_t(~kItalic);
        else if (p == 24)
            attr_.flags &= uint8_t(~kUnderline);
        else if (p == 27)
            attr_.flags &= uint8_t(~kInverse);
        else if (p == 28)
            attr_.flags &= uint8_t(~kHidden);
        else if (p == 29)
            attr_.flags &= uint8_t(~kStrike);
        else if (p >= 30 && p <= 37)
            attr_.fg = termcolor::indexed(p - 30);
        else if (p == 38)
            extendedColor(attr_.fg);
        else if (p == 39)
            attr_.fg = termcolor::kDefault;
        else if (p >= 40 && p <= 47)
            attr_.bg = termcolor::indexed(p - 40);
        else if (p == 48)
            extendedColor(attr_.bg);
        else if (p == 49)
            attr_.bg = termcolor::kDefault;
        else if (p >= 90 && p <= 97)
            attr_.fg = termcolor::indexed(p - 90 + 8);
        else if (p >= 100 && p <= 107)
            attr_.bg = termcolor::indexed(p - 100 + 8);
    }
}

// ---- Scrolling and erasing ----------------------------------------------------------------------

void TerminalScreen::lineFeed()
{
    if (cursorRow_ == scrollBottom_)
        scrollUp(scrollTop_, scrollBottom_, 1);
    else if (cursorRow_ < rows_ - 1)
        ++cursorRow_;
    wrapPending_ = false;
}

void TerminalScreen::reverseIndex()
{
    if (cursorRow_ == scrollTop_)
        scrollDown(scrollTop_, scrollBottom_, 1);
    else if (cursorRow_ > 0)
        --cursorRow_;
    wrapPending_ = false;
}

void TerminalScreen::scrollUp(int top, int bottom, int count)
{
    std::vector<TermLine>& lines = screen();
    count = std::min(count, bottom - top + 1);
    for (int i = 0; i < count; ++i) {
        // Lines leaving the top of a full-height region on the main screen go to the scrollback.
        if (top == 0 && !altActive_) {
            scrollback_.push_back(std::move(lines[size_t(top)]));
            if (int(scrollback_.size()) > kMaxScrollback)
                scrollback_.pop_front();
        }
        lines.erase(lines.begin() + top);
        lines.insert(lines.begin() + bottom, blankLine());
    }
}

void TerminalScreen::scrollDown(int top, int bottom, int count)
{
    std::vector<TermLine>& lines = screen();
    count = std::min(count, bottom - top + 1);
    for (int i = 0; i < count; ++i) {
        lines.erase(lines.begin() + bottom);
        lines.insert(lines.begin() + top, blankLine());
    }
}

void TerminalScreen::clearCells(TermLine& line, int from, int to)
{
    for (int c = std::max(0, from); c < std::min(to, columns_); ++c)
        line.cells[size_t(c)] = TermCell{U' ', TermAttr{termcolor::kDefault, attr_.bg, 0}};
}

void TerminalScreen::eraseInLine(int mode)
{
    TermLine& line = screen()[size_t(cursorRow_)];
    if (mode == 0)
        clearCells(line, cursorCol_, columns_);
    else if (mode == 1)
        clearCells(line, 0, cursorCol_ + 1);
    else
        clearCells(line, 0, columns_);
    if (mode != 1)
        line.wrapped = false;
}

void TerminalScreen::eraseInDisplay(int mode)
{
    std::vector<TermLine>& lines = screen();
    if (mode == 0) {
        eraseInLine(0);
        for (int r = cursorRow_ + 1; r < rows_; ++r)
            lines[size_t(r)] = blankLine();
    } else if (mode == 1) {
        for (int r = 0; r < cursorRow_; ++r)
            lines[size_t(r)] = blankLine();
        eraseInLine(1);
    } else if (mode == 2) {
        for (TermLine& line : lines)
            line = blankLine();
    } else if (mode == 3 && !altActive_) {
        scrollback_.clear();
    }
}

// ---- Text extraction ----------------------------------------------------------------------------

std::string TerminalScreen::text(int fromLine, int fromColumn, int toLine, int toColumn) const
{
    if (toLine < fromLine || (toLine == fromLine && toColumn < fromColumn)) {
        std::swap(fromLine, toLine);
        std::swap(fromColumn, toColumn);
    }
    fromLine = std::clamp(fromLine, 0, totalLines() - 1);
    toLine = std::clamp(toLine, 0, totalLines() - 1);
    std::string out;
    for (int l = fromLine; l <= toLine; ++l) {
        const TermLine& line = lineAt(l);
        const int begin = l == fromLine ? std::clamp(fromColumn, 0, columns_) : 0;
        const int end = l == toLine ? std::clamp(toColumn, 0, columns_) : columns_;
        std::string piece;
        for (int c = begin; c < end && c < int(line.cells.size()); ++c)
            appendUtf8(piece, line.cells[size_t(c)].ch);
        const bool joinNext = line.wrapped && l < toLine;
        if (!joinNext)
            while (!piece.empty() && piece.back() == ' ')
                piece.pop_back();
        out += piece;
        if (l < toLine && !joinNext)
            out += '\n';
    }
    return out;
}

// ---- Keyboard -----------------------------------------------------------------------------------

std::string encodeKey(TermKey key, TermMods mods, bool applicationCursorKeys)
{
    const int modifier = 1 + (mods.shift ? 1 : 0) + (mods.alt ? 2 : 0) + (mods.ctrl ? 4 : 0);
    const bool modified = modifier > 1;
    auto cursor = [&](char letter) {
        if (modified)
            return std::string("\x1b[1;") + std::to_string(modifier) + letter;
        return std::string(applicationCursorKeys ? "\x1bO" : "\x1b[") + letter;
    };
    auto tilde = [&](int number) {
        return "\x1b[" + std::to_string(number) + (modified ? ";" + std::to_string(modifier) : std::string()) + "~";
    };
    auto function = [&](char letter) { // F1-F4
        return modified ? std::string("\x1b[1;") + std::to_string(modifier) + letter : std::string("\x1bO") + letter;
    };
    switch (key) {
    case TermKey::Enter: return mods.alt ? "\x1b\r" : "\r";
    case TermKey::Backspace: return mods.ctrl ? "\x08" : (mods.alt ? "\x1b\x7f" : "\x7f");
    case TermKey::Tab: return mods.shift ? "\x1b[Z" : "\t";
    case TermKey::Escape: return "\x1b";
    case TermKey::Up: return cursor('A');
    case TermKey::Down: return cursor('B');
    case TermKey::Right: return cursor('C');
    case TermKey::Left: return cursor('D');
    case TermKey::Home: return cursor('H');
    case TermKey::End: return cursor('F');
    case TermKey::Insert: return tilde(2);
    case TermKey::Delete: return tilde(3);
    case TermKey::PageUp: return tilde(5);
    case TermKey::PageDown: return tilde(6);
    case TermKey::F1: return function('P');
    case TermKey::F2: return function('Q');
    case TermKey::F3: return function('R');
    case TermKey::F4: return function('S');
    case TermKey::F5: return tilde(15);
    case TermKey::F6: return tilde(17);
    case TermKey::F7: return tilde(18);
    case TermKey::F8: return tilde(19);
    case TermKey::F9: return tilde(20);
    case TermKey::F10: return tilde(21);
    case TermKey::F11: return tilde(23);
    case TermKey::F12: return tilde(24);
    }
    return {};
}

std::string encodeControl(char c)
{
    if (c >= 'a' && c <= 'z')
        return std::string(1, char(c - 'a' + 1));
    if (c >= 'A' && c <= 'Z')
        return std::string(1, char(c - 'A' + 1));
    switch (c) {
    case ' ':
    case '@':
    case '2': return std::string(1, '\0');
    case '[':
    case '3': return "\x1b";
    case '\\':
    case '4': return "\x1c";
    case ']':
    case '5': return "\x1d";
    case '^':
    case '6': return "\x1e";
    case '_':
    case '-':
    case '7': return "\x1f";
    default: return {};
    }
}

std::string encodeText(std::string_view utf8, bool alt)
{
    return alt ? "\x1b" + std::string(utf8) : std::string(utf8);
}

std::string encodePaste(std::string_view text, bool bracketed)
{
    std::string normalized;
    normalized.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
            continue; // CRLF -> CR, like a typed Enter
        normalized += text[i] == '\n' ? '\r' : text[i];
    }
    return bracketed ? "\x1b[200~" + normalized + "\x1b[201~" : normalized;
}

} // namespace enp::model
