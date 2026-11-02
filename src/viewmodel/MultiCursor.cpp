// NotepadViewModel: several carets at once, as in VS Code (Alt+click, Ctrl+D, Ctrl+Alt+Up/Down,
// Ctrl+Shift+L, column selection). Every edit runs once per caret, bottom to top, in one undo step.
#include "enp/viewmodel/NotepadViewModel.h"

#include "enp/os/Memory.h"

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace enp::viewmodel {

namespace {

bool isWordChar(char c)
{
    const unsigned char u = static_cast<unsigned char>(c);
    return u >= 0x80 || (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || c == '_';
}

// A position as its distance from the end of the buffer: lines below it, bytes after it in its
// line. An edit above (or before, in the same line) a position does not change this distance, so
// carets done earlier in a bottom-to-top pass stay right while the ones above them are edited.
struct FromEnd {
    int lines = 0;
    int columns = 0;
};

FromEnd fromEnd(const model::TextBuffer& buffer, model::TextPos pos)
{
    pos = buffer.clamp(pos);
    return {buffer.lineCount() - 1 - pos.line, int(buffer.line(pos.line).size()) - pos.column};
}

model::TextPos toPos(const model::TextBuffer& buffer, FromEnd e)
{
    const int line = std::clamp(buffer.lineCount() - 1 - e.lines, 0, buffer.lineCount() - 1);
    const int size = int(buffer.line(line).size());
    return buffer.clamp({line, std::clamp(size - e.columns, 0, size)});
}

// Lines covered by a selection (one ending at the start of a line does not cover that line).
void linesOf(const model::Selection& sel, int& first, int& last)
{
    first = sel.start().line;
    last = sel.end().line;
    if (last > first && sel.end().column == 0)
        --last;
}

int visualColumn(const std::string& s, int byteColumn, int tab)
{
    int v = 0;
    const int end = std::min(byteColumn, int(s.size()));
    for (int i = 0; i < end; ++i) {
        if (s[size_t(i)] == '\t')
            v = (v / tab + 1) * tab;
        else if (!model::isUtf8Continuation(s[size_t(i)]))
            ++v;
    }
    return v;
}

int byteColumnAt(const std::string& s, int visual, int tab)
{
    int v = 0;
    int i = 0;
    while (i < int(s.size())) {
        int next = v;
        if (s[size_t(i)] == '\t')
            next = (v / tab + 1) * tab;
        else
            ++next;
        if (next > visual)
            break;
        v = next;
        ++i;
        while (i < int(s.size()) && model::isUtf8Continuation(s[size_t(i)]))
            ++i;
    }
    return i;
}

// Every place `needle` occurs (one line at most), in document order.
std::vector<model::Selection> occurrences(const model::TextBuffer& buffer, const std::string& needle, bool wholeWord)
{
    std::vector<model::Selection> found;
    if (needle.empty())
        return found;
    for (int l = 0; l < buffer.lineCount(); ++l) {
        const std::string& line = buffer.line(l);
        for (size_t pos = line.find(needle); pos != std::string::npos; pos = line.find(needle, pos + 1)) {
            const size_t end = pos + needle.size();
            if (wholeWord && ((pos > 0 && isWordChar(line[pos - 1])) || (end < line.size() && isWordChar(line[end]))))
                continue;
            found.push_back({{l, int(pos)}, {l, int(end)}});
        }
    }
    return found;
}

} // namespace

// ---- Running edits at every caret ---------------------------------------------------------------

bool NotepadViewModel::forEachCursor(EditKind kind, const std::function<void(size_t index)>& edit)
{
    model::Document* doc = activeDocument();
    if (!doc || doc->cursors.empty())
        return false;
    model::normalizeCursors(*doc);
    if (doc->cursors.empty())
        return false; // they all met in one place

    const std::vector<model::Selection> all = doc->selections(); // [0] is the primary caret
    std::vector<size_t> order(all.size());
    std::iota(order.begin(), order.end(), size_t(0));
    std::sort(order.begin(), order.end(), [&all](size_t a, size_t b) { return all[a].start() < all[b].start(); });

    beginEdit(kind); // records every caret as it was, for undo
    doc->cursors.clear(); // while each caret runs, it is the only one (the edit takes its single-caret path)
    std::vector<std::pair<FromEnd, FromEnd>> results(all.size());
    for (size_t k = order.size(); k-- > 0;) {
        const size_t i = order[k];
        doc->anchor = doc->buffer.clamp(all[i].anchor);
        doc->caret = doc->buffer.clamp(all[i].caret);
        edit(k);
        doc->cursors.clear();
        results[i] = {fromEnd(doc->buffer, doc->anchor), fromEnd(doc->buffer, doc->caret)};
    }
    for (size_t i = 0; i < all.size(); ++i) {
        const model::Selection sel{toPos(doc->buffer, results[i].first), toPos(doc->buffer, results[i].second)};
        if (i == 0) {
            doc->anchor = sel.anchor;
            doc->caret = sel.caret;
        } else {
            doc->cursors.push_back(sel);
        }
    }
    model::normalizeCursors(*doc);
    endEdit();
    return true;
}

bool NotepadViewModel::forEachLineBlock(const std::function<void()>& op, bool collapse)
{
    model::Document* doc = activeDocument();
    if (!doc || doc->cursors.empty())
        return false;
    model::normalizeCursors(*doc);
    if (doc->cursors.empty())
        return false;

    const std::vector<model::Selection> all = doc->selections();
    // Blocks of lines: carets on the same or adjacent lines share one, so no line is done twice.
    struct Block {
        int first;
        int last;
        std::vector<size_t> members;
    };
    std::vector<size_t> order(all.size());
    std::iota(order.begin(), order.end(), size_t(0));
    std::sort(order.begin(), order.end(), [&all](size_t a, size_t b) { return all[a].start() < all[b].start(); });
    std::vector<Block> blocks;
    for (size_t i : order) {
        int first = 0, last = 0;
        linesOf(all[i], first, last);
        if (!blocks.empty() && first <= blocks.back().last + 1) {
            blocks.back().last = std::max(blocks.back().last, last);
            blocks.back().members.push_back(i);
        } else {
            blocks.push_back({first, last, {i}});
        }
    }

    beginEdit(EditKind::Other);
    doc->cursors.clear();
    std::vector<std::pair<FromEnd, FromEnd>> results(all.size());
    for (auto block = blocks.rbegin(); block != blocks.rend(); ++block) {
        const bool single = block->members.size() == 1;
        if (single) {
            doc->anchor = doc->buffer.clamp(all[block->members[0]].anchor);
            doc->caret = doc->buffer.clamp(all[block->members[0]].caret);
        } else { // the operation sees the block as one selection of whole lines
            doc->anchor = {block->first, 0};
            doc->caret = {block->last, int(doc->buffer.line(block->last).size())};
        }
        const int startBefore = doc->selectionStart().line;
        std::vector<int> lengthsBefore;
        for (int l = block->first; l <= block->last; ++l)
            lengthsBefore.push_back(int(doc->buffer.line(l).size()));
        op();
        doc->cursors.clear();
        if (collapse) {
            const model::Selection after = single ? doc->primary() : model::Selection{doc->caret, doc->caret};
            for (size_t m : block->members)
                results[m] = {fromEnd(doc->buffer, after.anchor), fromEnd(doc->buffer, after.caret)};
            continue;
        }
        // Each caret keeps its line (moved with the block) and its place in it: shifted by what the
        // operation added to or removed from the line (an indent, a comment marker).
        const int shift = doc->selectionStart().line - startBefore;
        auto follow = [&](model::TextPos p) {
            const int line = std::clamp(p.line + shift, 0, doc->buffer.lineCount() - 1);
            const int size = int(doc->buffer.line(line).size());
            int column = p.column;
            const int index = p.line - block->first;
            if (index >= 0 && index < int(lengthsBefore.size()) && column > 0)
                column += size - lengthsBefore[size_t(index)];
            return doc->buffer.clamp({line, std::clamp(column, 0, size)});
        };
        for (size_t m : block->members)
            results[m] = {fromEnd(doc->buffer, follow(all[m].anchor)), fromEnd(doc->buffer, follow(all[m].caret))};
    }
    for (size_t i = 0; i < all.size(); ++i) {
        const model::Selection sel{toPos(doc->buffer, results[i].first), toPos(doc->buffer, results[i].second)};
        if (i == 0) {
            doc->anchor = sel.anchor;
            doc->caret = sel.caret;
        } else {
            doc->cursors.push_back(sel);
        }
    }
    model::normalizeCursors(*doc);
    endEdit();
    return true;
}

// ---- Adding and moving carets -------------------------------------------------------------------

int NotepadViewModel::cursorCount() const
{
    const model::Document* doc = activeDocument();
    return doc ? int(doc->cursors.size()) + 1 : 1;
}

void NotepadViewModel::setSelections(std::vector<model::Selection> selections)
{
    model::Document* doc = activeDocument();
    if (!doc || selections.empty())
        return;
    doc->anchor = selections.front().anchor;
    doc->caret = selections.front().caret;
    selections.erase(selections.begin());
    doc->cursors = std::move(selections);
    model::normalizeCursors(*doc);
}

void NotepadViewModel::clearExtraCursors()
{
    if (model::Document* doc = activeDocument())
        os::mem::release(doc->cursors);
}

void NotepadViewModel::addCursor(model::TextPos pos)
{
    model::Document* doc = activeDocument();
    if (!doc)
        return;
    pos = doc->buffer.clamp(pos);
    // Alt+click on a caret (with others left) removes it.
    if (doc->multiCursor()) {
        for (auto it = doc->cursors.begin(); it != doc->cursors.end(); ++it)
            if (it->caret == pos && it->empty()) {
                doc->cursors.erase(it);
                return;
            }
        if (doc->caret == pos && !doc->hasSelection()) {
            doc->anchor = doc->cursors.back().anchor; // the last one added becomes the primary caret
            doc->caret = doc->cursors.back().caret;
            doc->cursors.pop_back();
            model::normalizeCursors(*doc);
            return;
        }
    }
    doc->cursors.push_back(doc->primary());
    doc->anchor = doc->caret = pos;
    model::normalizeCursors(*doc);
}

void NotepadViewModel::addCursorVertical(int direction)
{
    model::Document* doc = activeDocument();
    if (!doc)
        return;
    // From the outermost caret in that direction, at the same visual column.
    model::TextPos from = doc->caret;
    for (const model::Selection& sel : doc->cursors)
        if (direction < 0 ? sel.caret < from : from < sel.caret)
            from = sel.caret;
    const int line = from.line + (direction < 0 ? -1 : 1);
    if (line < 0 || line >= doc->buffer.lineCount())
        return;
    const int tab = std::max(1, doc->indent.width);
    const int visual = visualColumn(doc->buffer.line(from.line), from.column, tab);
    const model::TextPos pos{line, byteColumnAt(doc->buffer.line(line), visual, tab)};
    doc->cursors.push_back(doc->primary());
    doc->anchor = doc->caret = pos;
    model::normalizeCursors(*doc);
    caretRevealRequested.emit();
    statusText.set(std::to_string(cursorCount()) + " carets (Escape: back to one)");
}

void NotepadViewModel::moveCarets(const std::function<model::TextPos(const model::Selection&)>& move, bool extendSelection)
{
    model::Document* doc = activeDocument();
    if (!doc)
        return;
    if (!doc->multiCursor()) {
        setCaret(move(doc->primary()), extendSelection);
        return;
    }
    for (model::Selection& sel : doc->cursors) {
        sel.caret = doc->buffer.clamp(move(sel));
        if (!extendSelection)
            sel.anchor = sel.caret;
    }
    setCaret(move(doc->primary()), extendSelection, true); // recorded into a macro like a single caret
}

void NotepadViewModel::cursorsAtLineEnds()
{
    model::Document* doc = activeDocument();
    if (!doc)
        return;
    std::vector<model::Selection> carets;
    for (const model::Selection& sel : doc->selections()) {
        int first = 0, last = 0;
        linesOf(sel, first, last);
        for (int l = first; l <= last; ++l) {
            const model::TextPos end{l, int(doc->buffer.line(l).size())};
            carets.push_back({end, end});
        }
    }
    // The primary caret is the last line's.
    std::rotate(carets.begin(), carets.end() - 1, carets.end());
    setSelections(std::move(carets));
    caretRevealRequested.emit();
    statusText.set(std::to_string(cursorCount()) + " carets");
}

// ---- Occurrences of the selection ---------------------------------------------------------------

void NotepadViewModel::addNextOccurrence()
{
    model::Document* doc = activeDocument();
    if (!doc)
        return;
    // First press without a selection: the word at the caret, and from now on whole words only.
    if (!doc->hasSelection() && !doc->multiCursor()) {
        const model::TextPos a = doc->buffer.wordStart(doc->caret);
        const model::TextPos b = doc->buffer.wordEnd(doc->caret);
        if (a == b)
            return;
        setSelection(a, b);
        occurrence_ = {doc->id, doc->primary()};
        return;
    }
    if (!doc->multiCursor() && !(occurrence_.documentId == doc->id && occurrence_.word == doc->primary()))
        occurrence_ = {}; // a selection made by hand: any occurrence, inside words too
    const bool wholeWord = occurrence_.documentId == doc->id;
    model::Selection source = doc->primary();
    if (source.empty())
        for (const model::Selection& sel : doc->cursors)
            if (!sel.empty()) {
                source = sel;
                break;
            }
    if (source.empty() || source.start().line != source.end().line) {
        statusText.set("Add Next Occurrence works on a selection within one line");
        return;
    }
    const std::string needle = doc->buffer.text(source.start(), source.end());
    const std::vector<model::Selection> found = occurrences(doc->buffer, needle, wholeWord);
    const std::vector<model::Selection> current = doc->selections();
    auto taken = [&current](const model::Selection& s) {
        for (const model::Selection& c : current)
            if (c.start() == s.start())
                return true;
        return false;
    };
    // The next one after the primary (the last added) selection, wrapping at the end.
    const model::TextPos after = doc->primary().end();
    const model::Selection* next = nullptr;
    for (const model::Selection& s : found)
        if (!(s.start() < after) && !taken(s)) {
            next = &s;
            break;
        }
    if (!next)
        for (const model::Selection& s : found)
            if (!taken(s)) {
                next = &s;
                break;
            }
    if (!next) {
        statusText.set("Every occurrence of \"" + needle + "\" is selected");
        return;
    }
    doc->cursors.push_back(doc->primary());
    doc->anchor = next->start();
    doc->caret = next->end();
    model::normalizeCursors(*doc);
    revealLine(doc->caret.line);
    caretRevealRequested.emit();
    statusText.set(std::to_string(cursorCount()) + " of " + std::to_string(found.size()) + " occurrences of \"" + needle + "\" selected");
}

void NotepadViewModel::selectAllOccurrences()
{
    model::Document* doc = activeDocument();
    if (!doc)
        return;
    bool wholeWord = false;
    model::Selection source = doc->primary();
    if (source.empty()) {
        source = {doc->buffer.wordStart(doc->caret), doc->buffer.wordEnd(doc->caret)};
        wholeWord = true;
    } else if (occurrence_.documentId == doc->id && (occurrence_.word == source || doc->multiCursor())) {
        wholeWord = true; // carried on from Ctrl+D
    }
    if (source.empty() || source.start().line != source.end().line)
        return;
    const std::string needle = doc->buffer.text(source.start(), source.end());
    std::vector<model::Selection> found = occurrences(doc->buffer, needle, wholeWord);
    if (found.empty())
        return;
    // The primary caret stays on the occurrence it was on.
    auto primary = std::find_if(found.begin(), found.end(), [&source](const model::Selection& s) { return s.start() == source.start(); });
    if (primary != found.end())
        std::iter_swap(found.begin(), primary);
    const size_t count = found.size();
    setSelections(std::move(found));
    occurrence_ = wholeWord ? OccurrenceSearch{doc->id, {}} : OccurrenceSearch{};
    statusText.set(std::to_string(count) + " occurrences of \"" + needle + "\" selected");
}

// ---- Clipboard ----------------------------------------------------------------------------------

void NotepadViewModel::paste(std::string_view text)
{
    model::Document* doc = activeDocument();
    if (!doc)
        return;
    if (doc->multiCursor()) {
        std::string clean(text);
        clean.erase(std::remove(clean.begin(), clean.end(), '\r'), clean.end());
        // As many lines as carets: one line each (a copy from as many carets pastes back as it was).
        std::vector<std::string> pieces;
        size_t begin = 0;
        for (size_t nl = clean.find('\n'); nl != std::string::npos; nl = clean.find('\n', begin)) {
            pieces.push_back(clean.substr(begin, nl - begin));
            begin = nl + 1;
        }
        pieces.push_back(clean.substr(begin));
        if (pieces.size() == size_t(cursorCount()) + 1 && pieces.back().empty())
            pieces.pop_back();
        if (pieces.size() == size_t(cursorCount())) {
            MacroScope scope(*this);
            if (scope.outer)
                recordMacro({MacroStep::Kind::Text, clean, 0, 0, false, CommandId::Count});
            forEachCursor(EditKind::Other, [&](size_t index) { insertText(pieces[index]); });
            return;
        }
    }
    insertText(text);
}

} // namespace enp::viewmodel
