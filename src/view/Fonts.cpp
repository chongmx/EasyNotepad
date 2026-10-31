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
void mergeSymbolFallback(float size)
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
    for (const char* path : kCandidates) {
        std::optional<std::string> data = os::fs::readFile(path);
        if (!data || data->empty())
            continue;
        void* buffer = IM_ALLOC(data->size());
        std::memcpy(buffer, data->data(), data->size());
        ImFontConfig config;
        config.MergeMode = true;
        ImGui::GetIO().Fonts->AddFontFromMemoryTTF(buffer, int(data->size()), size, &config);
        return;
    }
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

    fonts.mono = addFont("fonts/Cousine-Regular.ttf", kEditorSize, ImFontConfig());
    if (fonts.mono) {
        mergeIcons(kEditorSize);
        mergeSymbolFallback(kEditorSize);
    } else {
        fonts.mono = fonts.ui;
    }

    return fonts;
}

} // namespace enp::view
