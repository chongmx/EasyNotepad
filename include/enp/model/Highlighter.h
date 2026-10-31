// Syntax highlighting driven by the language table: the colour class of every byte of a line.
// `state` carries constructs that span lines (block comments, triple-quoted and template
// strings, Markdown code fences, tags split over lines); start a document with 0 and pass each
// line in order.
#pragma once

#include "enp/model/Language.h"
#include "enp/model/TextBuffer.h"

#include <cstdint>

#include <string>
#include <vector>

namespace enp::model {

void highlightLine(const std::string& line, const LanguageSpec& spec, int& state, std::vector<Token>& out);

// The lexer state at the start of every line, computed lazily and kept across frames: drawing the
// screen costs the visible lines only, wherever in the file they are. Edits invalidate from the
// first changed line down; a language change invalidates everything.
class HighlightCache {
public:
    int stateAt(const TextBuffer& buffer, const LanguageSpec& spec, int line);
    size_t cachedLines() const { return states_.size(); } // for tests

private:
    const LanguageSpec* spec_ = nullptr;
    uint64_t version_ = UINT64_MAX;
    std::vector<int> states_; // states_[i]: state at the start of line i
    std::vector<Token> scratch_;
};

} // namespace enp::model
