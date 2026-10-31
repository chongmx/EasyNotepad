// NotepadViewModel: operations on whole lines and selections (split from NotepadViewModel.cpp).
#include "enp/viewmodel/NotepadViewModel.h"

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

const char* lineCommentToken(model::Language language)
{
    const std::string& token = model::languageSpec(language).lineComment;
    return token.empty() ? nullptr : token.c_str();
}

std::string leadingWhitespace(const std::string& line)
{
    size_t n = 0;
    while (n < line.size() && (line[n] == ' ' || line[n] == '\t'))
        ++n;
    return line.substr(0, n);
}

} // namespace

// ---- Line operations ----------------------------------------------------------------------------

void NotepadViewModel::selectedLines(int& first, int& last) const
{
    const model::Document* doc = activeDocument();
    first = doc->selectionStart().line;
    last = doc->selectionEnd().line;
    // A selection ending at the start of a line does not include that line.
    if (last > first && doc->selectionEnd().column == 0)
        --last;
}

void NotepadViewModel::transformLines(const std::function<void(std::vector<std::string>&)>& transform, bool wholeDocument)
{
    model::Document* doc = activeDocument();
    int first = 0, last = 0;
    selectedLines(first, last);
    if (wholeDocument) {
        first = 0;
        last = doc->buffer.lineCount() - 1;
    }
    const bool hadSelection = doc->hasSelection() && !wholeDocument;
    std::vector<std::string> lines;
    for (int l = first; l <= last; ++l)
        lines.push_back(doc->buffer.line(l));

    const std::vector<std::string> original = lines;
    transform(lines);
    if (lines.empty())
        lines.emplace_back();
    if (lines == original)
        return; // nothing to change: no edit, no undo step
    beginEdit(EditKind::Other);
    if (first == 0 && last == doc->buffer.lineCount() - 1) {
        std::string joined;
        for (size_t k = 0; k < lines.size(); ++k)
            joined += (k ? "\n" : "") + lines[k];
        doc->buffer.setText(joined);
    } else {
        for (int l = last; l >= first; --l)
            doc->buffer.eraseLine(l);
        for (size_t k = 0; k < lines.size(); ++k)
            doc->buffer.insertLine(first + int(k), lines[k]);
    }
    const int newLast = first + int(lines.size()) - 1;
    if (hadSelection)
        setSelection({first, 0}, {newLast, int(doc->buffer.line(newLast).size())});
    else
        setCaret(doc->caret, false);
    endEdit();
}

void NotepadViewModel::transformSelection(const std::function<std::string(const std::string&)>& transform)
{
    model::Document* doc = activeDocument();
    if (!doc->hasSelection()) {
        transformLines([&](std::vector<std::string>& lines) {
            for (std::string& l : lines)
                l = transform(l);
        });
        return;
    }
    const model::TextPos start = doc->selectionStart();
    const std::string replaced = transform(selectedText());
    if (replaced == selectedText())
        return; // already so: no edit, no undo step
    insertText(replaced);
    setSelection(start, doc->caret);
}

void NotepadViewModel::duplicateLines()
{
    model::Document* doc = activeDocument();
    int first = 0, last = 0;
    selectedLines(first, last);
    beginEdit(EditKind::Other);
    for (int l = last; l >= first; --l)
        doc->buffer.insertLine(last + 1, doc->buffer.line(l));
    const int count = last - first + 1;
    doc->caret.line += count;
    doc->anchor.line += count;
    endEdit();
}

void NotepadViewModel::deleteLines()
{
    model::Document* doc = activeDocument();
    int first = 0, last = 0;
    selectedLines(first, last);
    beginEdit(EditKind::Other);
    for (int l = last; l >= first; --l)
        doc->buffer.eraseLine(l);
    setCaret({first, 0}, false);
    endEdit();
}

void NotepadViewModel::moveLines(int direction)
{
    model::Document* doc = activeDocument();
    int first = 0, last = 0;
    selectedLines(first, last);
    if ((direction < 0 && first == 0) || (direction > 0 && last + 1 >= doc->buffer.lineCount()))
        return;
    beginEdit(EditKind::Other);
    if (direction < 0) {
        std::string above = doc->buffer.line(first - 1);
        doc->buffer.eraseLine(first - 1);
        doc->buffer.insertLine(last, std::move(above));
    } else {
        std::string below = doc->buffer.line(last + 1);
        doc->buffer.eraseLine(last + 1);
        doc->buffer.insertLine(first, std::move(below));
    }
    doc->caret.line += direction;
    doc->anchor.line += direction;
    endEdit();
}

void NotepadViewModel::toggleComment()
{
    model::Document* doc = activeDocument();
    const char* token = lineCommentToken(doc->language);
    if (!token) {
        statusText.set(std::string("No line comment syntax for ") + model::languageName(doc->language));
        return;
    }
    const std::string prefix = std::string(token) + " ";
    transformLines([&](std::vector<std::string>& lines) {
        bool allCommented = true;
        size_t indent = std::string::npos;
        for (const std::string& l : lines) {
            const size_t ws = leadingWhitespace(l).size();
            if (ws == l.size())
                continue; // blank lines do not count
            indent = std::min(indent, ws);
            if (l.compare(ws, std::strlen(token), token) != 0)
                allCommented = false;
        }
        if (indent == std::string::npos)
            return;
        for (std::string& l : lines) {
            const size_t ws = leadingWhitespace(l).size();
            if (ws == l.size())
                continue;
            if (allCommented) {
                const size_t len = l.compare(ws, prefix.size(), prefix) == 0 ? prefix.size() : std::strlen(token);
                l.erase(ws, len);
            } else {
                l.insert(indent, prefix);
            }
        }
    });
}

void NotepadViewModel::trimTrailing()
{
    model::Document* doc = activeDocument();
    beginEdit(EditKind::Other);
    for (int l = 0; l < doc->buffer.lineCount(); ++l) {
        std::string line = doc->buffer.line(l);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        doc->buffer.setLine(l, std::move(line));
    }
    doc->caret = doc->buffer.clamp(doc->caret);
    doc->anchor = doc->buffer.clamp(doc->anchor);
    endEdit();
}

void NotepadViewModel::toggleBookmark()
{
    model::Document* doc = activeDocument();
    const int line = doc->caret.line;
    if (!doc->bookmarks.erase(line))
        doc->bookmarks.insert(line);
}

void NotepadViewModel::jumpToBookmark(int direction)
{
    model::Document* doc = activeDocument();
    if (doc->bookmarks.empty()) {
        statusText.set("No bookmarks in " + doc->title);
        return;
    }
    const int line = doc->caret.line;
    int target = direction > 0 ? *doc->bookmarks.begin() : *doc->bookmarks.rbegin();
    if (direction > 0) {
        auto it = doc->bookmarks.upper_bound(line);
        if (it != doc->bookmarks.end())
            target = *it;
    } else {
        auto it = doc->bookmarks.lower_bound(line);
        if (it != doc->bookmarks.begin())
            target = *std::prev(it);
    }
    goToLine(target);
}

void NotepadViewModel::compareWithSaved()
{
    const model::Document* doc = activeDocument();
    std::vector<model::DiffRow> rows = model::diffLines(model::splitLines(doc->savedText), model::splitLines(doc->buffer.text()));
    const model::DiffStats stats = model::diffStats(rows);
    review_.setDiff(doc->title + ": saved vs current", std::move(rows));
    showPanel.set(true);
    panelTab.set(PanelTab::Diff);
    statusText.set("Compare with saved: +" + std::to_string(stats.added) + " -" + std::to_string(stats.removed));
}

void NotepadViewModel::askAgent(const std::string& prompt)
{
    if (workspace.get() == WorkspaceMode::Code)
        showAgentPanel.set(true);
    agent_.submitPrompt(prompt, selectionContextLabel());
}

void NotepadViewModel::showPanelTab(PanelTab tab)
{
    showPanel.set(true);
    panelTab.set(tab);
    if (tab == PanelTab::Terminal) {
        terminal_.ensureTerminal();
        terminal_.focusRequested.emit();
    }
}

void NotepadViewModel::setZoom(float value)
{
    zoom.set(std::clamp(std::round(value * 10.0f) / 10.0f, kMinZoom, kMaxZoom));
}

} // namespace enp::viewmodel
