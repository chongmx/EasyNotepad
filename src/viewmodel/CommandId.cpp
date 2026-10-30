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
    case CommandId::CloseFile: return {"Close", "Close the current document", "Ctrl+W"};

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
    case CommandId::ToggleReviewPanel: return {"Review Panel", "Show or hide the review panel", "Ctrl+Shift+R"};
    case CommandId::ToggleAgentPanel: return {"Agent Panel", "Show or hide the agent side panel", "Ctrl+Shift+I"};
    case CommandId::ToggleWordWrap: return {"Word Wrap", "Wrap long lines", "Alt+Z"};
    case CommandId::ToggleWhitespace: return {"Whitespace", "Show spaces and tabs", ""};
    case CommandId::ToggleLineNumbers: return {"Line Numbers", "Show line numbers", ""};
    case CommandId::ToggleIndentGuides: return {"Guides", "Show indent guides", ""};
    case CommandId::ZoomIn: return {"Zoom In", "Increase the text size", "Ctrl+="};
    case CommandId::ZoomOut: return {"Zoom Out", "Decrease the text size", "Ctrl+-"};
    case CommandId::ZoomReset: return {"100%", "Restore the default text size", "Ctrl+0"};
    case CommandId::ToggleTheme: return {"Theme", "Switch between dark and light theme", ""};

    case CommandId::EolCrLf: return {"CRLF", "Windows line endings (CR LF)", ""};
    case CommandId::EolLf: return {"LF", "Unix line endings (LF)", ""};
    case CommandId::EolCr: return {"CR", "Classic Mac line endings (CR)", ""};
    case CommandId::EncodingUtf8: return {"UTF-8", "Encode in UTF-8", ""};
    case CommandId::EncodingUtf8Bom: return {"UTF-8 BOM", "Encode in UTF-8 with byte order mark", ""};
    case CommandId::EncodingUtf16Le: return {"UTF-16 LE", "Encode in UTF-16 little endian", ""};
    case CommandId::EncodingUtf16Be: return {"UTF-16 BE", "Encode in UTF-16 big endian", ""};
    case CommandId::EncodingAnsi: return {"ANSI", "Encode in the system code page", ""};
    case CommandId::LanguagePlainText: return {"Text", "Plain text, no highlighting", ""};
    case CommandId::LanguageCpp: return {"C++", "C / C++ highlighting", ""};
    case CommandId::LanguagePython: return {"Python", "Python highlighting", ""};
    case CommandId::LanguageJavaScript: return {"JavaScript", "JavaScript / TypeScript highlighting", ""};
    case CommandId::LanguageMarkdown: return {"Markdown", "Markdown highlighting", ""};
    case CommandId::LanguageJson: return {"JSON", "JSON highlighting", ""};
    case CommandId::LanguageCMake: return {"CMake", "CMake highlighting", ""};

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

    case CommandId::SwitchToAgentMode: return {"Agent", "Agent-centric workspace: describe the goal, review the changes", ""};
    case CommandId::SwitchToCodeMode: return {"Code", "Manual coding workspace: classic editor with assistant on the side", ""};
    case CommandId::ToggleWorkspaceMode: return {"Switch Mode", "Toggle between Agent and Code workspaces", "Ctrl+Shift+M"};

    case CommandId::StartRecording: return {"Record", "Start recording a macro", ""};
    case CommandId::StopRecording: return {"Stop", "Stop recording", ""};
    case CommandId::PlayMacro: return {"Play", "Play back the recorded macro", ""};
    case CommandId::HashSha256: return {"SHA-256", "Show the SHA-256 hash of the selection", ""};
    case CommandId::Preferences: return {"Settings", "Preferences", ""};
    case CommandId::About: return {"About", "About EasyNotepad", ""};
    case CommandId::Exit: return {"Exit", "Quit EasyNotepad", "Alt+F4"};

    case CommandId::Count: break;
    }
    return {"", "", ""};
}

} // namespace

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
