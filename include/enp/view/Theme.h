// Colour palettes for the chrome (title bar, ribbon, panels) and the editor.
#pragma once

#include "enp/model/Language.h"

#include "imgui.h"

#include <cstdint>

namespace enp::view {

struct Palette {
    // Chrome
    ImU32 titleBarBg;
    ImU32 ribbonBg;
    ImU32 panelBg;
    ImU32 panelHeaderBg;
    ImU32 border;
    ImU32 text;
    ImU32 textDim;
    ImU32 textFaint;
    ImU32 accent;
    ImU32 accentSoft;   // selected tab / checked button background
    ImU32 hover;
    ImU32 tabActiveBg;
    ImU32 statusBarBg;
    ImU32 modeAgent;    // badge colours for the workspace modes
    ImU32 modeCode;

    // Ribbon icon tints
    ImU32 iconBlue;
    ImU32 iconTeal;
    ImU32 iconGreen;
    ImU32 iconOrange;
    ImU32 iconPurple;
    ImU32 iconRed;
    ImU32 iconGray;

    // Editor
    ImU32 editorBg;
    ImU32 gutterBg;
    ImU32 gutterText;
    ImU32 gutterTextCurrent;
    ImU32 currentLine;
    ImU32 selection;
    ImU32 caret;
    ImU32 findMatch;
    ImU32 findMatchCurrent;
    ImU32 whitespace;
    ImU32 indentGuide;
    ImU32 bookmark;

    // Syntax
    ImU32 synText;
    ImU32 synKeyword;
    ImU32 synType;
    ImU32 synString;
    ImU32 synNumber;
    ImU32 synComment;
    ImU32 synPreprocessor;
    ImU32 synHeading;
    ImU32 syntax[size_t(model::Token::Count)]; // default colour of every token class

    // Review / diff
    ImU32 severityError;
    ImU32 severityWarning;
    ImU32 severityInfo;
    ImU32 diffAddedBg;
    ImU32 diffRemovedBg;
    ImU32 diffChangedBg;
    ImU32 diffAddedText;
    ImU32 diffRemovedText;

    // Agent chat
    ImU32 bubbleUser;
    ImU32 bubbleAgent;
    ImU32 bubbleTool;

    // Terminal: the 16 ANSI colours as 0xRRGGBB (VS Code's defaults).
    uint32_t ansi[16];

    ImVec4 clearColor;
};

// Applies ImGui style colours for the theme and returns the matching palette.
Palette applyTheme(bool dark);

} // namespace enp::view
