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
    Effects fx;
    float breath = 1.0f; // this frame's share of the glows' intensity: 1 - depth .. 1 + depth
    float pulse = 1.0f;  // strength x breath: what every glow's alpha is multiplied by
};
Look look;

// The hint's fade in, in seconds, and its width in ems (wider hints wrap).
constexpr float kHintIn = 0.14f;
constexpr float kHintWidth = 26.0f;

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
float hintWrapWidth = 0.0f; // the hint being drawn: its text width (begin())
HintArea currentArea = HintArea::Dialogs; // the innermost HintScope's; outside every scope: a dialog or floating window

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
        const float seconds = std::max(0.01f, hovered ? look.fx.fadeIn : look.fx.fadeOut);
        light->progress = std::clamp(light->progress + (hovered ? step() : -step()) / seconds, 0.0f, 1.0f);
        light->frame = frame;
        light->hovered = hovered;
    }
    const float amount = ease(light->progress);
    if (light->progress <= 0.0f && !hovered)
        light->id = 0;
    return amount;
}

// The area of a hint begun now: a menu's or a dialog's by the window it is in, else the scope's.
HintArea areaHere()
{
    if (const ImGuiWindow* window = ImGui::GetCurrentWindowRead()) {
        const ImGuiWindow* root = window->RootWindow ? window->RootWindow : window;
        if (root->Flags & ImGuiWindowFlags_Modal)
            return HintArea::Dialogs;
        if (root->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_ChildMenu))
            return HintArea::Menus;
    }
    return currentArea;
}

bool begin(ImGuiTooltipFlags flags)
{
    // Hints switched off where this one would show: none (checked before anything is set up for it).
    if (depth == 0 && (look.fx.hiddenHints >> unsigned(areaHere())) & 1u)
        return false;
    const float s = look.s;
    const ImVec2 padding = look.enhanced ? ImVec2(12.0f * s, 9.0f * s) : ImVec2(9.0f * s, 6.0f * s);
    // A hint stays inside the window it belongs to. With multi-viewports a tooltip may otherwise
    // reach over the window's edge onto the monitor, and then it becomes an OS window of its own:
    // its transparent background (the glowing hint has no box) shows black there. Pinned to the
    // window's viewport, ImGui places it beside the cursor within that window, flipping or clamping.
    const ImGuiViewport* viewport = ImGui::GetWindowViewport();
    ImGui::SetNextWindowViewport(viewport->ID);
    // Its width: about kHintWidth characters, or less when the window has less room beside the
    // cursor, so a hint near the edge wraps onto more lines instead of running past it.
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = ImGui::IsMousePosValid(&io.MousePos) ? io.MousePos : ImVec2(viewport->Pos.x, viewport->Pos.y);
    const float edge = 2.0f * padding.x + 2.0f * ImGui::GetStyle().DisplaySafeAreaPadding.x + 8.0f * s;
    const float roomRight = viewport->Pos.x + viewport->Size.x - (mouse.x + 16.0f * s) - edge; // ImGui's tooltip offset: 16 px
    const float roomLeft = mouse.x - viewport->Pos.x - edge;
    hintWrapWidth = std::clamp(std::max(roomRight, roomLeft), 8.0f * ImGui::GetFontSize(), kHintWidth * ImGui::GetFontSize());
    hintWrapWidth = std::min(hintWrapWidth, std::max(1.0f, viewport->Size.x - edge));
    // Without a box the padding is room for the glow (and keeps the text where the box had it).
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
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
    // A long hint wraps: about kHintWidth characters a line, fewer near the window's edge (a caller
    // may set its own width inside).
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + hintWrapWidth);
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

// Whether the content [first, first + count) is quads of four vertices, each an upright rectangle:
// glyphs (and plain rectangles), which copyGrown can enlarge.
bool allQuads(const ImDrawList* dl, int first, int count)
{
    if (count % 4 != 0)
        return false;
    for (int q = first; q < first + count; q += 4) {
        const ImDrawVert* v = dl->VtxBuffer.Data + q;
        if (v[0].pos.y != v[1].pos.y || v[2].pos.y != v[3].pos.y || v[0].pos.x != v[3].pos.x || v[1].pos.x != v[2].pos.x)
            return false;
    }
    return true;
}

// As copyUnder, with every quad grown by `grow` pixels on each side and its texture stretched over
// it: a glyph becomes a larger, softer version of itself (the texture is filtered), with no grain.
void copyGrown(ImDrawList* dl, int first, int count, unsigned vtxOffset, float grow, ImU32 color)
{
    dl->PrimReserve(indices.Size, count);
    const unsigned base = dl->_VtxCurrentIdx;
    const float alpha = float((color >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
    for (int q = 0; q < count; q += 4) {
        ImDrawVert quad[4];
        for (int k = 0; k < 4; ++k)
            quad[k] = dl->VtxBuffer.Data[first + q + k];
        const float cx = (quad[0].pos.x + quad[1].pos.x) * 0.5f, cy = (quad[0].pos.y + quad[2].pos.y) * 0.5f;
        for (const ImDrawVert& v : quad) {
            const float a = float((v.col >> IM_COL32_A_SHIFT) & 0xFF) * alpha;
            dl->PrimWriteVtx(ImVec2(v.pos.x + (v.pos.x < cx ? -grow : grow), v.pos.y + (v.pos.y < cy ? -grow : grow)), v.uv,
                             (color & ~IM_COL32_A_MASK) | (ImU32(a + 0.5f) << IM_COL32_A_SHIFT));
        }
    }
    for (const ImDrawIdx i : indices)
        dl->PrimWriteIdx(ImDrawIdx(base + (vtxOffset + i - unsigned(first))));
}

// The enhanced hint: the content (channel 1) a little translucent; under it (channel 0) a glow in
// the accent, no outline, in three layers. Far out, the glyphs grown larger and larger, fainter
// and fainter (smooth: each is its glyph stretched). Closer in, blurs of the glyphs: copies spread
// over a disc (a sunflower pattern, so no direction has a gap) with Gaussian weights. Together they
// keep the text's shape and fade outwards. Content that is not one draw command (an image, a
// clipped child), or too much of it, gets the normal box instead.
void glowContent(ImDrawList* dl, int first)
{
    constexpr int kInner = 16;      // copies of the close glow
    constexpr int kMaxGlow = 40;    // copies of the wide glow
    constexpr int kMinGlow = 12;    // fewer looks grainy: a box instead
    constexpr float kSpread = 6.0f; // the wide glow's radius, in pixels at 100%
    // The far glow: glyphs grown by these shares of the reach, at these strengths.
    static constexpr float kGrow[] = {0.25f, 0.46f, 0.71f, 1.0f}; // shares of the reach
    static constexpr float kGrowAlpha[] = {0.10f, 0.07f, 0.045f, 0.025f};
    constexpr int kFarLayers = int(sizeof(kGrow) / sizeof(kGrow[0]));
    const int count = dl->VtxBuffer.Size - first;
    // One draw list holds at most 64K vertices: a long hint gets fewer copies.
    const int budget = count > 0 ? 60000 / count - 1 - kInner - kFarLayers : 0;
    const int glowCopies = std::min(kMaxGlow, budget);
    const bool far = allQuads(dl, first, count);
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
        // The glow reaches past the content's clip rect (half the padding) and past the window: its
        // faint end is clipped only well away from the text.
        const ImVec2 pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
        const float reach = std::max(2.0f, look.fx.reach) * s, margin = reach;
        dl->PushClipRect(ImVec2(pos.x - margin, pos.y - margin), ImVec2(pos.x + size.x + margin, pos.y + size.y + margin), false);
        constexpr float kGolden = 2.39996323f; // radians between neighbours in a sunflower
        // Outermost first: the far glow, a wide soft blur, and a close, denser one that sets the text
        // off from what lies under it.
        if (far)
            for (int i = kFarLayers - 1; i >= 0; --i)
                copyGrown(dl, first, count, vtxOffset, kGrow[i] * reach, withAlpha(look.glow, kGrowAlpha[i] * look.pulse));
        auto blur = [&](int copies, float radius, float strength, float softness) {
            const float sig = radius * softness;
            for (int i = 0; i < copies; ++i) {
                const float r = radius * std::sqrt((float(i) + 0.5f) / float(copies));
                const float a = float(i) * kGolden;
                const float weight = std::exp(-0.5f * (r / sig) * (r / sig));
                copyUnder(dl, first, count, vtxOffset, ImVec2(std::cos(a) * r, std::sin(a) * r), withAlpha(look.glow, strength * look.pulse * weight));
            }
        };
        blur(glowCopies, spread, 0.09f, 0.42f);
        blur(kInner, 2.2f * s, 0.16f, 0.42f);
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

void configure(bool enhanced, const Effects& effects, const Palette& palette, float scale)
{
    look.enhanced = enhanced;
    look.s = scale;
    look.fx = effects;
    const ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_PopupBg];
    look.box = ImGui::GetColorU32(ImVec4(bg.x, bg.y, bg.z, 0.88f));
    look.glow = effects.color != 0 ? (effects.color | IM_COL32_A_MASK) : palette.accent;
    // Breathing: the glows swell and ebb on a slow sine.
    look.breath = 1.0f;
    if (effects.breathe && effects.breathDepth > 0.0f) {
        const double phase = ImGui::GetTime() * 6.283185307 / std::max(0.5, double(effects.breathSeconds));
        look.breath = 1.0f + std::clamp(effects.breathDepth, 0.0f, 1.0f) * float(std::sin(phase));
    }
    look.pulse = std::max(0.0f, effects.strength) * look.breath;
}

bool enhanced() { return look.enhanced; }

const char* hintAreaName(HintArea area)
{
    switch (area) {
    case HintArea::TitleBar: return "Title bar";
    case HintArea::Menus: return "Menus and lists";
    case HintArea::Ribbon: return "Ribbon";
    case HintArea::Panels: return "Side and bottom panels";
    case HintArea::Editor: return "Editor and documents";
    case HintArea::StatusBar: return "Status bar";
    case HintArea::Dialogs: return "Dialogs";
    case HintArea::Count: break;
    }
    return "";
}

HintScope::HintScope(HintArea area) : previous_(currentArea)
{
    currentArea = area;
}

HintScope::~HintScope()
{
    currentArea = previous_;
}

bool animating()
{
    const int frame = ImGui::GetFrameCount();
    if (hintFade < 1.0f && hintFrame >= frame - 1)
        return true;
    // A breathing glow on screen (a hint, or a control lit or fading) needs every frame.
    const bool breathing = look.enhanced && look.fx.breathe && look.fx.breathDepth > 0.0f;
    if (breathing && hintFrame >= frame - 1)
        return true;
    for (const Light& l : lights)
        if (l.id != 0 && l.frame >= frame - 1 && (breathing ? l.progress > 0.0f || l.hovered
                                                            : l.hovered ? l.progress < 1.0f : l.progress > 0.0f))
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
        dl->AddRect(ImVec2(min.x - g, min.y - g), ImVec2(max.x + g, max.y + g), withAlpha(look.glow, amount * look.pulse * 0.30f * t * t),
                    rounding + g, 1.0f * s);
    }
    dl->AddRectFilled(min, max, withAlpha(look.glow, amount * look.pulse * 0.10f), rounding);
    const float inset = rounding * 0.6f, mid = (min.y + max.y) * 0.5f;
    if (max.x - min.x > inset * 2.0f)
        dl->AddRectFilledMultiColor(ImVec2(min.x + inset, min.y + 1.0f * s), ImVec2(max.x - inset, mid),
                                    IM_COL32(255, 255, 255, int(amount * 22.0f)), IM_COL32(255, 255, 255, int(amount * 22.0f)),
                                    IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0));
    dl->AddRect(min, max, withAlpha(look.glow, amount * std::min(1.0f, look.pulse) * 0.85f), rounding, 1.2f * s);
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
        dl->AddText(ImVec2(pos.x + std::cos(a) * r, pos.y + std::sin(a) * r), withAlpha(look.glow, amount * look.pulse * 0.11f * weight), text);
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
    amount *= std::max(0.0f, look.fx.iconLight) * look.breath;
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
    const ImU32 color = lit(tint, amount);
    const float alpha = amount * std::max(0.0f, look.fx.iconLight) * look.breath * 0.045f;
    for (int i = 0; i < kDiscs; ++i) {
        const float r = radius * (1.0f - float(i) / float(kDiscs));
        dl->AddCircleFilled(center, r, withAlpha(color, alpha), 32);
    }
}

} // namespace enp::view::ui
