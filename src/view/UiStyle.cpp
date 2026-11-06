#include "enp/view/UiStyle.h"

#include "enp/view/Theme.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstring>

namespace enp::view::ui {

namespace {

struct Look {
    bool enhanced = false;
    float s = 1.0f;
    ImU32 box = IM_COL32(37, 41, 47, 224); // the normal hint's box
    ImU32 glow = IM_COL32(59, 130, 246, 255);
};
Look look;

// Fades, in seconds.
constexpr float kLightIn = 0.12f;
constexpr float kLightOut = 0.28f;
constexpr float kHintIn = 0.14f;
constexpr float kHintWidth = 26.0f; // in ems: wider hints wrap

// How lit a control is: a fixed set of slots (no allocation per frame), one per control that is
// lit or fading; a control gone from the screen frees its slot.
struct Light {
    ImGuiID id = 0;
    float progress = 0.0f; // 0 dark .. 1 lit, linear in time
    int frame = -1;        // last updated
    bool hovered = false;
};
constexpr int kLights = 32;
Light lights[kLights];

// The hint on screen: when it appeared, for its fade in.
double hintShownAt = 0.0;
float hintFade = 1.0f;
int hintFrame = -1;

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

float ease(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// One frame's step of a fade: at most a 30th of a second, so the first frame after the app sat
// idle does not jump to the end.
float step() { return std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 1.0f / 30.0f); }

ImU32 withAlpha(ImU32 c, float alpha)
{
    const float a = float((c >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f);
    return (c & ~IM_COL32_A_MASK) | (ImU32(a + 0.5f) << IM_COL32_A_SHIFT);
}

Light* findLight(ImGuiID id)
{
    for (Light& l : lights)
        if (l.id == id)
            return &l;
    return nullptr;
}

// The control's light this frame, moved one step towards lit (hovered) or dark.
float updateLight(ImGuiID id, bool hovered)
{
    const int frame = ImGui::GetFrameCount();
    Light* light = findLight(id);
    if (!light) {
        if (!hovered)
            return 0.0f;
        for (Light& l : lights)
            if (l.id == 0 || l.frame < frame - 2) { // free, or its control left the screen
                l = Light{id, 0.0f, -1, false};
                light = &l;
                break;
            }
        if (!light)
            return 0.0f;
    }
    if (light->frame != frame) {
        light->progress = std::clamp(light->progress + (hovered ? step() / kLightIn : -step() / kLightOut), 0.0f, 1.0f);
        light->frame = frame;
        light->hovered = hovered;
    }
    const float amount = ease(light->progress);
    if (light->progress <= 0.0f && !hovered)
        light->id = 0;
    return amount;
}

bool begin(ImGuiTooltipFlags flags)
{
    const float s = look.s;
    // Without a box the padding is room for the glow (and keeps the text where the box had it).
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, look.enhanced ? ImVec2(12.0f * s, 9.0f * s) : ImVec2(9.0f * s, 6.0f * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f * s); // a tooltip is not a popup: it takes the window's
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, look.enhanced ? IM_COL32(0, 0, 0, 0) : look.box);
    const bool shown = ImGui::BeginTooltipEx(flags, ImGuiWindowFlags_None);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    if (!shown)
        return false;
    if (depth == 0) {
        const int frame = ImGui::GetFrameCount();
        if (ImGui::IsWindowAppearing() || hintFrame < frame - 1)
            hintShownAt = ImGui::GetTime();
        hintFrame = frame;
        hintFade = ease(float(ImGui::GetTime() - hintShownAt) / kHintIn);
    }
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
    // A long hint wraps: about 55 characters a line (a caller may set its own width inside).
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kHintWidth * ImGui::GetFontSize());
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

// The enhanced hint: the content (channel 1) a little translucent; under it (channel 0) a glow in
// the accent, no outline: blurs of the glyphs, drawn as copies of them spread over a disc (a
// sunflower pattern, so no direction has a gap) with Gaussian weights, which add up to the text's
// shape fading outwards. Content that is not one draw command
// (an image, a clipped child), or too much of it, gets the normal box instead.
void glowContent(ImDrawList* dl, int first)
{
    constexpr int kInner = 16;      // copies of the close glow
    constexpr int kMaxGlow = 40;    // copies of the wide glow
    constexpr int kMinGlow = 12;    // fewer looks grainy: a box instead
    constexpr float kSpread = 6.0f; // the wide glow's radius, in pixels at 100%
    const int count = dl->VtxBuffer.Size - first;
    // One draw list holds at most 64K vertices: a long hint gets fewer copies.
    const int glowCopies = count > 0 ? std::min(kMaxGlow, 60000 / count - 1 - kInner) : 0;
    int commands = 0;
    unsigned vtxOffset = 0;
    for (const ImDrawCmd& c : dl->CmdBuffer)
        if (c.ElemCount > 0) {
            ++commands;
            vtxOffset = c.VtxOffset;
            if (c.UserCallback)
                commands = 99;
        }
    const bool glow = commands == 1 && glowCopies >= kMinGlow;
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
        const float s = look.s, spread = kSpread * s;
        // The glow reaches past the content's clip rect (half the padding): clip to the window.
        const ImVec2 pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
        dl->PushClipRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), false);
        constexpr float kGolden = 2.39996323f; // radians between neighbours in a sunflower
        // Two layers of glow in the accent, both blurs of the glyphs: a wide soft one, and a close,
        // denser one that sets the text off from what lies under it.
        auto blur = [&](int copies, float radius, float strength) {
            const float sig = radius * 0.42f;
            for (int i = 0; i < copies; ++i) {
                const float r = radius * std::sqrt((float(i) + 0.5f) / float(copies));
                const float a = float(i) * kGolden;
                const float weight = std::exp(-0.5f * (r / sig) * (r / sig));
                copyUnder(dl, first, count, vtxOffset, ImVec2(std::cos(a) * r, std::sin(a) * r), withAlpha(look.glow, strength * weight));
            }
        };
        blur(glowCopies, spread, 0.09f);
        blur(kInner, 2.2f * s, 0.16f);
        dl->PopClipRect();
    } else {
        const ImVec2 pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
        dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), look.box, 6.0f * look.s);
    }
    dl->ChannelsMerge();
    if (indices.Capacity > 1 << 16) // one huge hint does not keep its buffer
        indices.clear();
}

// The hint fading in: every vertex of its window (box, glow, text) at the fade's alpha, rising
// into place from a few pixels below.
void fadeHint(ImDrawList* dl)
{
    if (hintFade >= 1.0f)
        return;
    const float rise = (1.0f - hintFade) * 4.0f * look.s;
    for (ImDrawVert& v : dl->VtxBuffer) {
        v.col = withAlpha(v.col, hintFade);
        v.pos.y += rise;
    }
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
    look.glow = palette.accent;
}

bool enhanced() { return look.enhanced; }

bool animating()
{
    const int frame = ImGui::GetFrameCount();
    if (hintFade < 1.0f && hintFrame >= frame - 1)
        return true;
    for (const Light& l : lights)
        if (l.id != 0 && l.frame >= frame - 1 && (l.hovered ? l.progress < 1.0f : l.progress > 0.0f))
            return true;
    return false;
}

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
    ImGui::PopTextWrapPos();
    if (depth < kMaxDepth && open[depth].split) {
        ImGui::PopStyleColor();
        glowContent(ImGui::GetWindowDrawList(), open[depth].firstVertex);
    }
    if (depth == 0)
        fadeHint(ImGui::GetWindowDrawList());
    ImGui::EndTooltip();
}

void hoverGlow(ImDrawList* dl, ImGuiID id, bool hovered, ImVec2 min, ImVec2 max, float rounding)
{
    if (!look.enhanced)
        return;
    const float amount = updateLight(id, hovered);
    if (amount <= 0.0f)
        return;
    const float s = look.s;
    // A glow fading outwards, one pixel at a time (eased: no bands), a faint fill with a sheen on its
    // upper half, and a lit edge: a highlighted button in a game's menu.
    constexpr int kRings = 8;
    for (int i = kRings; i >= 1; --i) {
        const float g = float(i) * s;
        const float t = 1.0f - (float(i) - 0.5f) / float(kRings);
        dl->AddRect(ImVec2(min.x - g, min.y - g), ImVec2(max.x + g, max.y + g), withAlpha(look.glow, amount * 0.30f * t * t),
                    rounding + g, 1.0f * s);
    }
    dl->AddRectFilled(min, max, withAlpha(look.glow, amount * 0.10f), rounding);
    const float inset = rounding * 0.6f, mid = (min.y + max.y) * 0.5f;
    if (max.x - min.x > inset * 2.0f)
        dl->AddRectFilledMultiColor(ImVec2(min.x + inset, min.y + 1.0f * s), ImVec2(max.x - inset, mid),
                                    IM_COL32(255, 255, 255, int(amount * 22.0f)), IM_COL32(255, 255, 255, int(amount * 22.0f)),
                                    IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0));
    dl->AddRect(min, max, withAlpha(look.glow, amount * 0.85f), rounding, 1.2f * s);
}

float hoverLight(ImGuiID id, bool hovered) { return look.enhanced ? updateLight(id, hovered) : 0.0f; }

void textGlow(ImDrawList* dl, ImVec2 pos, const char* text, float amount)
{
    if (amount <= 0.0f)
        return;
    // As the hint's glow: copies over a disc with Gaussian weights, fewer (a label is short).
    constexpr int kCopies = 24;
    constexpr float kGolden = 2.39996323f;
    const float spread = 5.0f * look.s, sigma = spread * 0.45f;
    for (int i = 0; i < kCopies; ++i) {
        const float r = spread * std::sqrt((float(i) + 0.5f) / float(kCopies));
        const float a = float(i) * kGolden;
        const float weight = std::exp(-0.5f * (r / sigma) * (r / sigma));
        dl->AddText(ImVec2(pos.x + std::cos(a) * r, pos.y + std::sin(a) * r), withAlpha(look.glow, amount * 0.11f * weight), text);
    }
}

float light(ImGuiID id)
{
    if (!look.enhanced)
        return 0.0f;
    const Light* l = findLight(id);
    return l ? ease(l->progress) : 0.0f;
}

ImU32 lit(ImU32 tint, float amount)
{
    if (amount <= 0.0f)
        return tint;
    const ImVec4 c = ImGui::ColorConvertU32ToFloat4(tint);
    float h, s, v;
    ImGui::ColorConvertRGBtoHSV(c.x, c.y, c.z, h, s, v);
    s = std::min(1.0f, s * (1.0f + 0.35f * amount) + 0.08f * amount);
    v = std::min(1.0f, v + 0.14f * amount);
    ImVec4 out(0.0f, 0.0f, 0.0f, c.w);
    ImGui::ColorConvertHSVtoRGB(h, s, v, out.x, out.y, out.z);
    return ImGui::ColorConvertFloat4ToU32(out);
}

void iconLight(ImDrawList* dl, ImVec2 center, float radius, ImU32 tint, float amount)
{
    if (amount <= 0.0f)
        return;
    // Discs of the icon's colour, smaller and smaller: their overlaps make a light that is brightest
    // in the middle and fades to nothing at `radius`.
    constexpr int kDiscs = 8;
    for (int i = 0; i < kDiscs; ++i) {
        const float r = radius * (1.0f - float(i) / float(kDiscs));
        dl->AddCircleFilled(center, r, withAlpha(lit(tint, amount), amount * 0.045f), 32);
    }
}

} // namespace enp::view::ui
