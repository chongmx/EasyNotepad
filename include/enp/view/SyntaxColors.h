// The colour of every syntax token for one language: the user's colour for that language, else
// their colour for all languages, else the theme's default.
#pragma once

#include "enp/model/Language.h"
#include "enp/view/Theme.h"
#include "enp/viewmodel/NotepadViewModel.h"

#include <array>

namespace enp::view {

using SyntaxColors = std::array<ImU32, size_t(model::Token::Count)>;

SyntaxColors resolveSyntaxColors(const Palette& palette, const viewmodel::NotepadViewModel& vm, model::Language language);

ImU32 rgbToU32(uint32_t rgb);
uint32_t u32ToRgb(ImU32 color);

} // namespace enp::view
