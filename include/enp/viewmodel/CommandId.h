// Every user-invocable action, with the display metadata shared by ribbon, menus and the
// command search box. Icons are a view concern and live in view/Icons.
#pragma once

#include <string_view>

namespace enp::viewmodel {

enum class CommandId {
    // File
    NewFile, OpenFile, OpenFolder, Save, SaveAs, SaveAll, CloseFile,
    CloseOthers, CloseAll, CloseSaved, NextDocument, PreviousDocument,
    // Clipboard / history
    Cut, Copy, Paste, Undo, Redo, SelectAll,
    // Line operations
    DuplicateLine, DeleteLine, MoveLineUp, MoveLineDown, SortLines,
    UpperCase, LowerCase, ToggleComment, Indent, Outdent, TrimTrailing,
    // Search
    Find, FindNext, FindPrevious, Replace, FindInFiles, GoToLine,
    ToggleBookmark, NextBookmark, PreviousBookmark, ClearBookmarks,
    // View
    ToggleExplorer, TogglePanel, ToggleAgentPanel, ToggleMaximizePanel, ToggleSlimRibbon,
    ToggleWordWrap, ToggleWhitespace, ToggleLineNumbers, ToggleIndentGuides, ToggleMarkdownView,
    ZoomIn, ZoomOut, ZoomReset, ToggleTheme, SyntaxColors,
    // Reading
    DetailFull, DetailBalanced, DetailOverview, DetailOutline,
    FoldAll, UnfoldAll, ToggleFold, ToggleLayersMap, ShowLayers, ShowMinimap, SummarizeFile,
    ShowCodeView, ShowMapView, FoldingSyntax, FoldingLogical, DissectProject, RefreshMap,
    // Document format
    EolCrLf, EolLf, EolCr,
    EncodingUtf8, EncodingUtf8Bom, EncodingUtf16Le, EncodingUtf16Be, EncodingAnsi,
    // Indentation
    ToggleSmartIndent, IndentWithSpaces, IndentWithTabs, IndentWidth2, IndentWidth4, IndentWidth8,
    DetectIndentation, ConvertIndentation,
    ChooseLanguage,
    // Review
    RunLint, RunStaticAnalysis, RunSecurityScan, RunStyleCheck, RunComplexity, RunAgentReview, RunAllReviews,
    CompareWithSaved, CompareAgentChanges, AddReviewComment, NextFinding, PreviousFinding, ClearFindings,
    // Agent
    NewChat, AskAgent, ExplainSelection, RefactorSelection, GenerateTests, FixFindings,
    AcceptAllChanges, RejectAllChanges, StopAgent, StartTour,
    // Workspace
    SwitchToAgentMode, SwitchToCodeMode, ToggleWorkspaceMode,
    // Terminal
    ToggleTerminal, NewTerminal, KillTerminal, ClearTerminal, PopOutTerminal,
    // Tools
    StartRecording, StopRecording, PlayMacro, HashSha256, Preferences, About, Exit,
    // Git
    ShowGit, GitCommit, GitStageAll, GitDiscardAll, GitBranches, GitMerge, GitSync, GitPull, GitPush, GitFetch, ShowGitHistory, GitBlame, GitStash, GitStashPop, GitWriteMessage, GitGuide,
    // File (appended)
    CloseFolder,

    Count
};

struct CommandInfo {
    const char* label;    // short ribbon caption
    const char* tooltip;  // longer description
    const char* shortcut; // display only, e.g. "Ctrl+S"; may be empty
};

const CommandInfo& commandInfo(CommandId id);
// Stable identifier (the enumerator name, e.g. "ToggleTerminal") used by automation and the MCP connector.
const char* commandKey(CommandId id);
bool commandFromKey(std::string_view key, CommandId& id);

} // namespace enp::viewmodel
