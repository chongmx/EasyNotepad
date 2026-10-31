#include "enp/view/SyntaxColors.h"

namespace enp::view {

ImU32 rgbToU32(uint32_t rgb)
{
    return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 255);
}

uint32_t u32ToRgb(ImU32 color)
{
    const uint32_t r = (color >> IM_COL32_R_SHIFT) & 0xFF, g = (color >> IM_COL32_G_SHIFT) & 0xFF,
                   b = (color >> IM_COL32_B_SHIFT) & 0xFF;
    return (r << 16) | (g << 8) | b;
}

SyntaxColors resolveSyntaxColors(const Palette& palette, const viewmodel::NotepadViewModel& vm, model::Language language)
{
    SyntaxColors colors;
    const bool dark = vm.darkTheme.get();
    const std::string key = model::languageSpec(language).key;
    for (size_t i = 0; i < colors.size(); ++i) {
        const model::Token token = model::Token(i);
        if (const auto own = vm.syntaxColor(dark, key, token))
            colors[i] = rgbToU32(*own);
        else if (const auto all = vm.syntaxColor(dark, viewmodel::NotepadViewModel::kAllLanguages, token))
            colors[i] = rgbToU32(*all);
        else
            colors[i] = palette.syntax[i];
    }
    return colors;
}

} // namespace enp::view
