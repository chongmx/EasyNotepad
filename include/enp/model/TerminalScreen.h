// Terminal emulator state: an xterm-compatible VT parser driving a character grid with scrollback.
// Supports what shells, ConPTY and common full-screen programs use: UTF-8 text, cursor movement,
// erase/insert/delete, scroll regions, 16/256/true colour SGR, the alternate screen, DEC private
// modes, OSC titles, and replies to status queries (DSR/DA). Characters are one cell wide.
#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace enp::model {

// Colour encoding: 0 = default, kIndexed | n (0-255 palette), kRgb | 0xRRGGBB.
namespace termcolor {
inline constexpr uint32_t kDefault = 0;
inline constexpr uint32_t kIndexed = 0x01000000u;
inline constexpr uint32_t kRgb = 0x02000000u;
inline constexpr uint32_t kTypeMask = 0xFF000000u;
inline uint32_t indexed(int n) { return kIndexed | uint32_t(n & 0xFF); }
inline uint32_t rgb(int r, int g, int b) { return kRgb | (uint32_t(r & 0xFF) << 16) | (uint32_t(g & 0xFF) << 8) | uint32_t(b & 0xFF); }
// 0xRRGGBB for an xterm palette index (16 base colours supplied by the caller's theme).
uint32_t paletteRgb(int index, const uint32_t base16[16]);
} // namespace termcolor

enum TermFlags : uint8_t {
    kBold = 1,
    kDim = 2,
    kItalic = 4,
    kUnderline = 8,
    kInverse = 16,
    kHidden = 32,
    kStrike = 64,
};

struct TermAttr {
    uint32_t fg = termcolor::kDefault;
    uint32_t bg = termcolor::kDefault;
    uint8_t flags = 0;
    bool operator==(const TermAttr& o) const { return fg == o.fg && bg == o.bg && flags == o.flags; }
    bool operator!=(const TermAttr& o) const { return !(*this == o); }
};

struct TermCell {
    char32_t ch = U' ';
    TermAttr attr;
};

struct TermLine {
    std::vector<TermCell> cells;
    bool wrapped = false; // continues on the next line (soft wrap), for copying
};

class TerminalScreen {
public:
    static constexpr int kMaxScrollback = 5000;

    TerminalScreen(int columns = 80, int rows = 24);

    void feed(std::string_view bytes);
    void resize(int columns, int rows);
    void reset();
    // "Clear terminal": drops the scrollback and screen but keeps the cursor's line (the prompt),
    // moved to the top.
    void clearKeepingCursorLine();

    int columns() const { return columns_; }
    int rows() const { return rows_; }
    // Scrollback lines (oldest first) followed by the visible rows.
    int totalLines() const { return int(scrollback_.size()) + rows_; }
    int scrollbackSize() const { return int(scrollback_.size()); }
    const TermLine& lineAt(int index) const;
    const TermLine& row(int index) const { return screen()[size_t(index)]; }

    int cursorRow() const { return cursorRow_; }
    int cursorColumn() const { return std::min(cursorCol_, columns_ - 1); }
    bool cursorVisible() const { return cursorVisible_; }
    bool applicationCursorKeys() const { return appCursorKeys_; }
    bool bracketedPaste() const { return bracketedPaste_; }
    bool alternateScreen() const { return altActive_; }
    const std::string& title() const { return title_; }
    // Bumped whenever the content changes (lets views skip work).
    uint64_t revision() const { return revision_; }

    // Bytes the terminal must send back to the process (answers to DSR/DA queries).
    std::string takeResponses();

    // Text between two positions in totalLines() coordinates, end exclusive. Trailing blanks of
    // each line are dropped; soft-wrapped lines are joined without a line break.
    std::string text(int fromLine, int fromColumn, int toLine, int toColumn) const;

private:
    enum class State { Ground, Escape, EscapeIntermediate, Csi, Osc, OscEscape, IgnoreString, IgnoreStringEscape, Charset };

    std::vector<TermLine>& screen() { return altActive_ ? alt_ : main_; }
    const std::vector<TermLine>& screen() const { return altActive_ ? alt_ : main_; }
    TermLine blankLine() const;

    void print(char32_t ch);
    void control(char c);
    void escape(char c);
    void csi(char final);
    void osc();
    void sgr();
    void setMode(bool privateMode, bool enable);

    void lineFeed();
    void reverseIndex();
    void scrollUp(int top, int bottom, int count);
    void scrollDown(int top, int bottom, int count);
    void eraseInLine(int mode);
    void eraseInDisplay(int mode);
    void clearCells(TermLine& line, int from, int to);
    void moveCursor(int row, int column);
    void switchScreen(bool alternate, bool saveCursor);
    int param(size_t index, int fallback) const;
    void changed() { ++revision_; }

    int columns_;
    int rows_;
    std::vector<TermLine> main_;
    std::vector<TermLine> alt_;
    std::deque<TermLine> scrollback_;
    bool altActive_ = false;

    int cursorRow_ = 0;
    int cursorCol_ = 0;
    bool wrapPending_ = false;
    TermAttr attr_;
    int scrollTop_ = 0;
    int scrollBottom_ = 0;
    bool autoWrap_ = true;
    bool cursorVisible_ = true;
    bool appCursorKeys_ = false;
    bool bracketedPaste_ = false;
    bool originMode_ = false;

    struct Saved {
        int row = 0, col = 0;
        TermAttr attr;
        bool originMode = false;
    } saved_, savedMain_;

    State state_ = State::Ground;
    std::vector<int> params_;
    bool paramStarted_ = false;
    char privateMarker_ = 0;
    std::string intermediates_;
    std::string oscBuffer_;
    char32_t utf8Codepoint_ = 0;
    int utf8Remaining_ = 0;

    std::string title_;
    std::string responses_;
    uint64_t revision_ = 0;
};

// ---- Keyboard ---------------------------------------------------------------------------------

enum class TermKey {
    Enter, Backspace, Tab, Escape, Up, Down, Left, Right, Home, End, PageUp, PageDown, Insert, Delete,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
};

struct TermMods {
    bool shift = false;
    bool alt = false;
    bool ctrl = false;
};

// Byte sequence an xterm sends for a key.
std::string encodeKey(TermKey key, TermMods mods, bool applicationCursorKeys);
// Ctrl+<character> as a C0 control code ("\x03" for Ctrl+C); empty when there is none.
std::string encodeControl(char c);
// Text typed with Alt held is prefixed with ESC.
std::string encodeText(std::string_view utf8, bool alt);
// Wraps pasted text in bracketed-paste markers when the program asked for them.
std::string encodePaste(std::string_view text, bool bracketed);

} // namespace enp::model
