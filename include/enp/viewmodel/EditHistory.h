// Undo and redo for every open document: each step is the buffer's journal of one edit (or a run
// of typing) plus the selection before and after it.
#pragma once

#include "enp/model/Document.h"

#include <map>
#include <vector>

namespace enp::viewmodel {

class EditHistory {
public:
    enum class Kind { Typing, Deleting, Other };
    static constexpr size_t kMaxSteps = 500;

    // Starts recording an edit of `doc`; calls nested inside it join its step. Typing (or
    // deleting) right where the previous one ended continues that step.
    void begin(model::Document& doc, Kind kind);
    // Ends the edit; true when the outermost one finished (the caller then refreshes the document).
    bool end(model::Document& doc);

    void undo(model::Document& doc);
    void redo(model::Document& doc);
    bool canUndo(int documentId) const;
    bool canRedo(int documentId) const;
    size_t bytes(int documentId) const; // text held by the document's steps
    void forget(int documentId);        // the document was closed

private:
    struct Step {
        std::vector<model::TextChange> changes;
        model::TextPos caretBefore, anchorBefore;
        model::TextPos caretAfter, anchorAfter;
    };
    struct History {
        std::vector<Step> undo;
        std::vector<Step> redo;
        Kind lastKind = Kind::Other;
        model::TextPos lastCaret{-1, -1};
        int depth = 0;
    };
    std::map<int, History> histories_;
};

} // namespace enp::viewmodel
