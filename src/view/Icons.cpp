#include "enp/view/Icons.h"

#include "IconsFontAwesome6.h"

namespace enp::view {

using viewmodel::CommandId;

const char* commandIcon(CommandId id)
{
    switch (id) {
    case CommandId::NewFile: return ICON_FA_FILE_CIRCLE_PLUS;
    case CommandId::OpenFile: return ICON_FA_FOLDER_OPEN;
    case CommandId::Save: return ICON_FA_FLOPPY_DISK;
    case CommandId::SaveAll: return ICON_FA_FILE_ARROW_DOWN;
    case CommandId::CloseFile: return ICON_FA_XMARK;
    case CommandId::Cut: return ICON_FA_SCISSORS;
    case CommandId::Copy: return ICON_FA_COPY;
    case CommandId::Paste: return ICON_FA_PASTE;
    case CommandId::Undo: return ICON_FA_ROTATE_LEFT;
    case CommandId::Redo: return ICON_FA_ROTATE_RIGHT;
    case CommandId::SelectAll: return ICON_FA_OBJECT_GROUP;
    case CommandId::DuplicateLine: return ICON_FA_CLONE;
    case CommandId::DeleteLine: return ICON_FA_TRASH_CAN;
    case CommandId::MoveLineUp: return ICON_FA_ARROW_UP;
    case CommandId::MoveLineDown: return ICON_FA_ARROW_DOWN;
    case CommandId::SortLines: return ICON_FA_ARROW_DOWN_A_Z;
    case CommandId::UpperCase: return ICON_FA_TEXT_HEIGHT;
    case CommandId::LowerCase: return ICON_FA_FONT;
    case CommandId::ToggleComment: return ICON_FA_COMMENT;
    case CommandId::Indent: return ICON_FA_INDENT;
    case CommandId::Outdent: return ICON_FA_OUTDENT;
    case CommandId::TrimTrailing: return ICON_FA_ERASER;
    case CommandId::Find: return ICON_FA_MAGNIFYING_GLASS;
    case CommandId::FindNext: return ICON_FA_FORWARD;
    case CommandId::FindPrevious: return ICON_FA_BACKWARD;
    case CommandId::Replace: return ICON_FA_RIGHT_LEFT;
    case CommandId::FindInFiles: return ICON_FA_FOLDER_TREE;
    case CommandId::GoToLine: return ICON_FA_ARROW_RIGHT_TO_BRACKET;
    case CommandId::ToggleBookmark: return ICON_FA_BOOKMARK;
    case CommandId::NextBookmark: return ICON_FA_ARROW_DOWN_LONG;
    case CommandId::PreviousBookmark: return ICON_FA_ARROW_UP_LONG;
    case CommandId::ClearBookmarks: return ICON_FA_BROOM;
    case CommandId::ToggleExplorer: return ICON_FA_SITEMAP;
    case CommandId::ToggleReviewPanel: return ICON_FA_CLIPBOARD_CHECK;
    case CommandId::ToggleAgentPanel: return ICON_FA_ROBOT;
    case CommandId::ToggleWordWrap: return ICON_FA_TURN_DOWN;
    case CommandId::ToggleWhitespace: return ICON_FA_PARAGRAPH;
    case CommandId::ToggleLineNumbers: return ICON_FA_LIST_OL;
    case CommandId::ToggleIndentGuides: return ICON_FA_GRIP_LINES_VERTICAL;
    case CommandId::ZoomIn: return ICON_FA_MAGNIFYING_GLASS_PLUS;
    case CommandId::ZoomOut: return ICON_FA_MAGNIFYING_GLASS_MINUS;
    case CommandId::ZoomReset: return ICON_FA_TEXT_WIDTH;
    case CommandId::ToggleTheme: return ICON_FA_CIRCLE_HALF_STROKE;
    case CommandId::EolCrLf:
    case CommandId::EolLf:
    case CommandId::EolCr: return ICON_FA_TURN_DOWN;
    case CommandId::EncodingUtf8:
    case CommandId::EncodingUtf8Bom:
    case CommandId::EncodingUtf16Le:
    case CommandId::EncodingUtf16Be:
    case CommandId::EncodingAnsi: return ICON_FA_LANGUAGE;
    case CommandId::LanguagePlainText: return ICON_FA_FILE_LINES;
    case CommandId::LanguageCpp:
    case CommandId::LanguagePython:
    case CommandId::LanguageJavaScript:
    case CommandId::LanguageJson:
    case CommandId::LanguageCMake: return ICON_FA_FILE_CODE;
    case CommandId::LanguageMarkdown: return ICON_FA_FILE_PEN;
    case CommandId::RunLint: return ICON_FA_SPELL_CHECK;
    case CommandId::RunStaticAnalysis: return ICON_FA_BUG;
    case CommandId::RunSecurityScan: return ICON_FA_SHIELD_HALVED;
    case CommandId::RunStyleCheck: return ICON_FA_RULER;
    case CommandId::RunComplexity: return ICON_FA_CHART_LINE;
    case CommandId::RunAgentReview: return ICON_FA_WAND_MAGIC_SPARKLES;
    case CommandId::RunAllReviews: return ICON_FA_CHECK_DOUBLE;
    case CommandId::CompareWithSaved: return ICON_FA_CODE_COMPARE;
    case CommandId::CompareAgentChanges: return ICON_FA_CODE_BRANCH;
    case CommandId::AddReviewComment: return ICON_FA_COMMENT_DOTS;
    case CommandId::NextFinding: return ICON_FA_ARROW_DOWN_LONG;
    case CommandId::PreviousFinding: return ICON_FA_ARROW_UP_LONG;
    case CommandId::ClearFindings: return ICON_FA_BROOM;
    case CommandId::NewChat: return ICON_FA_PLUS;
    case CommandId::AskAgent: return ICON_FA_MESSAGE;
    case CommandId::ExplainSelection: return ICON_FA_LIGHTBULB;
    case CommandId::RefactorSelection: return ICON_FA_SCREWDRIVER_WRENCH;
    case CommandId::GenerateTests: return ICON_FA_FLASK;
    case CommandId::FixFindings: return ICON_FA_WRENCH;
    case CommandId::AcceptAllChanges: return ICON_FA_CHECK_DOUBLE;
    case CommandId::RejectAllChanges: return ICON_FA_BAN;
    case CommandId::StopAgent: return ICON_FA_STOP;
    case CommandId::SwitchToAgentMode: return ICON_FA_ROBOT;
    case CommandId::SwitchToCodeMode: return ICON_FA_CODE;
    case CommandId::ToggleWorkspaceMode: return ICON_FA_ARROWS_ROTATE;
    case CommandId::StartRecording: return ICON_FA_CIRCLE_DOT;
    case CommandId::StopRecording: return ICON_FA_STOP;
    case CommandId::PlayMacro: return ICON_FA_PLAY;
    case CommandId::HashSha256: return ICON_FA_HASHTAG;
    case CommandId::Preferences: return ICON_FA_GEAR;
    case CommandId::About: return ICON_FA_CIRCLE_INFO;
    case CommandId::Exit: return ICON_FA_POWER_OFF;
    case CommandId::Count: break;
    }
    return ICON_FA_SQUARE;
}

ImU32 commandTint(CommandId id, const Palette& p)
{
    switch (id) {
    case CommandId::NewFile:
    case CommandId::OpenFile:
    case CommandId::Save:
    case CommandId::SaveAll:
    case CommandId::Find:
    case CommandId::FindNext:
    case CommandId::FindPrevious:
    case CommandId::Replace:
    case CommandId::FindInFiles:
    case CommandId::GoToLine:
    case CommandId::ToggleBookmark:
    case CommandId::NextBookmark:
    case CommandId::PreviousBookmark:
    case CommandId::ToggleExplorer:
        return p.iconBlue;
    case CommandId::Cut:
    case CommandId::Copy:
    case CommandId::Paste:
    case CommandId::DuplicateLine:
    case CommandId::MoveLineUp:
    case CommandId::MoveLineDown:
    case CommandId::SortLines:
    case CommandId::Indent:
    case CommandId::Outdent:
    case CommandId::ToggleComment:
        return p.iconTeal;
    case CommandId::RunLint:
    case CommandId::RunStyleCheck:
    case CommandId::RunComplexity:
    case CommandId::CompareWithSaved:
    case CommandId::CompareAgentChanges:
    case CommandId::AddReviewComment:
    case CommandId::ToggleReviewPanel:
        return p.iconOrange;
    case CommandId::RunStaticAnalysis:
    case CommandId::RunSecurityScan:
    case CommandId::DeleteLine:
    case CommandId::CloseFile:
    case CommandId::RejectAllChanges:
    case CommandId::StopAgent:
    case CommandId::StopRecording:
    case CommandId::StartRecording:
        return p.iconRed;
    case CommandId::RunAllReviews:
    case CommandId::AcceptAllChanges:
    case CommandId::PlayMacro:
        return p.iconGreen;
    case CommandId::RunAgentReview:
    case CommandId::NewChat:
    case CommandId::AskAgent:
    case CommandId::ExplainSelection:
    case CommandId::RefactorSelection:
    case CommandId::GenerateTests:
    case CommandId::FixFindings:
    case CommandId::ToggleAgentPanel:
    case CommandId::SwitchToAgentMode:
        return p.iconPurple;
    default:
        return p.iconGray;
    }
}

} // namespace enp::view
