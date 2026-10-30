// Every user-invocable action, with the display metadata shared by ribbon, menus and the
// command search box. Icons are a view concern and live in view/Icons.
#pragma once

namespace enp::viewmodel {

enum class CommandId {
    // File
    NewFile, OpenFile, Save, SaveAll, CloseFile,
    // Clipboard / history
    Cut, Copy, Paste, Undo, Redo, SelectAll,
    // Line operations
    DuplicateLine, DeleteLine, MoveLineUp, MoveLineDown, SortLines,
    UpperCase, LowerCase, ToggleComment, Indent, Outdent, TrimTrailing,
    // Search
    Find, FindNext, FindPrevious, Replace, FindInFiles, GoToLine,
    ToggleBookmark, NextBookmark, PreviousBookmark, ClearBookmarks,
    // View
    ToggleExplorer, ToggleReviewPanel, ToggleAgentPanel,
    ToggleWordWrap, ToggleWhitespace, ToggleLineNumbers, ToggleIndentGuides,
    ZoomIn, ZoomOut, ZoomReset, ToggleTheme,
    // Document format
    EolCrLf, EolLf, EolCr,
    EncodingUtf8, EncodingUtf8Bom, EncodingUtf16Le, EncodingUtf16Be, EncodingAnsi,
    LanguagePlainText, LanguageCpp, LanguagePython, LanguageJavaScript, LanguageMarkdown, LanguageJson, LanguageCMake,
    // Review
    RunLint, RunStaticAnalysis, RunSecurityScan, RunStyleCheck, RunComplexity, RunAgentReview, RunAllReviews,
    CompareWithSaved, CompareAgentChanges, AddReviewComment, NextFinding, PreviousFinding, ClearFindings,
    // Agent
    NewChat, AskAgent, ExplainSelection, RefactorSelection, GenerateTests, FixFindings,
    AcceptAllChanges, RejectAllChanges, StopAgent,
    // Workspace
    SwitchToAgentMode, SwitchToCodeMode, ToggleWorkspaceMode,
    // Tools
    StartRecording, StopRecording, PlayMacro, HashSha256, Preferences, About, Exit,

    Count
};

struct CommandInfo {
    const char* label;    // short ribbon caption
    const char* tooltip;  // longer description
    const char* shortcut; // display only, e.g. "Ctrl+S"; may be empty
};

const CommandInfo& commandInfo(CommandId id);

} // namespace enp::viewmodel
