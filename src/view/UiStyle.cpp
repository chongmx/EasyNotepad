#include "enp/view/UiStyle.h"

#include "enp/view/Theme.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cstdarg>
#include <cstring>

namespace enp::view::ui {

namespace {

struct Look {
    bool enhanced = false;
    float s = 1.0f;
    ImU32 box = IM_COL32(37, 41, 47, 224); // the normal hint's box
    ImU32 halo = IM_COL32(8, 10, 14, 255); // the enhanced hint's outline: against the text's colour
    ImU32 glow = IM_COL32(59, 130, 246, 255);
};
Look look;

// An open tooltip: where its content starts in its window's draw list, and whether the content
// went to a channel of its own (the enhanced look draws the outline under it).
struct Open {
    int firstVertex = 0;
    bool split = false;
};
constexpr int kMaxDepth = 4; // tooltips do not nest; a margin
Open open[kMaxDepth];
int depth = 0;
ImVector<ImDrawIdx> indices; // the content's, copied while it is drawn (kept: no allocation per frame)

ImU32 withAlpha(ImU32 c, float alpha)
{
    const float a = float((c >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f);
    return (c & ~IM_COL32_A_MASK) | (ImU32(a + 0.5f) << IM_COL32_A_SHIFT);
}

float luminance(ImU32 c)
{
    const float r = float((c >> IM_COL32_R_SHIFT) & 0xFF), g = float((c >> IM_COL32_G_SHIFT) & 0xFF),
                b = float((c >> IM_COL32_B_SHIFT) & 0xFF);
    return (0.2126f * r + 0.7152f * g + 0.0722f * b) / 255.0f;
}

bool begin(ImGuiTooltipFlags flags)
{
    const float s = look.s;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(9.0f * s, 6.0f * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f * s); // a tooltip is not a popup: it takes the window's
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, look.enhanced ? IM_COL32(0, 0, 0, 0) : look.box);
    const bool shown = ImGui::BeginTooltipEx(flags, ImGuiWindowFlags_None);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    if (!shown)
        return false;
    Open o;
    if (look.enhanced && depth == 0) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        o.firstVertex = dl->VtxBuffer.Size;
        dl->ChannelsSplit(2);
        dl->ChannelsSetCurrent(1);
        o.split = true;
        // Dim text halfway to the text's colour: on an outline, without a box, it would fade away.
        const ImVec4 text = ImGui::GetStyle().Colors[ImGuiCol_Text], dim = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
        ImGui::PushStyleColor(ImGuiCol_TextDisabled,
                              ImVec4((text.x + dim.x) * 0.5f, (text.y + dim.y) * 0.5f, (text.z + dim.z) * 0.5f, dim.w));
    }
    if (depth < kMaxDepth)
        open[depth] = o;
    ++depth;
    return true;
}

// Copies of the content's vertices [first, first + count), moved by `offset` and in `color` (each
// keeps its own alpha share), with the content's indices: drawn under the content.
void copyUnder(ImDrawList* dl, int first, int count, unsigned vtxOffset, ImVec2 offset, ImU32 color)
{
    dl->PrimReserve(indices.Size, count);
    const unsigned base = dl->_VtxCurrentIdx;
    const float alpha = float((color >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
    for (int k = 0; k < count; ++k) {
        const ImDrawVert v = dl->VtxBuffer.Data[first + k];
        const float a = float((v.col >> IM_COL32_A_SHIFT) & 0xFF) * alpha;
        dl->PrimWriteVtx(ImVec2(v.pos.x + offset.x, v.pos.y + offset.y), v.uv,
                         (color & ~IM_COL32_A_MASK) | (ImU32(a + 0.5f) << IM_COL32_A_SHIFT));
    }
    for (const ImDrawIdx i : indices)
        dl->PrimWriteIdx(ImDrawIdx(base + (vtxOffset + i - unsigned(first))));
}

// The enhanced hint: the content (channel 1) a little translucent, with an outline in the halo
// colour and a glow in the accent under it (channel 0), drawn from copies of its own glyphs. Content
// that is not one draw command (an image, a clipped child) gets the normal box instead.
void glowContent(ImDrawList* dl, int first)
{
    constexpr int kCopies = 24; // three rings of eight
    const int count = dl->VtxBuffer.Size - first;
    int commands = 0;
    unsigned vtxOffset = 0;
    for (const ImDrawCmd& c : dl->CmdBuffer)
        if (c.ElemCount > 0) {
            ++commands;
            vtxOffset = c.VtxOffset;
            if (c.UserCallback)
                commands = 99;
        }
    const bool glow = commands == 1 && count > 0 && count * kCopies < 60000;
    if (glow) {
        indices.resize(dl->IdxBuffer.Size);
        std::memcpy(indices.Data, dl->IdxBuffer.Data, size_t(dl->IdxBuffer.Size) * sizeof(ImDrawIdx));
        for (int k = 0; k < count; ++k) {
            ImDrawVert& v = dl->VtxBuffer.Data[first + k];
            v.col = withAlpha(v.col, 0.94f);
        }
    }
    dl->ChannelsSetCurrent(0);
    if (glow) {
        static const ImVec2 kDirections[8] = {{1.0f, 0.0f},  {0.7071f, 0.7071f},   {0.0f, 1.0f},  {-0.7071f, 0.7071f},
                                              {-1.0f, 0.0f}, {-0.7071f, -0.7071f}, {0.0f, -1.0f}, {0.7071f, -0.7071f}};
        const float s = look.s;
        // Outermost first: the glow, wide and faint; a soft outline that hides what is under the
        // text; the outline, close and strong.
        struct Ring {
            float radius;
            ImU32 color;
        };
        const Ring rings[] = {{3.2f, withAlpha(look.glow, 0.15f)}, {2.0f, withAlpha(look.halo, 0.35f)}, {1.0f, withAlpha(look.halo, 0.7f)}};
        for (const Ring& ring : rings)
            for (const ImVec2& d : kDirections)
                copyUnder(dl, first, count, vtxOffset, ImVec2(d.x * ring.radius * s, d.y * ring.radius * s), ring.color);
    } else {
        const ImVec2 pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
        dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), look.box, 6.0f * look.s);
    }
    dl->ChannelsMerge();
    if (indices.Capacity > 1 << 16) // one huge hint does not keep its buffer
        indices.clear();
}

void textTooltip(ImGuiTooltipFlags flags, const char* fmt, va_list args)
{
    if (!begin(flags))
        return;
    ImGui::TextV(fmt, args);
    endTooltip();
}

} // namespace

void configure(bool enhanced, const Palette& palette, float scale)
{
    look.enhanced = enhanced;
    look.s = scale;
    const ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_PopupBg];
    look.box = ImGui::GetColorU32(ImVec4(bg.x, bg.y, bg.z, 0.88f));
    // Light text gets a dark outline, dark text a light one.
    look.halo = luminance(palette.text) > 0.5f ? IM_COL32(6, 8, 12, 255) : IM_COL32(255, 255, 255, 255);
    look.glow = palette.accent;
}

bool enhanced() { return look.enhanced; }

void tooltip(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    textTooltip(ImGuiTooltipFlags_OverridePrevious, fmt, args);
    va_end(args);
}

void itemTooltip(const char* fmt, ...)
{
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        return;
    va_list args;
    va_start(args, fmt);
    textTooltip(ImGuiTooltipFlags_OverridePrevious, fmt, args);
    va_end(args);
}

bool beginTooltip() { return begin(ImGuiTooltipFlags_None); }

bool beginItemTooltip()
{
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        return false;
    return begin(ImGuiTooltipFlags_None);
}

void endTooltip()
{
    depth = std::max(0, depth - 1);
    if (depth < kMaxDepth && open[depth].split) {
        ImGui::PopStyleColor();
        glowContent(ImGui::GetWindowDrawList(), open[depth].firstVertex);
    }
    ImGui::EndTooltip();
}

void hoverGlow(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding)
{
    if (!look.enhanced)
        return;
    const float s = look.s;
    // Rings fading outwards, then a lit edge and a faint fill: a highlighted button in a game's menu.
    for (int i = 4; i >= 1; --i) {
        const float g = float(i) * 1.6f * s;
        dl->AddRect(ImVec2(min.x - g, min.y - g), ImVec2(max.x + g, max.y + g), withAlpha(look.glow, 0.07f * float(5 - i)),
                    rounding + g, 2.0f * s);
    }
    dl->AddRectFilled(min, max, withAlpha(look.glow, 0.10f), rounding);
    dl->AddRect(min, max, withAlpha(look.glow, 0.85f), rounding, 1.2f * s);
}

} // namespace enp::view::ui
