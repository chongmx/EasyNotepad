#include "enp/model/Document.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace enp::model {

const char* eolName(EolMode eol)
{
    switch (eol) {
    case EolMode::CrLf: return "Windows (CR LF)";
    case EolMode::Lf: return "Unix (LF)";
    case EolMode::Cr: return "Macintosh (CR)";
    }
    return "";
}

const char* eolShortName(EolMode eol)
{
    switch (eol) {
    case EolMode::CrLf: return "CRLF";
    case EolMode::Lf: return "LF";
    case EolMode::Cr: return "CR";
    }
    return "";
}

const char* encodingName(Encoding encoding)
{
    switch (encoding) {
    case Encoding::Utf8: return "UTF-8";
    case Encoding::Utf8Bom: return "UTF-8-BOM";
    case Encoding::Utf16Le: return "UTF-16 LE BOM";
    case Encoding::Utf16Be: return "UTF-16 BE BOM";
    case Encoding::Ansi: return "ANSI";
    }
    return "";
}

std::vector<Selection> Document::selections() const
{
    std::vector<Selection> all;
    all.reserve(cursors.size() + 1);
    all.push_back(primary());
    all.insert(all.end(), cursors.begin(), cursors.end());
    return all;
}

void normalizeCursors(Document& doc)
{
    if (doc.cursors.empty()) {
        doc.caret = doc.buffer.clamp(doc.caret);
        doc.anchor = doc.buffer.clamp(doc.anchor);
        return;
    }
    struct Item {
        Selection sel;
        bool primary;
    };
    std::vector<Item> items;
    items.reserve(doc.cursors.size() + 1);
    items.push_back({{doc.buffer.clamp(doc.anchor), doc.buffer.clamp(doc.caret)}, true});
    for (const Selection& s : doc.cursors)
        items.push_back({{doc.buffer.clamp(s.anchor), doc.buffer.clamp(s.caret)}, false});
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.sel.start() < b.sel.start(); });

    std::vector<Item> merged;
    merged.reserve(items.size());
    for (const Item& item : items) {
        if (!merged.empty()) {
            Item& last = merged.back();
            // Overlapping selections, or two carets at the same place, become one.
            const bool overlap = item.sel.start() < last.sel.end() || item.sel.start() == last.sel.start();
            if (overlap) {
                const TextPos start = last.sel.start();
                const TextPos end = std::max(last.sel.end(), item.sel.end(), [](TextPos a, TextPos b) { return a < b; });
                // Keep the direction of the caret that survives (the primary when it is one of them).
                const Selection& keep = item.primary ? item.sel : last.sel;
                const bool forward = !(keep.caret < keep.anchor);
                last.sel = forward ? Selection{start, end} : Selection{end, start};
                last.primary = last.primary || item.primary;
                continue;
            }
        }
        merged.push_back(item);
    }
    doc.cursors.clear();
    for (const Item& item : merged) {
        if (item.primary) {
            doc.anchor = item.sel.anchor;
            doc.caret = item.sel.caret;
        } else {
            doc.cursors.push_back(item.sel);
        }
    }
}

} // namespace enp::model
