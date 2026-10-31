#include "enp/viewmodel/EditHistory.h"

#include <iterator>

namespace enp::viewmodel {

void EditHistory::begin(model::Document& doc, Kind kind)
{
    History& h = histories_[doc.id];
    if (h.depth++ > 0)
        return; // inside another edit: its step takes these changes too
    const bool coalesce = kind != Kind::Other && kind == h.lastKind && doc.caret == h.lastCaret && !doc.hasSelection() &&
                          !h.undo.empty();
    if (!coalesce) {
        h.undo.push_back({{}, doc.caret, doc.anchor, doc.caret, doc.anchor});
        if (h.undo.size() > kMaxSteps)
            h.undo.erase(h.undo.begin());
    }
    h.redo.clear();
    h.lastKind = kind;
    doc.buffer.beginJournal();
}

bool EditHistory::end(model::Document& doc)
{
    History& h = histories_[doc.id];
    if (h.depth > 0 && --h.depth > 0)
        return false;
    std::vector<model::TextChange> changes = doc.buffer.endJournal();
    if (!h.undo.empty()) {
        Step& step = h.undo.back();
        step.changes.insert(step.changes.end(), std::make_move_iterator(changes.begin()), std::make_move_iterator(changes.end()));
        step.caretAfter = doc.caret;
        step.anchorAfter = doc.anchor;
        if (step.changes.empty())
            h.undo.pop_back(); // nothing changed: no step
    }
    h.lastCaret = doc.caret;
    return true;
}

void EditHistory::undo(model::Document& doc)
{
    History& h = histories_[doc.id];
    if (h.undo.empty())
        return;
    Step step = std::move(h.undo.back());
    h.undo.pop_back();
    h.lastKind = Kind::Other;
    doc.buffer.revert(step.changes);
    doc.caret = step.caretBefore;
    doc.anchor = step.anchorBefore;
    h.redo.push_back(std::move(step));
}

void EditHistory::redo(model::Document& doc)
{
    History& h = histories_[doc.id];
    if (h.redo.empty())
        return;
    Step step = std::move(h.redo.back());
    h.redo.pop_back();
    h.lastKind = Kind::Other;
    doc.buffer.reapply(step.changes);
    doc.caret = step.caretAfter;
    doc.anchor = step.anchorAfter;
    h.undo.push_back(std::move(step));
}

bool EditHistory::canUndo(int documentId) const
{
    const auto it = histories_.find(documentId);
    return it != histories_.end() && !it->second.undo.empty();
}

bool EditHistory::canRedo(int documentId) const
{
    const auto it = histories_.find(documentId);
    return it != histories_.end() && !it->second.redo.empty();
}

size_t EditHistory::bytes(int documentId) const
{
    const auto it = histories_.find(documentId);
    if (it == histories_.end())
        return 0;
    size_t total = 0;
    for (const auto* steps : {&it->second.undo, &it->second.redo})
        for (const Step& step : *steps)
            for (const model::TextChange& c : step.changes)
                total += c.removed.size() + c.inserted.size();
    return total;
}

void EditHistory::forget(int documentId)
{
    histories_.erase(documentId);
}

} // namespace enp::viewmodel
