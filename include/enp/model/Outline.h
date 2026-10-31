// Document structure for folding, AI summaries and the layers map: nested regions (namespaces,
// types, functions, blocks, Markdown sections) with their line ranges.
#pragma once

#include "enp/model/Document.h"

#include <string>
#include <vector>

namespace enp::model {

// Step: a logical paragraph of a function body (logical folding), usually led by a comment.
enum class OutlineKind { Namespace, Type, Function, Block, Section, Step };

// Syntax: regions follow the code's brackets / indentation / headings.
// Logical: inside functions, regions are the logical steps (comment-led paragraphs) instead of
// the syntactic blocks, so a folded function reads as the list of what it does.
enum class OutlineMode { Syntax, Logical };

struct OutlineNode {
    int startLine = 0; // header line; stays visible when folded
    int endLine = 0;   // last line of the region (inclusive)
    int depth = 0;
    int parent = -1;   // index into the outline, -1 for top level
    OutlineKind kind = OutlineKind::Block;
    std::string title; // header text, trimmed
    std::string name;  // short display name: "set(...)" for "void NotepadViewModel::set(CommandId id, ...)"
    std::string hint;  // summary written by the author (a step's leading comment), if any

    int lineCount() const { return endLine - startLine + 1; }
    bool contains(int line) const { return line > startLine && line <= endLine; } // folded part
};

const char* outlineKindName(OutlineKind kind);

// Regions of at least two lines, sorted by start line (parents before children).
// How regions are found comes from the language table (braces, indentation, keywords, tags,
// headings, sections), plus #region markers in any language.
std::vector<OutlineNode> buildOutline(const TextBuffer& buffer, Language language,
                                      OutlineMode mode = OutlineMode::Syntax);

} // namespace enp::model
