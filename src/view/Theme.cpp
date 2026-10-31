#include "enp/view/Theme.h"

#include <algorithm>
#include <iterator>

namespace enp::view {

namespace {

ImU32 rgba(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
ImVec4 vec(int r, int g, int b, int a = 255) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f); }

} // namespace

Palette applyTheme(bool dark)
{
    ImGuiStyle& style = ImGui::GetStyle();
    Palette p{};
    if (dark) {
        ImGui::StyleColorsDark(&style);
        ImVec4* c = style.Colors;
        c[ImGuiCol_Text] = vec(222, 226, 231);
        c[ImGuiCol_TextDisabled] = vec(124, 132, 142);
        c[ImGuiCol_WindowBg] = vec(30, 33, 38);
        c[ImGuiCol_ChildBg] = vec(0, 0, 0, 0);
        c[ImGuiCol_PopupBg] = vec(37, 41, 47, 252);
        c[ImGuiCol_Border] = vec(52, 57, 65);
        c[ImGuiCol_FrameBg] = vec(22, 25, 29);
        c[ImGuiCol_FrameBgHovered] = vec(44, 49, 56);
        c[ImGuiCol_FrameBgActive] = vec(50, 56, 64);
        c[ImGuiCol_Button] = vec(44, 49, 56);
        c[ImGuiCol_ButtonHovered] = vec(55, 61, 70);
        c[ImGuiCol_ButtonActive] = vec(59, 130, 246, 170);
        c[ImGuiCol_Header] = vec(59, 130, 246, 60);
        c[ImGuiCol_HeaderHovered] = vec(255, 255, 255, 18);
        c[ImGuiCol_HeaderActive] = vec(59, 130, 246, 110);
        c[ImGuiCol_CheckMark] = vec(96, 165, 250);
        c[ImGuiCol_SliderGrab] = vec(96, 165, 250);
        c[ImGuiCol_Separator] = vec(52, 57, 65);
        c[ImGuiCol_Tab] = vec(30, 33, 38);
        c[ImGuiCol_TabHovered] = vec(55, 61, 70);
        c[ImGuiCol_TabSelected] = vec(44, 49, 56);
        c[ImGuiCol_TabSelectedOverline] = vec(59, 130, 246);
        c[ImGuiCol_TableHeaderBg] = vec(37, 41, 47);
        c[ImGuiCol_TableBorderStrong] = vec(52, 57, 65);
        c[ImGuiCol_TableBorderLight] = vec(44, 49, 56);
        c[ImGuiCol_TableRowBgAlt] = vec(255, 255, 255, 6);
        c[ImGuiCol_ScrollbarBg] = vec(0, 0, 0, 0);
        c[ImGuiCol_ScrollbarGrab] = vec(70, 76, 86);
        // Window title bars (dialogs, popped-out windows) in the app's chrome colours, not ImGui's blue.
        c[ImGuiCol_TitleBg] = vec(24, 27, 31);
        c[ImGuiCol_TitleBgActive] = vec(34, 38, 44);
        c[ImGuiCol_TitleBgCollapsed] = vec(24, 27, 31);
        c[ImGuiCol_ResizeGrip] = vec(255, 255, 255, 14);
        c[ImGuiCol_ResizeGripHovered] = vec(59, 130, 246, 120);
        c[ImGuiCol_ResizeGripActive] = vec(59, 130, 246, 200);
        c[ImGuiCol_NavWindowingHighlight] = vec(59, 130, 246, 180);

        p.titleBarBg = rgba(24, 27, 31);
        p.ribbonBg = rgba(34, 38, 44);
        p.panelBg = rgba(30, 33, 38);
        p.panelHeaderBg = rgba(37, 41, 47);
        p.border = rgba(52, 57, 65);
        p.text = rgba(222, 226, 231);
        p.textDim = rgba(150, 158, 168);
        p.textFaint = rgba(104, 112, 122);
        p.accent = rgba(59, 130, 246);
        p.accentSoft = rgba(59, 130, 246, 55);
        p.hover = rgba(255, 255, 255, 16);
        p.tabActiveBg = rgba(48, 54, 62);
        p.statusBarBg = rgba(24, 27, 31);
        p.modeAgent = rgba(167, 139, 250);
        p.modeCode = rgba(56, 189, 148);

        p.iconBlue = rgba(96, 165, 250);
        p.iconTeal = rgba(45, 212, 191);
        p.iconGreen = rgba(74, 222, 128);
        p.iconOrange = rgba(251, 146, 60);
        p.iconPurple = rgba(167, 139, 250);
        p.iconRed = rgba(248, 113, 113);
        p.iconGray = rgba(180, 186, 194);

        p.editorBg = rgba(22, 25, 29);
        p.gutterBg = rgba(22, 25, 29);
        p.gutterText = rgba(88, 96, 106);
        p.gutterTextCurrent = rgba(200, 206, 214);
        p.currentLine = rgba(255, 255, 255, 9);
        p.selection = rgba(59, 130, 246, 85);
        p.caret = rgba(235, 238, 242);
        p.findMatch = rgba(234, 179, 8, 70);
        p.findMatchCurrent = rgba(234, 179, 8, 150);
        p.whitespace = rgba(255, 255, 255, 40);
        p.indentGuide = rgba(255, 255, 255, 22);
        p.bookmark = rgba(96, 165, 250);

        p.synText = rgba(212, 216, 222);
        p.synKeyword = rgba(198, 120, 221);
        p.synType = rgba(229, 192, 123);
        p.synString = rgba(152, 195, 121);
        p.synNumber = rgba(209, 154, 102);
        p.synComment = rgba(106, 115, 125);
        p.synPreprocessor = rgba(97, 175, 239);
        p.synHeading = rgba(97, 175, 239);
        {
            const ImU32 colors[] = {
                p.synText, p.synKeyword, p.synType, rgba(97, 175, 239), rgba(224, 108, 117), p.synString,
                p.synNumber, p.synComment, rgba(86, 182, 194), rgba(224, 108, 117), p.synHeading,
                rgba(198, 120, 221), rgba(152, 195, 121), rgba(86, 182, 194), rgba(224, 108, 117), rgba(209, 154, 102),
                // CSV columns
                rgba(97, 175, 239), rgba(152, 195, 121), rgba(229, 192, 123), rgba(198, 120, 221), rgba(86, 182, 194),
                rgba(224, 108, 117)};
            static_assert(sizeof(colors) / sizeof(colors[0]) == size_t(model::Token::Count), "one colour per token");
            std::copy(std::begin(colors), std::end(colors), p.syntax);
        }

        p.severityError = rgba(248, 81, 73);
        p.severityWarning = rgba(234, 179, 8);
        p.severityInfo = rgba(96, 165, 250);
        p.diffAddedBg = rgba(46, 160, 67, 45);
        p.diffRemovedBg = rgba(248, 81, 73, 45);
        p.diffChangedBg = rgba(234, 179, 8, 28);
        p.diffAddedText = rgba(86, 211, 100);
        p.diffRemovedText = rgba(248, 113, 113);

        p.bubbleUser = rgba(59, 130, 246, 50);
        p.bubbleAgent = rgba(44, 49, 56);
        p.bubbleTool = rgba(255, 255, 255, 8);
        static const uint32_t ansiDark[16] = {0x000000, 0xCD3131, 0x0DBC79, 0xE5E510, 0x2472C8, 0xBC3FBC,
                                              0x11A8CD, 0xE5E5E5, 0x666666, 0xF14C4C, 0x23D18B, 0xF5F543,
                                              0x3B8EEA, 0xD670D6, 0x29B8DB, 0xE5E5E5};
        std::copy(std::begin(ansiDark), std::end(ansiDark), p.ansi);
        p.clearColor = vec(24, 27, 31);
    } else {
        ImGui::StyleColorsLight(&style);
        ImVec4* c = style.Colors;
        c[ImGuiCol_Text] = vec(31, 35, 40);
        c[ImGuiCol_TextDisabled] = vec(110, 118, 128);
        c[ImGuiCol_WindowBg] = vec(246, 247, 249);
        c[ImGuiCol_ChildBg] = vec(0, 0, 0, 0);
        c[ImGuiCol_PopupBg] = vec(255, 255, 255, 252);
        c[ImGuiCol_Border] = vec(214, 219, 225);
        c[ImGuiCol_FrameBg] = vec(255, 255, 255);
        c[ImGuiCol_FrameBgHovered] = vec(238, 241, 245);
        c[ImGuiCol_FrameBgActive] = vec(230, 234, 240);
        c[ImGuiCol_Button] = vec(236, 239, 243);
        c[ImGuiCol_ButtonHovered] = vec(224, 228, 234);
        c[ImGuiCol_ButtonActive] = vec(37, 99, 235, 120);
        c[ImGuiCol_Header] = vec(37, 99, 235, 40);
        c[ImGuiCol_HeaderHovered] = vec(0, 0, 0, 14);
        c[ImGuiCol_HeaderActive] = vec(37, 99, 235, 90);
        c[ImGuiCol_CheckMark] = vec(37, 99, 235);
        c[ImGuiCol_Separator] = vec(214, 219, 225);
        c[ImGuiCol_TabSelectedOverline] = vec(37, 99, 235);
        c[ImGuiCol_TableHeaderBg] = vec(238, 241, 245);
        c[ImGuiCol_TableBorderStrong] = vec(214, 219, 225);
        c[ImGuiCol_TableBorderLight] = vec(228, 232, 237);
        c[ImGuiCol_TableRowBgAlt] = vec(0, 0, 0, 6);
        c[ImGuiCol_ScrollbarBg] = vec(0, 0, 0, 0);
        c[ImGuiCol_TitleBg] = vec(232, 235, 239);
        c[ImGuiCol_TitleBgActive] = vec(222, 226, 232);
        c[ImGuiCol_TitleBgCollapsed] = vec(232, 235, 239);
        c[ImGuiCol_ResizeGrip] = vec(0, 0, 0, 14);
        c[ImGuiCol_ResizeGripHovered] = vec(37, 99, 235, 110);
        c[ImGuiCol_ResizeGripActive] = vec(37, 99, 235, 190);

        p.titleBarBg = rgba(232, 235, 239);
        p.ribbonBg = rgba(246, 247, 249);
        p.panelBg = rgba(246, 247, 249);
        p.panelHeaderBg = rgba(236, 239, 243);
        p.border = rgba(214, 219, 225);
        p.text = rgba(31, 35, 40);
        p.textDim = rgba(88, 96, 106);
        p.textFaint = rgba(140, 148, 158);
        p.accent = rgba(37, 99, 235);
        p.accentSoft = rgba(37, 99, 235, 36);
        p.hover = rgba(0, 0, 0, 14);
        p.tabActiveBg = rgba(255, 255, 255);
        p.statusBarBg = rgba(232, 235, 239);
        p.modeAgent = rgba(124, 58, 237);
        p.modeCode = rgba(5, 150, 105);

        p.iconBlue = rgba(37, 99, 235);
        p.iconTeal = rgba(13, 148, 136);
        p.iconGreen = rgba(22, 163, 74);
        p.iconOrange = rgba(234, 88, 12);
        p.iconPurple = rgba(124, 58, 237);
        p.iconRed = rgba(220, 38, 38);
        p.iconGray = rgba(75, 85, 99);

        p.editorBg = rgba(255, 255, 255);
        p.gutterBg = rgba(255, 255, 255);
        p.gutterText = rgba(160, 168, 178);
        p.gutterTextCurrent = rgba(31, 35, 40);
        p.currentLine = rgba(0, 0, 0, 9);
        p.selection = rgba(37, 99, 235, 60);
        p.caret = rgba(20, 20, 20);
        p.findMatch = rgba(250, 204, 21, 90);
        p.findMatchCurrent = rgba(249, 115, 22, 120);
        p.whitespace = rgba(0, 0, 0, 50);
        p.indentGuide = rgba(0, 0, 0, 25);
        p.bookmark = rgba(37, 99, 235);

        p.synText = rgba(36, 41, 47);
        p.synKeyword = rgba(166, 38, 164);
        p.synType = rgba(152, 104, 1);
        p.synString = rgba(80, 161, 79);
        p.synNumber = rgba(152, 104, 1);
        p.synComment = rgba(140, 148, 158);
        p.synPreprocessor = rgba(64, 120, 242);
        p.synHeading = rgba(64, 120, 242);
        {
            const ImU32 colors[] = {
                p.synText, p.synKeyword, p.synType, rgba(64, 120, 242), rgba(228, 86, 73), p.synString,
                p.synNumber, p.synComment, rgba(1, 132, 188), rgba(228, 86, 73), p.synHeading,
                rgba(166, 38, 164), rgba(80, 161, 79), rgba(1, 132, 188), rgba(228, 86, 73), rgba(152, 104, 1),
                // CSV columns
                rgba(64, 120, 242), rgba(80, 161, 79), rgba(183, 107, 1), rgba(166, 38, 164), rgba(1, 132, 188),
                rgba(228, 86, 73)};
            static_assert(sizeof(colors) / sizeof(colors[0]) == size_t(model::Token::Count), "one colour per token");
            std::copy(std::begin(colors), std::end(colors), p.syntax);
        }

        p.severityError = rgba(220, 38, 38);
        p.severityWarning = rgba(202, 138, 4);
        p.severityInfo = rgba(37, 99, 235);
        p.diffAddedBg = rgba(34, 197, 94, 40);
        p.diffRemovedBg = rgba(239, 68, 68, 36);
        p.diffChangedBg = rgba(234, 179, 8, 30);
        p.diffAddedText = rgba(21, 128, 61);
        p.diffRemovedText = rgba(185, 28, 28);

        p.bubbleUser = rgba(37, 99, 235, 28);
        p.bubbleAgent = rgba(236, 239, 243);
        p.bubbleTool = rgba(0, 0, 0, 6);
        static const uint32_t ansiLight[16] = {0x000000, 0xCD3131, 0x00BC00, 0x949800, 0x0451A5, 0xBC05BC,
                                               0x0598BC, 0x555555, 0x666666, 0xCD3131, 0x14CE14, 0xB5BA00,
                                               0x0451A5, 0xBC05BC, 0x0598BC, 0xA5A5A5};
        std::copy(std::begin(ansiLight), std::end(ansiLight), p.ansi);
        p.clearColor = vec(232, 235, 239);
    }
    return p;
}

} // namespace enp::view
