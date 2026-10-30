// Icon glyphs and ribbon tints for commands.
#pragma once

#include "enp/view/Theme.h"
#include "enp/viewmodel/CommandId.h"

namespace enp::view {

// Font Awesome glyph (UTF-8) for a command.
const char* commandIcon(viewmodel::CommandId id);
// Ribbon tint for a command, picked from the palette's icon colours.
ImU32 commandTint(viewmodel::CommandId id, const Palette& palette);

} // namespace enp::view
