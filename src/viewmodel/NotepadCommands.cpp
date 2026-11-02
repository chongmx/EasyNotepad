// NotepadViewModel: every command and what it is bound to (split from NotepadViewModel.cpp).
#include "enp/viewmodel/NotepadViewModel.h"

#include "enp/model/Hash.h"
#include "enp/model/Json.h"
#include "enp/viewmodel/DummyData.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace enp::viewmodel {

namespace {

constexpr int kIndentWidth = 4;
constexpr float kMinZoom = 0.5f;
constexpr float kMaxZoom = 3.0f;
constexpr size_t kMaxUndo = 500;

} // namespace

// ---- Commands -----------------------------------------------------------------------------------

void NotepadViewModel::set(CommandId id, std::function<void()> execute, std::function<bool()> canExecute)
{
    commands_[size_t(id)] = Command(std::move(execute), std::move(canExecute));
}

void NotepadViewModel::notImplemented(CommandId id)
{
    statusText.set(std::string(commandInfo(id).label) + ": not available yet (no services connected)");
}

void NotepadViewModel::initCommands()
{
    using C = CommandId;
    auto hasDoc = [this]() { return activeDocument() != nullptr; };
    auto hasSelection = [this]() { return activeDocument() && activeDocument()->hasSelection(); };

    set(C::NewFile, [this]() { newDocument(); });
    // The dialog starts in the active document's folder, else the workspace, else home.
    auto startFolder = [this]() {
        const model::Document* doc = activeDocument();
        if (doc && !doc->path.empty() && (files_ != nullptr))
            return FileDialogViewModel::parent(absolutePath(doc->path));
        return workspaceRoot_.empty() ? homeDirectory() : workspaceRoot_;
    };
    set(C::OpenFile, [this, startFolder]() { fileDialog_.show(FileDialogMode::Open, startFolder()); },
        [this]() { return files_ != nullptr; });
    set(C::CloseFolder, [this]() { requestCloseFolder(); }, [this]() { return !workspaceRoot_.empty(); });
    set(C::OpenFolder, [this]() {
        fileDialog_.show(FileDialogMode::OpenFolder, workspaceRoot_.empty() ? homeDirectory() : FileDialogViewModel::parent(workspaceRoot_));
    }, [this]() { return files_ != nullptr; });
    set(C::SaveAs, [this, startFolder]() {
        const model::Document* doc = activeDocument();
        fileDialog_.show(FileDialogMode::SaveAs, startFolder(), doc->path.empty() ? doc->title + ".txt" : doc->title);
    }, [this]() { return files_ != nullptr && activeDocument() != nullptr; });
    set(C::Save, [this]() {
        model::Document& doc = *activeDocument();
        if (doc.path.empty() && files_)
            execute(C::SaveAs); // untitled: choose where
        else
            saveDocument(doc);
    }, hasDoc);
    set(C::SaveAll, [this]() {
        int saved = 0;
        for (auto& doc : documents_)
            if (doc->modified && saveDocument(*doc))
                ++saved;
        statusText.set("Saved " + std::to_string(saved) + " document(s)");
    }, hasDoc);
    set(C::CloseFile, [this]() { requestCloseDocument(activeIndex.get()); }, hasDoc);
    set(C::CloseOthers, [this]() { closeOthers(activeIndex.get()); }, [this]() { return documents_.size() > 1; });
    set(C::CloseAll, [this]() { closeAll(); }, hasDoc);
    set(C::CloseSaved, [this]() { closeSaved(); }, hasDoc);
    set(C::NextDocument, [this]() {
        activate((activeIndex.get() + 1) % int(documents_.size()));
    }, [this]() { return documents_.size() > 1; });
    set(C::PreviousDocument, [this]() {
        activate((activeIndex.get() + int(documents_.size()) - 1) % int(documents_.size()));
    }, [this]() { return documents_.size() > 1; });

    set(C::Cut, [this]() { clipboardRequested.emit(ClipboardAction::Cut); }, hasSelection);
    set(C::Copy, [this]() { clipboardRequested.emit(ClipboardAction::Copy); }, hasSelection);
    set(C::Paste, [this]() { clipboardRequested.emit(ClipboardAction::Paste); }, hasDoc);
    set(C::Undo, [this]() { undo(); }, [this]() { return canUndo(); });
    set(C::Redo, [this]() { redo(); }, [this]() { return canRedo(); });
    set(C::SelectAll, [this]() { selectAll(); }, hasDoc);

    set(C::DuplicateLine, [this]() { duplicateLines(); }, hasDoc);
    set(C::DeleteLine, [this]() { deleteLines(); }, hasDoc);
    set(C::MoveLineUp, [this]() { moveLines(-1); }, hasDoc);
    set(C::MoveLineDown, [this]() { moveLines(+1); }, hasDoc);
    set(C::SortLines, [this]() {
        transformLines([](std::vector<std::string>& lines) { std::sort(lines.begin(), lines.end()); });
    }, hasDoc);
    set(C::UpperCase, [this]() {
        transformSelection([](std::string s) {
            for (char& c : s)
                c = char(std::toupper(static_cast<unsigned char>(c)));
            return s;
        });
    }, hasDoc);
    set(C::LowerCase, [this]() {
        transformSelection([](std::string s) {
            for (char& c : s)
                c = char(std::tolower(static_cast<unsigned char>(c)));
            return s;
        });
    }, hasDoc);
    set(C::ToggleComment, [this]() { toggleComment(); }, hasDoc);
    set(C::Indent, [this]() {
        const std::string unit = model::indentUnit(activeDocument()->indent);
        transformLines([unit](std::vector<std::string>& lines) {
            for (std::string& l : lines)
                if (!l.empty())
                    l.insert(0, unit);
        });
    }, hasDoc);
    set(C::Outdent, [this]() {
        const int width = activeDocument()->indent.width;
        transformLines([width](std::vector<std::string>& lines) {
            for (std::string& l : lines) {
                if (!l.empty() && l[0] == '\t') {
                    l.erase(0, 1);
                    continue;
                }
                size_t n = 0;
                while (n < l.size() && n < size_t(width) && l[n] == ' ')
                    ++n;
                l.erase(0, n);
            }
        });
    }, hasDoc);
    set(C::TrimTrailing, [this]() { trimTrailing(); }, hasDoc);

    set(C::Find, [this]() {
        if (model::Document* doc = activeDocument(); doc && doc->hasSelection() && doc->anchor.line == doc->caret.line)
            findQuery.set(selectedText());
        findVisible.set(true);
        replaceVisible.set(false);
        focusFindRequested.emit();
    }, hasDoc);
    set(C::Replace, [this]() {
        findVisible.set(true);
        replaceVisible.set(true);
        focusFindRequested.emit();
    }, hasDoc);
    set(C::FindNext, [this]() { findNext(+1); }, hasDoc);
    set(C::FindPrevious, [this]() { findNext(-1); }, hasDoc);
    set(C::FindInFiles, [this]() {
        // The selection (one line) becomes the query.
        if (model::Document* doc = activeDocument(); doc && doc->hasSelection() && doc->anchor.line == doc->caret.line)
            findInFiles_.query = selectedText();
        showPanelTab(PanelTab::Search);
        searchFocusRequested.emit();
        if (!findInFiles_.query.empty())
            findInFiles_.search();
    });
    set(C::GoToLine, [this]() { goToLineRequested.emit(); }, hasDoc);
    set(C::ToggleBookmark, [this]() { toggleBookmark(); }, hasDoc);
    set(C::NextBookmark, [this]() { jumpToBookmark(+1); }, hasDoc);
    set(C::PreviousBookmark, [this]() { jumpToBookmark(-1); }, hasDoc);
    set(C::ClearBookmarks, [this]() { activeDocument()->bookmarks.clear(); }, hasDoc);

    set(C::ToggleExplorer, [this]() { showExplorer.set(!showExplorer.get()); });
    set(C::TogglePanel, [this]() {
        showPanel.set(!showPanel.get());
        if (!showPanel.get())
            panelMaximized.set(false);
        else if (panelTab.get() == PanelTab::Terminal)
            showPanelTab(PanelTab::Terminal);
    });
    set(C::ToggleSlimRibbon, [this]() { slimRibbon.set(!slimRibbon.get()); });
    set(C::ToggleMaximizePanel, [this]() {
        showPanel.set(true);
        panelMaximized.set(!panelMaximized.get());
    });
    set(C::ToggleTerminal, [this]() {
        if (showPanel.get() && panelTab.get() == PanelTab::Terminal) {
            showPanel.set(false);
            panelMaximized.set(false);
        } else {
            showPanelTab(PanelTab::Terminal);
        }
    });
    set(C::NewTerminal, [this]() {
        showPanel.set(true);
        panelTab.set(PanelTab::Terminal);
        terminal_.newTerminal(0);
    }, [this]() { return terminal_.supported(); });
    set(C::KillTerminal, [this]() { terminal_.killActive(); }, [this]() { return terminal_.active() != nullptr; });
    set(C::ClearTerminal, [this]() { terminal_.clearActive(); }, [this]() { return terminal_.active() != nullptr; });
    set(C::PopOutTerminal, [this]() {
        terminalPoppedOut.set(!terminalPoppedOut.get());
        if (terminalPoppedOut.get() && terminal_.supported() && !terminal_.active())
            terminal_.newTerminal(0);
    }, [this]() { return terminal_.supported(); });
    set(C::ToggleAgentPanel, [this]() { showAgentPanel.set(!showAgentPanel.get()); });
    set(C::ToggleWordWrap, [this]() { wordWrap.set(!wordWrap.get()); });
    // Markdown and CSV: rendered or text. JSON and YAML: text, tree, accordion in turn.
    set(C::ToggleMarkdownView, [this]() {
        model::Document* doc = activeDocument();
        if (doc->language == model::Language::Json || doc->language == model::Language::Yaml) {
            setDataView(doc->dataView == model::DataView::Text   ? model::DataView::Tree
                        : doc->dataView == model::DataView::Tree ? model::DataView::Accordion
                                                                 : model::DataView::Text);
            return;
        }
        doc->rendered = !doc->rendered;
        if (!doc->rendered)
            revealLine(doc->caret.line);
    }, [this]() {
        const model::Document* doc = activeDocument();
        if (!doc || doc->kind != model::FileKind::Text)
            return false;
        const model::Language l = doc->language;
        return l == model::Language::Markdown || l == model::Language::Csv || l == model::Language::Json || l == model::Language::Yaml;
    });
    set(C::ToggleWhitespace, [this]() { showWhitespace.set(!showWhitespace.get()); });
    set(C::ToggleLineNumbers, [this]() { showLineNumbers.set(!showLineNumbers.get()); });
    set(C::ToggleIndentGuides, [this]() { showIndentGuides.set(!showIndentGuides.get()); });
    set(C::ZoomIn, [this]() { setZoom(zoom.get() + 0.1f); }, [this]() { return zoom.get() < kMaxZoom; });
    set(C::ZoomOut, [this]() { setZoom(zoom.get() - 0.1f); }, [this]() { return zoom.get() > kMinZoom; });
    set(C::ZoomReset, [this]() { setZoom(1.0f); });
    set(C::ToggleTheme, [this]() { darkTheme.set(!darkTheme.get()); });
    set(C::SyntaxColors, [this]() { syntaxColorsRequested.emit(); });

    set(C::DetailFull, [this]() { detailLevel.set(0); }, hasDoc);
    set(C::DetailBalanced, [this]() { detailLevel.set(1); }, hasDoc);
    set(C::DetailOverview, [this]() { detailLevel.set(2); }, hasDoc);
    set(C::DetailOutline, [this]() { detailLevel.set(3); }, hasDoc);
    set(C::FoldAll, [this]() { foldAll(); }, hasDoc);
    set(C::UnfoldAll, [this]() { unfoldAll(); }, hasDoc);
    set(C::ToggleFold, [this]() {
        // The innermost region around the caret.
        const model::Document* doc = activeDocument();
        const model::OutlineNode* best = nullptr;
        for (const model::OutlineNode& n : outline())
            if (n.startLine <= doc->caret.line && doc->caret.line <= n.endLine)
                best = &n;
        if (best)
            toggleFold(best->startLine);
    }, hasDoc);
    set(C::ToggleLayersMap, [this]() { showLayersMap.set(!showLayersMap.get()); });
    set(C::ShowCodeView, [this]() { editorMode.set(EditorMode::Code); });
    set(C::ShowMapView, [this]() { editorMode.set(EditorMode::Map); });
    set(C::FoldingSyntax, [this]() { foldingMode.set(model::OutlineMode::Syntax); });
    set(C::FoldingLogical, [this]() { foldingMode.set(model::OutlineMode::Logical); });
    set(C::DissectProject, [this]() { logicalMap_.dissectWithAgent(); }, [this]() { return !logicalMap_.analyzing.get(); });
    set(C::RefreshMap, [this]() { logicalMap_.refresh(); });
    set(C::ShowLayers, [this]() {
        sideBar.set(SideBar::Layers);
        showLayersMap.set(true);
    });
    set(C::ShowMinimap, [this]() {
        sideBar.set(SideBar::Minimap);
        showLayersMap.set(true);
    });
    set(C::SummarizeFile, [this]() {
        outline_.summaries.clear();
        statusText.set(workspaceRoot_.empty() ? "Summaries refreshed (sample data)"
                                              : "Summaries refreshed from the code's comments and structure");
    }, hasDoc);

    auto setEol = [this](model::EolMode eol) {
        return [this, eol]() {
            activeDocument()->eol = eol;
            statusText.set(std::string("Line endings: ") + model::eolName(eol));
        };
    };
    set(C::EolCrLf, setEol(model::EolMode::CrLf), hasDoc);
    set(C::EolLf, setEol(model::EolMode::Lf), hasDoc);
    set(C::EolCr, setEol(model::EolMode::Cr), hasDoc);
    auto setEncoding = [this](model::Encoding encoding) {
        return [this, encoding]() {
            activeDocument()->encoding = encoding;
            statusText.set(std::string("Encoding: ") + model::encodingName(encoding));
        };
    };
    set(C::EncodingUtf8, setEncoding(model::Encoding::Utf8), hasDoc);
    set(C::EncodingUtf8Bom, setEncoding(model::Encoding::Utf8Bom), hasDoc);
    set(C::EncodingUtf16Le, setEncoding(model::Encoding::Utf16Le), hasDoc);
    set(C::EncodingUtf16Be, setEncoding(model::Encoding::Utf16Be), hasDoc);
    set(C::EncodingAnsi, setEncoding(model::Encoding::Ansi), hasDoc);
    set(C::ToggleSmartIndent, [this]() { smartIndent.set(!smartIndent.get()); });
    auto withStyle = [this](auto change) {
        return [this, change]() {
            model::IndentStyle style = activeDocument()->indent;
            change(style);
            setIndentStyle(style);
        };
    };
    set(C::IndentWithSpaces, withStyle([](model::IndentStyle& s) { s.tabs = false; }), hasDoc);
    set(C::IndentWithTabs, withStyle([](model::IndentStyle& s) { s.tabs = true; }), hasDoc);
    set(C::IndentWidth2, withStyle([](model::IndentStyle& s) { s.width = 2; }), hasDoc);
    set(C::IndentWidth4, withStyle([](model::IndentStyle& s) { s.width = 4; }), hasDoc);
    set(C::IndentWidth8, withStyle([](model::IndentStyle& s) { s.width = 8; }), hasDoc);
    set(C::DetectIndentation, [this]() {
        model::Document& doc = *activeDocument();
        applyDetectedIndent(doc);
        statusText.set(std::string(doc.indentDetected ? "Detected: " : "Nothing indented yet; using the language default: ") +
                       (doc.indent.tabs ? "tabs" : std::to_string(doc.indent.width) + " spaces"));
    }, hasDoc);
    set(C::ConvertIndentation, [this]() {
        const model::IndentStyle style = activeDocument()->indent;
        transformLines([style](std::vector<std::string>& lines) {
            for (std::string& l : lines) {
                const std::string ws = model::leadingWhitespace(l);
                l = model::makeIndent(model::indentColumns(ws, style.width), style) + l.substr(ws.size());
            }
        }, true);
    }, hasDoc);
    set(C::ChooseLanguage, [this]() { languagePickerRequested.emit(); }, hasDoc);

    auto runTool = [this](model::ReviewTool tool) {
        return [this, tool]() {
            review_.runTool(tool);
            showPanel.set(true);
            panelTab.set(PanelTab::Problems);
        };
    };
    set(C::RunLint, runTool(model::ReviewTool::Lint));
    set(C::RunStaticAnalysis, runTool(model::ReviewTool::StaticAnalysis));
    set(C::RunSecurityScan, runTool(model::ReviewTool::Security));
    set(C::RunStyleCheck, runTool(model::ReviewTool::Style));
    set(C::RunComplexity, runTool(model::ReviewTool::Complexity));
    set(C::RunAgentReview, runTool(model::ReviewTool::AgentReview));
    set(C::RunAllReviews, [this]() {
        review_.runAll();
        showPanel.set(true);
        panelTab.set(PanelTab::Problems);
    });
    set(C::CompareWithSaved, [this]() { compareWithSaved(); }, hasDoc);
    set(C::CompareAgentChanges, [this]() {
        if (const model::FileChange* change = agent_.currentChange())
            review_.setDiff("Agent change: " + change->path, change->diff);
        showPanel.set(true);
        panelTab.set(PanelTab::Diff);
    }, [this]() { return agent_.currentChange() != nullptr; });
    set(C::AddReviewComment, [this]() { addCommentRequested.emit(); }, hasDoc);
    set(C::NextFinding, [this]() { review_.nextFinding(+1); });
    set(C::PreviousFinding, [this]() { review_.nextFinding(-1); });
    set(C::ClearFindings, [this]() { review_.clearFindings(); }, [this]() { return !review_.findings().empty(); });

    auto notBusy = [this]() { return !agent_.busy.get(); };
    set(C::NewChat, [this]() { agent_.newChat(); });
    set(C::AskAgent, [this]() {
        if (workspace.get() == WorkspaceMode::Code)
            showAgentPanel.set(true);
        focusPromptRequested.emit();
    });
    set(C::ExplainSelection, [this]() { askAgent("Explain this code."); }, [=]() { return hasDoc() && notBusy(); });
    set(C::RefactorSelection, [this]() { askAgent("Refactor this code for readability without changing behaviour."); },
        [=]() { return hasDoc() && notBusy(); });
    set(C::GenerateTests, [this]() { askAgent("Write unit tests for this file."); }, [=]() { return hasDoc() && notBusy(); });
    set(C::FixFindings, [this]() {
        askAgent("Fix the open review findings (" + std::to_string(review_.visibleFindings().size()) + ").");
    }, notBusy);
    set(C::AcceptAllChanges, [this]() { agent_.acceptAll(); }, [this]() { return agent_.pendingChangeCount() > 0; });
    set(C::RejectAllChanges, [this]() { agent_.rejectAll(); }, [this]() { return agent_.pendingChangeCount() > 0; });
    // Stops every agent run: the chat, a project dissection, a commit message being written.
    set(C::StopAgent, [this]() {
        agent_.stop();
        if (agentCli_)
            agentCli_->cancel();
    }, [this]() { return agent_.busy.get() || logicalMap_.analyzing.get() || git_.writingMessage(); });
    set(C::StartTour, [this]() {
        if (tourRunning())
            stopTour();
        else
            startTour();
    });

    set(C::SwitchToAgentMode, [this]() { workspace.set(WorkspaceMode::Agent); });
    set(C::SwitchToCodeMode, [this]() { workspace.set(WorkspaceMode::Code); });
    set(C::ToggleWorkspaceMode, [this]() {
        workspace.set(workspace.get() == WorkspaceMode::Agent ? WorkspaceMode::Code : WorkspaceMode::Agent);
    });

    set(C::StartRecording, [this]() {
        macroRecording_.clear();
        recording.set(true);
        statusText.set("Recording a macro: type and edit, then Stop");
    }, [this]() { return !recording.get(); });
    set(C::StopRecording, [this]() {
        recording.set(false);
        if (macroRecording_.empty()) {
            statusText.set("Nothing was recorded; the previous macro is kept");
            return;
        }
        macro_ = std::move(macroRecording_);
        macroRecording_.clear();
        settings_.markDirty();
        statusText.set("Recorded a macro of " + std::to_string(macro_.size()) + " step(s): Play to repeat it");
    }, [this]() { return recording.get(); });
    set(C::PlayMacro, [this]() { playMacro(); }, [this]() { return !macro_.empty() && !recording.get() && activeDocument(); });
    set(C::HashSha256, [this]() {
        const model::Document* doc = activeDocument();
        const std::string text = doc->hasSelection() ? selectedText() : doc->buffer.text();
        hashResult.set(model::sha256Hex(text));
        statusText.set(std::string("SHA-256 of the ") + (doc->hasSelection() ? "selection" : "document") + ": " + hashResult.get());
    }, hasDoc);
    // Git (GitViewModel; demo data while the guide runs or no folder is open).
    auto gitReady = [this]() { return git_.available() && git_.repo().repository; };
    auto hasChanges = [this, gitReady]() { return gitReady() && git_.changeCount() > 0; };
    set(C::ShowGit, [this]() {
        showExplorer.set(true);
        gitTabRequested.emit();
    });
    set(C::GitCommit, [this]() {
        if (git_.commitMessage.empty()) {
            showExplorer.set(true);
            gitTabRequested.emit();
        }
        git_.commitWithMessage();
    }, hasChanges);
    set(C::GitStageAll, [this]() { git_.stage(git_.unstagedPaths()); }, hasChanges);
    set(C::GitDiscardAll, [this]() { gitDiscardAllRequested.emit(); }, hasChanges);
    set(C::GitBranches, [this]() { branchPickerRequested.emit(); }, gitReady);
    set(C::GitMerge, [this]() {
        branchPickerRequested.emit();
        statusText.set("Right-click the branch to merge and choose Merge into current");
    }, gitReady);
    set(C::GitSync, [this]() { git_.sync(); }, gitReady);
    set(C::GitPull, [this]() { git_.pull(); }, gitReady);
    set(C::GitPush, [this]() { git_.push(); }, gitReady);
    set(C::GitFetch, [this]() { git_.fetch(); }, gitReady);
    set(C::GitStash, [this]() { gitStashRequested.emit(); }, hasChanges);
    set(C::GitStashPop, [this]() { git_.stashApply(0, true); }, [this, gitReady]() { return gitReady() && !git_.repo().stashes.empty(); });
    set(C::GitWriteMessage, [this]() {
        showExplorer.set(true);
        gitTabRequested.emit();
        git_.writeMessage();
    }, hasChanges);
    set(C::GitBlame, [this]() {
        if (const model::Document* doc = activeDocument())
            git_.toggleBlame(doc->path, doc->buffer.text());
    }, [this, gitReady]() { return gitReady() && activeDocument() && activeDocument()->onDisk; });
    set(C::GitGuide, [this]() { gitGuideRequested.emit(); });
    set(C::ShowGitHistory, [this]() { showPanelTab(PanelTab::History); });
    set(C::ShowGitStatus, [this]() { showPanelTab(PanelTab::Status); });
    // The agent set-up card, where the local model is downloaded (open to everyone, not only developers).
    set(C::LocalModel, [this]() {
        if (workspace.get() == WorkspaceMode::Code)
            showAgentPanel.set(true);
        agent_.showSetup.set(true);
        localModelRequested.emit();
    });
    set(C::Preferences, [this]() { preferencesRequested.emit(); });
    set(C::About, [this]() { aboutRequested.emit(); });
    set(C::EngineLab, [this]() { engineLabRequested.emit(); }, [this]() { return developerMode.get(); });
    set(C::Exit, [this]() { requestQuit(); });
}

void NotepadViewModel::execute(CommandId id) const
{
    MacroScope scope(*this);
    // Editing commands are part of a macro; navigation, files and the recorder itself are not.
    static const CommandId kRecorded[] = {CommandId::DuplicateLine, CommandId::DeleteLine, CommandId::MoveLineUp,
                                          CommandId::MoveLineDown, CommandId::SortLines, CommandId::UpperCase,
                                          CommandId::LowerCase, CommandId::ToggleComment, CommandId::Indent,
                                          CommandId::Outdent, CommandId::TrimTrailing, CommandId::SelectAll};
    if (scope.outer && std::find(std::begin(kRecorded), std::end(kRecorded), id) != std::end(kRecorded))
        recordMacro({MacroStep::Kind::Command, {}, 0, 0, false, id});
    command(id).execute();
}

bool NotepadViewModel::isChecked(CommandId id) const
{
    using C = CommandId;
    const model::Document* doc = activeDocument();
    switch (id) {
    case C::ToggleExplorer: return showExplorer.get();
    case C::TogglePanel: return showPanel.get();
    case C::ToggleTerminal: return showPanel.get() && panelTab.get() == PanelTab::Terminal;
    case C::ToggleMaximizePanel: return panelMaximized.get();
    case C::ToggleSlimRibbon: return slimRibbon.get();
    case C::ToggleAgentPanel: return showAgentPanel.get();
    case C::ToggleWordWrap: return wordWrap.get();
    case C::ToggleMarkdownView: return activeDocument() && (activeDocument()->rendered || activeDocument()->dataView != model::DataView::Text);
    case C::ToggleWhitespace: return showWhitespace.get();
    case C::ToggleLineNumbers: return showLineNumbers.get();
    case C::ToggleIndentGuides: return showIndentGuides.get();
    case C::ToggleTheme: return darkTheme.get();
    case C::DetailFull: return detailLevel.get() == 0;
    case C::DetailBalanced: return detailLevel.get() == 1;
    case C::DetailOverview: return detailLevel.get() == 2;
    case C::DetailOutline: return detailLevel.get() == 3;
    case C::ToggleLayersMap: return showLayersMap.get();
    case C::ShowCodeView: return editorMode.get() == EditorMode::Code;
    case C::ShowMapView: return editorMode.get() == EditorMode::Map;
    case C::FoldingSyntax: return foldingMode.get() == model::OutlineMode::Syntax;
    case C::FoldingLogical: return foldingMode.get() == model::OutlineMode::Logical;
    case C::GitBlame: return doc && git_.blameShown(doc->path);
    case C::ShowGitHistory: return showPanel.get() && panelTab.get() == PanelTab::History;
    case C::ShowGitStatus: return showPanel.get() && panelTab.get() == PanelTab::Status;
    case C::DissectProject: return logicalMap_.analyzing.get();
    case C::ShowLayers: return showLayersMap.get() && sideBar.get() == SideBar::Layers;
    case C::ShowMinimap: return showLayersMap.get() && sideBar.get() == SideBar::Minimap;
    case C::StartTour: return tourRunning();
    case C::StartRecording: return recording.get();
    case C::SwitchToAgentMode: return workspace.get() == WorkspaceMode::Agent;
    case C::SwitchToCodeMode: return workspace.get() == WorkspaceMode::Code;
    case C::EolCrLf: return doc && doc->eol == model::EolMode::CrLf;
    case C::EolLf: return doc && doc->eol == model::EolMode::Lf;
    case C::EolCr: return doc && doc->eol == model::EolMode::Cr;
    case C::EncodingUtf8: return doc && doc->encoding == model::Encoding::Utf8;
    case C::EncodingUtf8Bom: return doc && doc->encoding == model::Encoding::Utf8Bom;
    case C::EncodingUtf16Le: return doc && doc->encoding == model::Encoding::Utf16Le;
    case C::EncodingUtf16Be: return doc && doc->encoding == model::Encoding::Utf16Be;
    case C::EncodingAnsi: return doc && doc->encoding == model::Encoding::Ansi;
    case C::PopOutTerminal: return terminalPoppedOut.get();
    case C::ToggleSmartIndent: return smartIndent.get();
    case C::IndentWithSpaces: return doc && !doc->indent.tabs;
    case C::IndentWithTabs: return doc && doc->indent.tabs;
    case C::IndentWidth2: return doc && doc->indent.width == 2;
    case C::IndentWidth4: return doc && doc->indent.width == 4;
    case C::IndentWidth8: return doc && doc->indent.width == 8;
    default: return false;
    }
}

} // namespace enp::viewmodel
