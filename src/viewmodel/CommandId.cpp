#include "enp/viewmodel/CommandId.h"

namespace enp::viewmodel {

namespace {

CommandInfo makeInfo(CommandId id)
{
    switch (id) {
    case CommandId::NewFile: return {"New", "Create a new document", "Ctrl+N"};
    case CommandId::OpenFile: return {"Open", "Open a file", "Ctrl+O"};
    case CommandId::Save: return {"Save", "Save the current document", "Ctrl+S"};
    case CommandId::SaveAll: return {"Save All", "Save all open documents", "Ctrl+Shift+S"};
    case CommandId::SaveAs: return {"Save As", "Save the document under a new name", "Ctrl+Alt+S"};
    case CommandId::OpenFolder: return {"Open Folder", "Open a folder", "Ctrl+Alt+O"};
    case CommandId::CloseFile: return {"Close", "Close the current document", "Ctrl+W"};
    case CommandId::CloseOthers: return {"Close Others", "Close every other document (unsaved ones stay open)", ""};
    case CommandId::CloseAll: return {"Close All", "Close all documents (unsaved ones stay open)", ""};
    case CommandId::CloseSaved: return {"Close Saved", "Close the documents without unsaved changes", ""};
    case CommandId::NextDocument: return {"Next Tab", "Switch to the next document", "Ctrl+Tab"};
    case CommandId::PreviousDocument: return {"Previous Tab", "Switch to the previous document", "Ctrl+Shift+Tab"};

    case CommandId::Cut: return {"Cut", "Cut the selection", "Ctrl+X"};
    case CommandId::Copy: return {"Copy", "Copy the selection", "Ctrl+C"};
    case CommandId::Paste: return {"Paste", "Paste from the clipboard", "Ctrl+V"};
    case CommandId::Undo: return {"Undo", "Undo the last edit", "Ctrl+Z"};
    case CommandId::Redo: return {"Redo", "Redo the last undone edit", "Ctrl+Y"};
    case CommandId::SelectAll: return {"Select All", "Select the whole document", "Ctrl+A"};

    case CommandId::DuplicateLine: return {"Duplicate", "Duplicate the current line(s)", "Ctrl+D"};
    case CommandId::DeleteLine: return {"Delete Line", "Delete the current line(s)", "Ctrl+Shift+L"};
    case CommandId::MoveLineUp: return {"Move Up", "Move the current line(s) up", "Ctrl+Shift+Up"};
    case CommandId::MoveLineDown: return {"Move Down", "Move the current line(s) down", "Ctrl+Shift+Down"};
    case CommandId::SortLines: return {"Sort", "Sort the selected lines ascending", ""};
    case CommandId::UpperCase: return {"UPPER", "Convert the selection to upper case", "Ctrl+Shift+U"};
    case CommandId::LowerCase: return {"lower", "Convert the selection to lower case", "Ctrl+U"};
    case CommandId::ToggleComment: return {"Comment", "Toggle line comment", "Ctrl+Q"};
    case CommandId::Indent: return {"Indent", "Increase indentation", "Tab"};
    case CommandId::Outdent: return {"Outdent", "Decrease indentation", "Shift+Tab"};
    case CommandId::TrimTrailing: return {"Trim", "Trim trailing whitespace", ""};

    case CommandId::Find: return {"Find", "Find in the current document", "Ctrl+F"};
    case CommandId::FindNext: return {"Next", "Find the next match", "F3"};
    case CommandId::FindPrevious: return {"Previous", "Find the previous match", "Shift+F3"};
    case CommandId::Replace: return {"Replace", "Find and replace", "Ctrl+H"};
    case CommandId::FindInFiles: return {"In Files", "Find in all files of the workspace", "Ctrl+Shift+F"};
    case CommandId::GoToLine: return {"Go To", "Go to a line number", "Ctrl+G"};
    case CommandId::ToggleBookmark: return {"Bookmark", "Toggle a bookmark on the current line", "Ctrl+F2"};
    case CommandId::NextBookmark: return {"Next Mark", "Go to the next bookmark", "F2"};
    case CommandId::PreviousBookmark: return {"Prev Mark", "Go to the previous bookmark", "Shift+F2"};
    case CommandId::ClearBookmarks: return {"Clear Marks", "Remove all bookmarks", ""};

    case CommandId::ToggleExplorer: return {"Explorer", "Show or hide the workspace explorer", "Ctrl+Shift+E"};
    case CommandId::TogglePanel: return {"Panel", "Show or hide the bottom panel (review and terminal)", "Ctrl+J"};
    case CommandId::ToggleMaximizePanel: return {"Maximize", "Maximize or restore the bottom panel", ""};
    case CommandId::ToggleSlimRibbon: return {"Slim Ribbon", "A slimmer ribbon: icons only, more room for your work", "Ctrl+F1"};
    case CommandId::ToggleAgentPanel: return {"Agent Panel", "Show or hide the agent side panel", "Ctrl+Shift+I"};
    case CommandId::ToggleWordWrap: return {"Word Wrap", "Wrap long lines", "Alt+Z"};
    case CommandId::ToggleMarkdownView: return {"Rendered", "Show this Markdown file formatted, or as text to edit", "Ctrl+Shift+V"};
    case CommandId::ToggleWhitespace: return {"Whitespace", "Show spaces and tabs", ""};
    case CommandId::ToggleLineNumbers: return {"Line Numbers", "Show line numbers", ""};
    case CommandId::ToggleIndentGuides: return {"Guides", "Show indent guides", ""};
    case CommandId::ZoomIn: return {"Zoom In", "Increase the text size", "Ctrl+="};
    case CommandId::ZoomOut: return {"Zoom Out", "Decrease the text size", "Ctrl+-"};
    case CommandId::ZoomReset: return {"100%", "Restore the default text size", "Ctrl+0"};
    case CommandId::ToggleTheme: return {"Theme", "Switch between dark and light theme", ""};
    case CommandId::SyntaxColors: return {"Colours", "Customize the syntax colours of every language", ""};
    case CommandId::ChooseLanguage: return {"Language", "Choose the document's language: colouring, folding and indentation", ""};

    case CommandId::DetailFull: return {"Full", "Show every line of code", "Ctrl+Alt+1"};
    case CommandId::DetailBalanced: return {"Balanced", "Collapse long functions and blocks into AI summaries", "Ctrl+Alt+2"};
    case CommandId::DetailOverview: return {"Overview", "Collapse every function body into its AI summary", "Ctrl+Alt+3"};
    case CommandId::DetailOutline: return {"Outline", "Show only the top-level structure, each part summarized", "Ctrl+Alt+4"};
    case CommandId::FoldAll: return {"Fold All", "Collapse every region into its summary", "Ctrl+K Ctrl+0"};
    case CommandId::UnfoldAll: return {"Unfold All", "Expand every region", "Ctrl+K Ctrl+J"};
    case CommandId::ToggleFold: return {"Fold", "Fold or unfold the region around the caret", "Ctrl+Shift+["};
    case CommandId::ToggleLayersMap: return {"Side Bar", "Show or hide the side bar beside the editor (layers or minimap)", "Ctrl+Alt+B"};
    case CommandId::ShowLayers: return {"Layers", "Side bar: zoomable layers of the file or project, with AI summaries", ""};
    case CommandId::ShowMinimap: return {"Minimap", "Side bar: a zoomed-out strip of the whole file", ""};
    case CommandId::SummarizeFile: return {"Summarize", "Regenerate the AI summaries of this file", ""};
    case CommandId::ShowCodeView: return {"Code", "Read the code as text", "Ctrl+Alt+C"};
    case CommandId::ShowMapView: return {"Map", "Read the program as a logical map: modules, components, features, flows", "Ctrl+Alt+M"};
    case CommandId::FoldingSyntax: return {"Syntax", "Fold by the code's structure: brackets, indentation, headings", ""};
    case CommandId::FoldingLogical: return {"Logical", "Fold function bodies into their logical steps, each summarized", ""};
    case CommandId::DissectProject: return {"Dissect", "Ask the agent to dissect the project into features and flows", ""};
    case CommandId::RefreshMap: return {"Rebuild Map", "Dissect the project locally again", ""};

    case CommandId::EolCrLf: return {"CRLF", "Windows line endings (CR LF)", ""};
    case CommandId::EolLf: return {"LF", "Unix line endings (LF)", ""};
    case CommandId::EolCr: return {"CR", "Classic Mac line endings (CR)", ""};
    case CommandId::EncodingUtf8: return {"UTF-8", "Encode in UTF-8", ""};
    case CommandId::EncodingUtf8Bom: return {"UTF-8 BOM", "Encode in UTF-8 with byte order mark", ""};
    case CommandId::EncodingUtf16Le: return {"UTF-16 LE", "Encode in UTF-16 little endian", ""};
    case CommandId::EncodingUtf16Be: return {"UTF-16 BE", "Encode in UTF-16 big endian", ""};
    case CommandId::EncodingAnsi: return {"ANSI", "Encode in the system code page", ""};
    case CommandId::ToggleSmartIndent: return {"Smart Indent", "Indent new lines and closing lines for the language", ""};
    case CommandId::IndentWithSpaces: return {"Spaces", "Indent with spaces", ""};
    case CommandId::IndentWithTabs: return {"Tabs", "Indent with tabs", ""};
    case CommandId::IndentWidth2: return {"Width 2", "Indent width: 2", ""};
    case CommandId::IndentWidth4: return {"Width 4", "Indent width: 4", ""};
    case CommandId::IndentWidth8: return {"Width 8", "Indent width: 8", ""};
    case CommandId::DetectIndentation: return {"Detect", "Detect the indentation from the content", ""};
    case CommandId::ConvertIndentation: return {"Convert", "Re-write every line's indentation in the current style", ""};

    case CommandId::RunLint: return {"Lint", "Run the linter on the workspace", "F7"};
    case CommandId::RunStaticAnalysis: return {"Analyze", "Run static analysis", ""};
    case CommandId::RunSecurityScan: return {"Security", "Run the security scanner", ""};
    case CommandId::RunStyleCheck: return {"Style", "Check formatting and naming", ""};
    case CommandId::RunComplexity: return {"Complexity", "Measure code complexity", ""};
    case CommandId::RunAgentReview: return {"AI Review", "Ask the agent to review the change", ""};
    case CommandId::RunAllReviews: return {"Run All", "Run every review tool", "Ctrl+Shift+F7"};
    case CommandId::CompareWithSaved: return {"Vs Saved", "Compare the document with its saved version", ""};
    case CommandId::CompareAgentChanges: return {"Changes", "Review the changes proposed by the agent", ""};
    case CommandId::AddReviewComment: return {"Comment", "Add a review comment on the current line", "Ctrl+Alt+M"};
    case CommandId::NextFinding: return {"Next", "Go to the next finding", "F8"};
    case CommandId::PreviousFinding: return {"Previous", "Go to the previous finding", "Shift+F8"};
    case CommandId::ClearFindings: return {"Clear", "Clear all findings", ""};

    case CommandId::NewChat: return {"New Chat", "Start a new agent conversation", ""};
    case CommandId::AskAgent: return {"Ask", "Focus the agent prompt", "Ctrl+I"};
    case CommandId::ExplainSelection: return {"Explain", "Ask the agent to explain the selection", ""};
    case CommandId::RefactorSelection: return {"Refactor", "Ask the agent to refactor the selection", ""};
    case CommandId::GenerateTests: return {"Tests", "Ask the agent to write tests for this file", ""};
    case CommandId::FixFindings: return {"Fix Issues", "Ask the agent to fix the open review findings", ""};
    case CommandId::AcceptAllChanges: return {"Accept All", "Apply every proposed change", ""};
    case CommandId::RejectAllChanges: return {"Reject All", "Discard every proposed change", ""};
    case CommandId::StopAgent: return {"Stop", "Stop the running agent", ""};
    case CommandId::StartTour: return {"Tour", "Let the agent show you around with its cursor", ""};

    case CommandId::SwitchToAgentMode: return {"Agent", "Agent-centric workspace: describe the goal, review the changes", ""};
    case CommandId::SwitchToCodeMode: return {"Code", "Manual coding workspace: classic editor with assistant on the side", ""};
    case CommandId::ToggleWorkspaceMode: return {"Switch Mode", "Toggle between Agent and Code workspaces", "Ctrl+Shift+M"};

    case CommandId::ToggleTerminal: return {"Terminal", "Show or hide the integrated terminal", "Ctrl+`"};
    case CommandId::NewTerminal: return {"New Terminal", "Open a new terminal with the default shell", "Ctrl+Shift+`"};
    case CommandId::KillTerminal: return {"Kill", "Close the active terminal and end its process", ""};
    case CommandId::ClearTerminal: return {"Clear", "Clear the active terminal", ""};
    case CommandId::PopOutTerminal: return {"Pop Out", "Move the terminal into its own window (or back into the panel)", ""};

    case CommandId::StartRecording: return {"Record", "Start recording a macro", ""};
    case CommandId::StopRecording: return {"Stop", "Stop recording", ""};
    case CommandId::PlayMacro: return {"Play", "Play back the recorded macro", ""};
    case CommandId::HashSha256: return {"SHA-256", "Show the SHA-256 hash of the selection", ""};
    case CommandId::Preferences: return {"Settings", "Preferences", ""};
    case CommandId::About: return {"About", "About EasyNotepad", ""};
    case CommandId::EngineLab: return {"Engine Lab", "Developer: measure the on-device model on this device", ""};
    case CommandId::ShowGitStatus: return {"Status", "Git status: branch, sync with the remote, and every changed file", ""};
    case CommandId::LocalModel: return {"Local Model", "The offline model for the agent: download it and choose when it answers", ""};
    case CommandId::Exit: return {"Exit", "Quit EasyNotepad", "Alt+F4"};

    case CommandId::ShowGit: return {"Git", "Show the Git tab: changes, staging and commit", ""};
    case CommandId::GitCommit: return {"Commit", "Commit the staged changes", "Ctrl+Enter"};
    case CommandId::GitStageAll: return {"Stage All", "Stage every change", ""};
    case CommandId::GitDiscardAll: return {"Discard", "Discard all unstaged changes", ""};
    case CommandId::GitBranches: return {"Branch", "Switch, create, merge or delete branches", ""};
    case CommandId::GitMerge: return {"Merge", "Merge another branch into the current one", ""};
    case CommandId::GitSync: return {"Sync", "Pull then push the current branch", ""};
    case CommandId::GitPull: return {"Pull", "Pull from the upstream branch", ""};
    case CommandId::GitPush: return {"Push", "Push the current branch", ""};
    case CommandId::GitFetch: return {"Fetch", "Fetch from all remotes", ""};
    case CommandId::ShowGitHistory: return {"History", "Show the commit history", ""};
    case CommandId::GitBlame: return {"Blame", "Show who last changed each line", ""};
    case CommandId::GitStash: return {"Stash", "Stash the uncommitted changes", ""};
    case CommandId::GitStashPop: return {"Pop", "Apply and drop the latest stash", ""};
    case CommandId::GitWriteMessage: return {"Message", "Let the agent write the commit message", ""};
    case CommandId::GitGuide: return {"Learn Git", "Step-by-step guide to git, using this app's Git tools", ""};
    case CommandId::CloseFolder: return {"Close Folder", "Close the folder and its documents", ""};

    case CommandId::Count: break;
    }
    return {"", "", ""};
}

} // namespace

const char* commandKey(CommandId id)
{
    switch (id) {
    case CommandId::NewFile: return "NewFile";
    case CommandId::OpenFile: return "OpenFile";
    case CommandId::Save: return "Save";
    case CommandId::SaveAll: return "SaveAll";
    case CommandId::SaveAs: return "SaveAs";
    case CommandId::OpenFolder: return "OpenFolder";
    case CommandId::CloseFile: return "CloseFile";
    case CommandId::CloseOthers: return "CloseOthers";
    case CommandId::CloseAll: return "CloseAll";
    case CommandId::CloseSaved: return "CloseSaved";
    case CommandId::NextDocument: return "NextDocument";
    case CommandId::PreviousDocument: return "PreviousDocument";
    case CommandId::Cut: return "Cut";
    case CommandId::Copy: return "Copy";
    case CommandId::Paste: return "Paste";
    case CommandId::Undo: return "Undo";
    case CommandId::Redo: return "Redo";
    case CommandId::SelectAll: return "SelectAll";
    case CommandId::DuplicateLine: return "DuplicateLine";
    case CommandId::DeleteLine: return "DeleteLine";
    case CommandId::MoveLineUp: return "MoveLineUp";
    case CommandId::MoveLineDown: return "MoveLineDown";
    case CommandId::SortLines: return "SortLines";
    case CommandId::UpperCase: return "UpperCase";
    case CommandId::LowerCase: return "LowerCase";
    case CommandId::ToggleComment: return "ToggleComment";
    case CommandId::Indent: return "Indent";
    case CommandId::Outdent: return "Outdent";
    case CommandId::TrimTrailing: return "TrimTrailing";
    case CommandId::Find: return "Find";
    case CommandId::FindNext: return "FindNext";
    case CommandId::FindPrevious: return "FindPrevious";
    case CommandId::Replace: return "Replace";
    case CommandId::FindInFiles: return "FindInFiles";
    case CommandId::GoToLine: return "GoToLine";
    case CommandId::ToggleBookmark: return "ToggleBookmark";
    case CommandId::NextBookmark: return "NextBookmark";
    case CommandId::PreviousBookmark: return "PreviousBookmark";
    case CommandId::ClearBookmarks: return "ClearBookmarks";
    case CommandId::ToggleExplorer: return "ToggleExplorer";
    case CommandId::TogglePanel: return "TogglePanel";
    case CommandId::ToggleAgentPanel: return "ToggleAgentPanel";
    case CommandId::ToggleMaximizePanel: return "ToggleMaximizePanel";
    case CommandId::ToggleSlimRibbon: return "ToggleSlimRibbon";
    case CommandId::ToggleWordWrap: return "ToggleWordWrap";
    case CommandId::ToggleMarkdownView: return "ToggleMarkdownView";
    case CommandId::ToggleWhitespace: return "ToggleWhitespace";
    case CommandId::ToggleLineNumbers: return "ToggleLineNumbers";
    case CommandId::ToggleIndentGuides: return "ToggleIndentGuides";
    case CommandId::ZoomIn: return "ZoomIn";
    case CommandId::ZoomOut: return "ZoomOut";
    case CommandId::ZoomReset: return "ZoomReset";
    case CommandId::ToggleTheme: return "ToggleTheme";
    case CommandId::SyntaxColors: return "SyntaxColors";
    case CommandId::ChooseLanguage: return "ChooseLanguage";
    case CommandId::DetailFull: return "DetailFull";
    case CommandId::DetailBalanced: return "DetailBalanced";
    case CommandId::DetailOverview: return "DetailOverview";
    case CommandId::DetailOutline: return "DetailOutline";
    case CommandId::FoldAll: return "FoldAll";
    case CommandId::UnfoldAll: return "UnfoldAll";
    case CommandId::ToggleFold: return "ToggleFold";
    case CommandId::ToggleLayersMap: return "ToggleLayersMap";
    case CommandId::ShowLayers: return "ShowLayers";
    case CommandId::ShowMinimap: return "ShowMinimap";
    case CommandId::SummarizeFile: return "SummarizeFile";
    case CommandId::ShowCodeView: return "ShowCodeView";
    case CommandId::ShowMapView: return "ShowMapView";
    case CommandId::FoldingSyntax: return "FoldingSyntax";
    case CommandId::FoldingLogical: return "FoldingLogical";
    case CommandId::DissectProject: return "DissectProject";
    case CommandId::RefreshMap: return "RefreshMap";
    case CommandId::EolCrLf: return "EolCrLf";
    case CommandId::EolLf: return "EolLf";
    case CommandId::EolCr: return "EolCr";
    case CommandId::EncodingUtf8: return "EncodingUtf8";
    case CommandId::EncodingUtf8Bom: return "EncodingUtf8Bom";
    case CommandId::EncodingUtf16Le: return "EncodingUtf16Le";
    case CommandId::EncodingUtf16Be: return "EncodingUtf16Be";
    case CommandId::EncodingAnsi: return "EncodingAnsi";
    case CommandId::ToggleSmartIndent: return "ToggleSmartIndent";
    case CommandId::IndentWithSpaces: return "IndentWithSpaces";
    case CommandId::IndentWithTabs: return "IndentWithTabs";
    case CommandId::IndentWidth2: return "IndentWidth2";
    case CommandId::IndentWidth4: return "IndentWidth4";
    case CommandId::IndentWidth8: return "IndentWidth8";
    case CommandId::DetectIndentation: return "DetectIndentation";
    case CommandId::ConvertIndentation: return "ConvertIndentation";
    case CommandId::RunLint: return "RunLint";
    case CommandId::RunStaticAnalysis: return "RunStaticAnalysis";
    case CommandId::RunSecurityScan: return "RunSecurityScan";
    case CommandId::RunStyleCheck: return "RunStyleCheck";
    case CommandId::RunComplexity: return "RunComplexity";
    case CommandId::RunAgentReview: return "RunAgentReview";
    case CommandId::RunAllReviews: return "RunAllReviews";
    case CommandId::CompareWithSaved: return "CompareWithSaved";
    case CommandId::CompareAgentChanges: return "CompareAgentChanges";
    case CommandId::AddReviewComment: return "AddReviewComment";
    case CommandId::NextFinding: return "NextFinding";
    case CommandId::PreviousFinding: return "PreviousFinding";
    case CommandId::ClearFindings: return "ClearFindings";
    case CommandId::NewChat: return "NewChat";
    case CommandId::AskAgent: return "AskAgent";
    case CommandId::ExplainSelection: return "ExplainSelection";
    case CommandId::RefactorSelection: return "RefactorSelection";
    case CommandId::GenerateTests: return "GenerateTests";
    case CommandId::FixFindings: return "FixFindings";
    case CommandId::AcceptAllChanges: return "AcceptAllChanges";
    case CommandId::RejectAllChanges: return "RejectAllChanges";
    case CommandId::StopAgent: return "StopAgent";
    case CommandId::StartTour: return "StartTour";
    case CommandId::SwitchToAgentMode: return "SwitchToAgentMode";
    case CommandId::SwitchToCodeMode: return "SwitchToCodeMode";
    case CommandId::ToggleWorkspaceMode: return "ToggleWorkspaceMode";
    case CommandId::ToggleTerminal: return "ToggleTerminal";
    case CommandId::NewTerminal: return "NewTerminal";
    case CommandId::KillTerminal: return "KillTerminal";
    case CommandId::ClearTerminal: return "ClearTerminal";
    case CommandId::PopOutTerminal: return "PopOutTerminal";
    case CommandId::StartRecording: return "StartRecording";
    case CommandId::StopRecording: return "StopRecording";
    case CommandId::PlayMacro: return "PlayMacro";
    case CommandId::HashSha256: return "HashSha256";
    case CommandId::Preferences: return "Preferences";
    case CommandId::About: return "About";
    case CommandId::EngineLab: return "EngineLab";
    case CommandId::ShowGitStatus: return "ShowGitStatus";
    case CommandId::LocalModel: return "LocalModel";
    case CommandId::Exit: return "Exit";
    case CommandId::ShowGit: return "ShowGit";
    case CommandId::GitCommit: return "GitCommit";
    case CommandId::GitStageAll: return "GitStageAll";
    case CommandId::GitDiscardAll: return "GitDiscardAll";
    case CommandId::GitBranches: return "GitBranches";
    case CommandId::GitMerge: return "GitMerge";
    case CommandId::GitSync: return "GitSync";
    case CommandId::GitPull: return "GitPull";
    case CommandId::GitPush: return "GitPush";
    case CommandId::GitFetch: return "GitFetch";
    case CommandId::ShowGitHistory: return "ShowGitHistory";
    case CommandId::GitBlame: return "GitBlame";
    case CommandId::GitStash: return "GitStash";
    case CommandId::GitStashPop: return "GitStashPop";
    case CommandId::GitWriteMessage: return "GitWriteMessage";
    case CommandId::GitGuide: return "GitGuide";
    case CommandId::CloseFolder: return "CloseFolder";
    case CommandId::Count: break;
    }
    return "";
}

bool commandFromKey(std::string_view key, CommandId& id)
{
    for (int i = 0; i < int(CommandId::Count); ++i)
        if (key == commandKey(CommandId(i))) {
            id = CommandId(i);
            return true;
        }
    return false;
}

const CommandInfo& commandInfo(CommandId id)
{
    static const auto table = [] {
        struct Table {
            CommandInfo entries[int(CommandId::Count)];
        } t{};
        for (int i = 0; i < int(CommandId::Count); ++i)
            t.entries[i] = makeInfo(CommandId(i));
        return t;
    }();
    return table.entries[int(id)];
}

} // namespace enp::viewmodel
