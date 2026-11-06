#include "enp/view/CodeGrid.h"

#include <algorithm>
#include <cmath>

namespace enp::view {

namespace {

// The weight of a line from the cell's centre to one of its edges.
enum Weight : uint8_t { N = 0, L = 1, H = 2, D = 3 }; // none, light, heavy, double
enum Arm { Up, Right, Down, Left };

constexpr uint8_t arms(int up, int right, int down, int left)
{
    return uint8_t(up | right << 2 | down << 4 | left << 6);
}

// U+2500-U+257F: the weight of each arm (up, right, down, left). The rounded corners are listed
// by their arms and drawn as arcs; the diagonals (U+2571-U+2573) have none.
constexpr uint8_t kArms[128] = {
    arms(N, L, N, L), arms(N, H, N, H), arms(L, N, L, N), arms(H, N, H, N), // ─ ━ │ ┃
    arms(N, L, N, L), arms(N, H, N, H), arms(L, N, L, N), arms(H, N, H, N), // ┄ ┅ ┆ ┇
    arms(N, L, N, L), arms(N, H, N, H), arms(L, N, L, N), arms(H, N, H, N), // ┈ ┉ ┊ ┋
    arms(N, L, L, N), arms(N, H, L, N), arms(N, L, H, N), arms(N, H, H, N), // ┌ ┍ ┎ ┏
    arms(N, N, L, L), arms(N, N, L, H), arms(N, N, H, L), arms(N, N, H, H), // ┐ ┑ ┒ ┓
    arms(L, L, N, N), arms(L, H, N, N), arms(H, L, N, N), arms(H, H, N, N), // └ ┕ ┖ ┗
    arms(L, N, N, L), arms(L, N, N, H), arms(H, N, N, L), arms(H, N, N, H), // ┘ ┙ ┚ ┛
    arms(L, L, L, N), arms(L, H, L, N), arms(H, L, L, N), arms(L, L, H, N), // ├ ┝ ┞ ┟
    arms(H, L, H, N), arms(H, H, L, N), arms(L, H, H, N), arms(H, H, H, N), // ┠ ┡ ┢ ┣
    arms(L, N, L, L), arms(L, N, L, H), arms(H, N, L, L), arms(L, N, H, L), // ┤ ┥ ┦ ┧
    arms(H, N, H, L), arms(H, N, L, H), arms(L, N, H, H), arms(H, N, H, H), // ┨ ┩ ┪ ┫
    arms(N, L, L, L), arms(N, L, L, H), arms(N, H, L, L), arms(N, H, L, H), // ┬ ┭ ┮ ┯
    arms(N, L, H, L), arms(N, L, H, H), arms(N, H, H, L), arms(N, H, H, H), // ┰ ┱ ┲ ┳
    arms(L, L, N, L), arms(L, L, N, H), arms(L, H, N, L), arms(L, H, N, H), // ┴ ┵ ┶ ┷
    arms(H, L, N, L), arms(H, L, N, H), arms(H, H, N, L), arms(H, H, N, H), // ┸ ┹ ┺ ┻
    arms(L, L, L, L), arms(L, L, L, H), arms(L, H, L, L), arms(L, H, L, H), // ┼ ┽ ┾ ┿
    arms(H, L, L, L), arms(L, L, H, L), arms(H, L, H, L), arms(H, L, L, H), // ╀ ╁ ╂ ╃
    arms(H, H, L, L), arms(L, L, H, H), arms(L, H, H, L), arms(H, H, L, H), // ╄ ╅ ╆ ╇
    arms(L, H, H, H), arms(H, L, H, H), arms(H, H, H, L), arms(H, H, H, H), // ╈ ╉ ╊ ╋
    arms(N, L, N, L), arms(N, H, N, H), arms(L, N, L, N), arms(H, N, H, N), // ╌ ╍ ╎ ╏
    arms(N, D, N, D), arms(D, N, D, N), arms(N, D, L, N), arms(N, L, D, N), // ═ ║ ╒ ╓
    arms(N, D, D, N), arms(N, N, L, D), arms(N, N, D, L), arms(N, N, D, D), // ╔ ╕ ╖ ╗
    arms(L, D, N, N), arms(D, L, N, N), arms(D, D, N, N), arms(L, N, N, D), // ╘ ╙ ╚ ╛
    arms(D, N, N, L), arms(D, N, N, D), arms(L, D, L, N), arms(D, L, D, N), // ╜ ╝ ╞ ╟
    arms(D, D, D, N), arms(L, N, L, D), arms(D, N, D, L), arms(D, N, D, D), // ╠ ╡ ╢ ╣
    arms(N, D, L, D), arms(N, L, D, L), arms(N, D, D, D), arms(L, D, N, D), // ╤ ╥ ╦ ╧
    arms(D, L, N, L), arms(D, D, N, D), arms(L, D, L, D), arms(D, L, D, L), // ╨ ╩ ╪ ╫
    arms(D, D, D, D), arms(N, L, L, N), arms(N, N, L, L), arms(L, N, N, L), // ╬ ╭ ╮ ╯
    arms(L, L, N, N), 0, 0, 0,                                              // ╰ ╱ ╲ ╳
    arms(N, N, N, L), arms(L, N, N, N), arms(N, L, N, N), arms(N, N, L, N), // ╴ ╵ ╶ ╷
    arms(N, N, N, H), arms(H, N, N, N), arms(N, H, N, N), arms(N, N, H, N), // ╸ ╹ ╺ ╻
    arms(N, H, N, L), arms(L, N, H, N), arms(N, L, N, H), arms(H, N, L, N), // ╼ ╽ ╾ ╿
};

// Dashed lines: how many dashes cross the cell.
int dashesOf(uint32_t c)
{
    if (c >= 0x2504 && c <= 0x2507)
        return 3;
    if (c >= 0x2508 && c <= 0x250B)
        return 4;
    if (c >= 0x254C && c <= 0x254F)
        return 2;
    return 0;
}

ImU32 withAlpha(ImU32 color, float alpha)
{
    const float a = float((color >> IM_COL32_A_SHIFT) & 0xFF) * alpha;
    return (color & ~IM_COL32_A_MASK) | (ImU32(a + 0.5f) << IM_COL32_A_SHIFT);
}

// Block elements U+2580-U+259F: eighths, halves, quadrants and shades.
void drawBlock(ImDrawList* dl, uint32_t c, ImVec2 a, ImVec2 b, ImU32 color)
{
    const float w = b.x - a.x, h = b.y - a.y;
    const float mx = std::floor(a.x + w * 0.5f), my = std::floor(a.y + h * 0.5f);
    auto rect = [&](float x0, float y0, float x1, float y1, ImU32 col) { dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col); };
    if (c == 0x2580)
        rect(a.x, a.y, b.x, my, color); // ▀
    else if (c >= 0x2581 && c <= 0x2588)
        rect(a.x, std::round(b.y - h * float(c - 0x2580) / 8.0f), b.x, b.y, color); // ▁ ... █: the lower eighths
    else if (c >= 0x2589 && c <= 0x258F)
        rect(a.x, a.y, std::round(a.x + w * float(0x2590 - c) / 8.0f), b.y, color); // ▉ ... ▏: the left eighths
    else if (c == 0x2590)
        rect(mx, a.y, b.x, b.y, color); // ▐
    else if (c >= 0x2591 && c <= 0x2593)
        rect(a.x, a.y, b.x, b.y, withAlpha(color, float(c - 0x2590) * 0.25f)); // ░ ▒ ▓
    else if (c == 0x2594)
        rect(a.x, a.y, b.x, std::round(a.y + h / 8.0f), color); // ▔
    else if (c == 0x2595)
        rect(std::round(b.x - w / 8.0f), a.y, b.x, b.y, color); // ▕
    else {
        // ▖ ▗ ▘ ▙ ▚ ▛ ▜ ▝ ▞ ▟: quadrants, upper left 1, upper right 2, lower left 4, lower right 8.
        static constexpr uint8_t kQuadrants[10] = {4, 8, 1, 13, 9, 7, 11, 2, 6, 14};
        const uint8_t q = kQuadrants[c - 0x2596];
        if (q & 1)
            rect(a.x, a.y, mx, my, color);
        if (q & 2)
            rect(mx, a.y, b.x, my, color);
        if (q & 4)
            rect(a.x, my, mx, b.y, color);
        if (q & 8)
            rect(mx, my, b.x, b.y, color);
    }
}

} // namespace

uint32_t decodeUtf8(const std::string& text, size_t i, int* length)
{
    const unsigned char lead = static_cast<unsigned char>(text[i]);
    const size_t n = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 0;
    *length = 1;
    if (n <= 1)
        return n == 1 ? lead : 0xFFFD;
    if (i + n > text.size())
        return 0xFFFD;
    uint32_t c = lead & (0xFF >> (n + 1));
    for (size_t k = 1; k < n; ++k) {
        const unsigned char b = static_cast<unsigned char>(text[i + k]);
        if ((b & 0xC0) != 0x80)
            return 0xFFFD;
        c = c << 6 | (b & 0x3F);
    }
    *length = int(n);
    return c;
}

bool isBoxCharacter(uint32_t c)
{
    return c >= 0x2500 && c <= 0x259F;
}

int cellColumns(uint32_t c)
{
    // East Asian wide and fullwidth characters, and the emoji blocks (as terminals count them).
    const bool wide = (c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0x303E) || (c >= 0x3041 && c <= 0x33FF) ||
                      (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xA000 && c <= 0xA4CF) ||
                      (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE4F) ||
                      (c >= 0xFF00 && c <= 0xFF60) || (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x1F300 && c <= 0x1F64F) ||
                      (c >= 0x1F900 && c <= 0x1F9FF) || (c >= 0x20000 && c <= 0x3FFFD);
    return wide ? 2 : 1;
}

bool drawBoxCharacter(ImDrawList* dl, uint32_t c, ImVec2 min, ImVec2 max, ImU32 color, float lineWidth)
{
    if (!isBoxCharacter(c))
        return false;
    // Every edge on a whole pixel: no half-covered pixel where two cells or two arms meet. The
    // cell is rounded (neighbours round their shared edge alike), the centre lines run through
    // pixel centres, and every line is an odd number of pixels wide.
    min = ImVec2(std::round(min.x), std::round(min.y));
    max = ImVec2(std::round(max.x), std::round(max.y));
    if (c >= 0x2580) {
        drawBlock(dl, c, min, max, color);
        return true;
    }
    const float t = 2.0f * std::floor(std::max(0.0f, lineWidth - 1.0f) * 0.5f + 0.5f) + 1.0f; // light: 1, 3, 5 px
    const float cx = std::floor((min.x + max.x) * 0.5f) + 0.5f, cy = std::floor((min.y + max.y) * 0.5f) + 0.5f;
    // Double lines: two light lines, this far either side of the centre line.
    const float g = std::max(t + 1.0f, std::round(std::min(max.x - min.x, max.y - min.y) * 0.18f));

    if (c >= 0x2571 && c <= 0x2573) { // ╱ ╲ ╳
        if (c != 0x2572)
            dl->AddLine(ImVec2(min.x, max.y), ImVec2(max.x, min.y), color, t);
        if (c != 0x2571)
            dl->AddLine(min, max, color, t);
        return true;
    }

    const uint8_t packed = kArms[c - 0x2500];
    const uint8_t a[4] = {uint8_t(packed & 3), uint8_t(packed >> 2 & 3), uint8_t(packed >> 4 & 3), uint8_t(packed >> 6 & 3)};

    if (c >= 0x256D && c <= 0x2570) { // ╭ ╮ ╯ ╰: a quarter circle joining the two arms
        const float r = std::floor(std::min(cx - min.x, cy - min.y));
        const bool right = a[Right] != N, down = a[Down] != N;
        const ImVec2 centre(cx + (right ? r : -r), cy + (down ? r : -r));
        // From the vertical arm's end, along the arc (angles from +x, clockwise on screen), to the
        // horizontal arm's end: ╭ pi..1.5pi, ╮ 0..-0.5pi, ╯ 0..0.5pi, ╰ pi..0.5pi.
        const float pi = 3.14159265f;
        const float from = right ? pi : 0.0f;
        const float to = down ? (right ? 1.5f * pi : -0.5f * pi) : 0.5f * pi;
        dl->PathLineTo(ImVec2(cx, down ? max.y : min.y));
        dl->PathArcTo(centre, r, from, to, 8);
        dl->PathLineTo(ImVec2(right ? max.x : min.x, cy));
        dl->PathStroke(color, t);
        return true;
    }

    const int dashes = dashesOf(c);
    auto widthOf = [&](uint8_t w) { return w == H ? 2.0f * t + 1.0f : w == N ? 0.0f : t; }; // odd, like t
    // A line along arm `arm`, from `from` past the centre (negative: back over it) to the cell's
    // edge, `offset` to the side (up or left negative), `width` wide.
    auto segment = [&](int arm, float from, float offset, float width) {
        const float half = width * 0.5f;
        if (arm == Right || arm == Left) {
            const float y = cy + offset;
            const float x0 = arm == Right ? cx + from : min.x, x1 = arm == Right ? max.x : cx - from;
            dl->AddRectFilled(ImVec2(x0, y - half), ImVec2(x1, y + half), color);
        } else {
            const float x = cx + offset;
            const float y0 = arm == Down ? cy + from : min.y, y1 = arm == Down ? max.y : cy - from;
            dl->AddRectFilled(ImVec2(x - half, y0), ImVec2(x + half, y1), color);
        }
    };

    if (dashes > 0) { // ┄ ┆ ┈ ┊ ╌ ╎ and their heavy forms: one straight line, broken into dashes
        const bool horizontal = a[Right] != N;
        const float width = widthOf(horizontal ? a[Right] : a[Up]);
        const float lo = horizontal ? min.x : min.y, hi = horizontal ? max.x : max.y;
        const float period = (hi - lo) / float(dashes);
        for (int i = 0; i < dashes; ++i) {
            const float d0 = std::round(lo + period * (float(i) + 0.2f)), d1 = std::round(lo + period * (float(i) + 0.8f));
            if (horizontal)
                dl->AddRectFilled(ImVec2(d0, cy - width * 0.5f), ImVec2(d1, cy + width * 0.5f), color);
            else
                dl->AddRectFilled(ImVec2(cx - width * 0.5f, d0), ImVec2(cx + width * 0.5f, d1), color);
        }
        return true;
    }

    for (int arm = 0; arm < 4; ++arm) {
        const uint8_t w = a[arm];
        if (w == N)
            continue;
        const bool horizontal = arm == Right || arm == Left;
        // The arms across this one: before it (up or left) and after it (down or right).
        const uint8_t sideBefore = horizontal ? a[Up] : a[Left], sideAfter = horizontal ? a[Down] : a[Right];
        const bool opposite = a[(arm + 2) % 4] != N;
        const float across = std::max(sideBefore == D ? 0.0f : widthOf(sideBefore), sideAfter == D ? 0.0f : widthOf(sideAfter));
        if (w != D) {
            // Into the lines across, so the corner is filled; with none, over the centre pixel,
            // where the opposite arm (│ ─) overlaps it.
            float from = -std::max(across, widthOf(w)) * 0.5f;
            if (sideBefore == D && sideAfter == D)
                from = opposite ? -(g + t * 0.5f) : g - t * 0.5f; // ╪ crosses both lines; ╤ meets the near one
            else if (sideBefore == D || sideAfter == D)
                from = -(g + t * 0.5f); // ╒ ╞: on to the far line
            segment(arm, from, 0.0f, widthOf(w));
            continue;
        }
        for (float side : {-1.0f, 1.0f}) {
            const uint8_t same = side < 0.0f ? sideBefore : sideAfter, other = side < 0.0f ? sideAfter : sideBefore;
            float from = -std::max(across, t) * 0.5f; // ╞ ╥: up to the single line across (═: over the centre)
            if (same == D)
                from = g - t * 0.5f; // the inner line stops at the double line across (╬ ╦ ╔)
            else if (other == D)
                from = -(g + t * 0.5f); // the outer line goes round the corner (╔ ╦)
            segment(arm, from, side * g, t);
        }
    }
    return true;
}

} // namespace enp::view
