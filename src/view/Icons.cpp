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
    case CommandId::SaveAs: return ICON_FA_FILE_EXPORT;
    case CommandId::OpenFolder: return ICON_FA_FOLDER_TREE;
    case CommandId::CloseFile: return ICON_FA_XMARK;
    case CommandId::CloseOthers: return ICON_FA_RECTANGLE_XMARK;
    case CommandId::CloseAll: return ICON_FA_RECTANGLE_XMARK;
    case CommandId::CloseSaved: return ICON_FA_CIRCLE_CHECK;
    case CommandId::NextDocument: return ICON_FA_ARROW_RIGHT;
    case CommandId::PreviousDocument: return ICON_FA_ARROW_LEFT;
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
    case CommandId::TogglePanel: return ICON_FA_TABLE_LIST;
    case CommandId::ToggleMaximizePanel: return ICON_FA_UP_RIGHT_AND_DOWN_LEFT_FROM_CENTER;
    case CommandId::ToggleTerminal: return ICON_FA_TERMINAL;
    case CommandId::NewTerminal: return ICON_FA_SQUARE_PLUS;
    case CommandId::KillTerminal: return ICON_FA_TRASH_CAN;
    case CommandId::ClearTerminal: return ICON_FA_ERASER;
    case CommandId::PopOutTerminal: return ICON_FA_UP_RIGHT_FROM_SQUARE;
    case CommandId::ToggleAgentPanel: return ICON_FA_ROBOT;
    case CommandId::ToggleWordWrap: return ICON_FA_TURN_DOWN;
    case CommandId::ToggleWhitespace: return ICON_FA_PARAGRAPH;
    case CommandId::ToggleLineNumbers: return ICON_FA_LIST_OL;
    case CommandId::ToggleIndentGuides: return ICON_FA_GRIP_LINES_VERTICAL;
    case CommandId::ZoomIn: return ICON_FA_MAGNIFYING_GLASS_PLUS;
    case CommandId::ZoomOut: return ICON_FA_MAGNIFYING_GLASS_MINUS;
    case CommandId::ZoomReset: return ICON_FA_TEXT_WIDTH;
    case CommandId::ToggleTheme: return ICON_FA_CIRCLE_HALF_STROKE;
    case CommandId::SyntaxColors: return ICON_FA_PALETTE;
    case CommandId::DetailFull: return ICON_FA_ALIGN_JUSTIFY;
    case CommandId::DetailBalanced: return ICON_FA_ALIGN_LEFT;
    case CommandId::DetailOverview: return ICON_FA_LIST_UL;
    case CommandId::DetailOutline: return ICON_FA_LAYER_GROUP;
    case CommandId::FoldAll: return ICON_FA_COMPRESS;
    case CommandId::UnfoldAll: return ICON_FA_EXPAND;
    case CommandId::ToggleFold: return ICON_FA_SQUARE_MINUS;
    case CommandId::ToggleLayersMap: return ICON_FA_TABLE_COLUMNS;
    case CommandId::ShowLayers: return ICON_FA_LAYER_GROUP;
    case CommandId::ShowMinimap: return ICON_FA_BARS_STAGGERED;
    case CommandId::SummarizeFile: return ICON_FA_WAND_MAGIC_SPARKLES;
    case CommandId::ShowCodeView: return ICON_FA_CODE;
    case CommandId::ShowMapView: return ICON_FA_DIAGRAM_PROJECT;
    case CommandId::FoldingSyntax: return ICON_FA_BARS_STAGGERED;
    case CommandId::FoldingLogical: return ICON_FA_SHOE_PRINTS;
    case CommandId::DissectProject: return ICON_FA_BRAIN;
    case CommandId::RefreshMap: return ICON_FA_ARROWS_ROTATE;
    case CommandId::EolCrLf:
    case CommandId::EolLf:
    case CommandId::EolCr: return ICON_FA_TURN_DOWN;
    case CommandId::EncodingUtf8:
    case CommandId::EncodingUtf8Bom:
    case CommandId::EncodingUtf16Le:
    case CommandId::EncodingUtf16Be:
    case CommandId::EncodingAnsi: return ICON_FA_LANGUAGE;
    case CommandId::ToggleSmartIndent: return ICON_FA_WAND_MAGIC_SPARKLES;
    case CommandId::IndentWithSpaces: return ICON_FA_ELLIPSIS;
    case CommandId::IndentWithTabs: return ICON_FA_ARROW_RIGHT_TO_BRACKET;
    case CommandId::IndentWidth2:
    case CommandId::IndentWidth4:
    case CommandId::IndentWidth8: return ICON_FA_TEXT_WIDTH;
    case CommandId::DetectIndentation: return ICON_FA_MAGNIFYING_GLASS;
    case CommandId::ConvertIndentation: return ICON_FA_ARROWS_ROTATE;
    case CommandId::ChooseLanguage: return ICON_FA_FILE_CODE;
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
    case CommandId::StartTour: return ICON_FA_HAND_POINTER;
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
    case CommandId::ShowGit: return ICON_FA_CODE_BRANCH;
    case CommandId::GitCommit: return ICON_FA_CODE_COMMIT;
    case CommandId::GitStageAll: return ICON_FA_PLUS;
    case CommandId::GitDiscardAll: return ICON_FA_ROTATE_LEFT;
    case CommandId::GitBranches: return ICON_FA_CODE_BRANCH;
    case CommandId::GitMerge: return ICON_FA_CODE_MERGE;
    case CommandId::GitSync: return ICON_FA_ARROWS_ROTATE;
    case CommandId::GitPull: return ICON_FA_DOWNLOAD;
    case CommandId::GitPush: return ICON_FA_UPLOAD;
    case CommandId::GitFetch: return ICON_FA_CLOUD_ARROW_DOWN;
    case CommandId::ShowGitHistory: return ICON_FA_CLOCK_ROTATE_LEFT;
    case CommandId::GitBlame: return ICON_FA_USER_PEN;
    case CommandId::GitStash: return ICON_FA_BOX_ARCHIVE;
    case CommandId::GitStashPop: return ICON_FA_BOX_OPEN;
    case CommandId::GitWriteMessage: return ICON_FA_WAND_MAGIC_SPARKLES;
    case CommandId::GitGuide: return ICON_FA_GRADUATION_CAP;
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
    case CommandId::TogglePanel:
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
    case CommandId::DetailFull:
    case CommandId::DetailBalanced:
    case CommandId::DetailOverview:
    case CommandId::DetailOutline:
    case CommandId::FoldAll:
    case CommandId::UnfoldAll:
    case CommandId::ToggleFold:
        return p.iconTeal;
    case CommandId::ToggleLayersMap:
    case CommandId::ShowLayers:
    case CommandId::ShowMinimap:
    case CommandId::ShowMapView:
        return p.iconOrange;
    case CommandId::ShowCodeView:
    case CommandId::FoldingSyntax:
    case CommandId::RefreshMap:
        return p.iconBlue;
    case CommandId::SummarizeFile:
    case CommandId::DissectProject:
    case CommandId::FoldingLogical:
    case CommandId::StartTour:
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
    case CommandId::ToggleTerminal:
    case CommandId::NewTerminal:
    case CommandId::GitCommit:
        return p.iconGreen;
    case CommandId::ShowGit:
    case CommandId::GitBranches:
    case CommandId::GitMerge:
    case CommandId::GitWriteMessage:
    case CommandId::GitGuide:
        return p.iconPurple;
    case CommandId::GitStageAll:
        return p.iconBlue;
    case CommandId::GitDiscardAll:
        return p.iconRed;
    case CommandId::GitSync:
    case CommandId::GitPull:
    case CommandId::GitPush:
    case CommandId::GitFetch:
        return p.iconTeal;
    case CommandId::ShowGitHistory:
    case CommandId::GitBlame:
        return p.iconOrange;
    default:
        return p.iconGray;
    }
}

} // namespace enp::view
