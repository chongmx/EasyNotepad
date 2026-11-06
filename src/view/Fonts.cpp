#include "enp/view/Fonts.h"

#include "enp/os/Os.h"

#include "IconsFontAwesome6.h"

#include <cstring>

namespace enp::view {

namespace {

// ImGui takes ownership of TTF data and frees it with IM_FREE, so copy into IM_ALLOC'd memory.
ImFont* addFont(const char* asset, float size, const ImFontConfig& config, const ImWchar* ranges = nullptr)
{
    std::optional<std::string> data = os::fs::readAsset(asset);
    if (!data || data->empty())
        return nullptr;
    void* buffer = IM_ALLOC(data->size());
    std::memcpy(buffer, data->data(), data->size());
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(buffer, int(data->size()), size, &config, ranges);
}

// Glyphs the bundled fonts lack (terminal programs draw with box drawing, block elements, arrows,
// geometric shapes and braille: htop's sort arrow, btop's graphs) come from a system font when
// one is installed. With dynamic fonts only the glyphs actually used are rasterized.
// `monospace` (the code font): on Windows Consolas is merged before Segoe UI Symbol, so arrows and
// the like come one cell wide (Segoe's are proportional, its arrows 1.4 cells); Segoe still gives
// what Consolas lacks (braille, geometric shapes). Elsewhere the first candidate is monospace.
void mergeSymbolFallback(float size, bool monospace)
{
    static const char* const kCandidates[] = {
#if defined(_WIN32)
        "C:/Windows/Fonts/seguisym.ttf", "C:/Windows/Fonts/consola.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/Menlo.ttc", "/System/Library/Fonts/Apple Symbols.ttf",
#else
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
        "/usr/share/fonts/dejavu/DejaVuSansMono.ttf", "/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf",
        "/usr/share/fonts/truetype/freefont/FreeMono.ttf",
#endif
    };
    auto merge = [size](const char* path) {
        std::optional<std::string> data = os::fs::readFile(path);
        if (!data || data->empty())
            return false;
        void* buffer = IM_ALLOC(data->size());
        std::memcpy(buffer, data->data(), data->size());
        ImFontConfig config;
        config.MergeMode = true;
        ImGui::GetIO().Fonts->AddFontFromMemoryTTF(buffer, int(data->size()), size, &config);
        return true;
    };
#if defined(_WIN32)
    if (monospace) { // a merged font gives the glyphs that the fonts before it lack
        merge(kCandidates[1]);
        merge(kCandidates[0]);
        return;
    }
#else
    (void)monospace;
#endif
    for (const char* path : kCandidates)
        if (merge(path))
            return;
}

bool mergeIcons(float size)
{
    static const ImWchar iconRanges[] = {ICON_MIN_FA, ICON_MAX_16_FA, 0};
    ImFontConfig iconConfig;
    iconConfig.MergeMode = true;
    iconConfig.PixelSnapH = true;
    iconConfig.GlyphMinAdvanceX = size; // fixed-width icons align nicely in buttons
    return addFont("fonts/" FONT_ICON_FILE_NAME_FAS, size, iconConfig, iconRanges) != nullptr;
}

} // namespace

Fonts Fonts::load()
{
    Fonts fonts;
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;

    ImFontConfig textConfig;
    textConfig.OversampleH = 2;
    fonts.ui = addFont("fonts/Roboto-Medium.ttf", kUiSize, textConfig);
    if (!fonts.ui)
        fonts.ui = atlas->AddFontDefaultVector();
    fonts.hasIcons = mergeIcons(kUiSize);
    mergeSymbolFallback(kUiSize, false); // ∑ ∫ √ and other math glyphs Roboto lacks (rendered Markdown)

    fonts.mono = addFont("fonts/Cousine-Regular.ttf", kEditorSize, ImFontConfig());
    if (fonts.mono) {
        mergeIcons(kEditorSize);
        mergeSymbolFallback(kEditorSize, true);
    } else {
        fonts.mono = fonts.ui;
    }

    return fonts;
}

} // namespace enp::view
