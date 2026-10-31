# 04 · Find in Files

**Problem.** Find in Files (Ctrl+Shift+F) is a stub.

**Design.**
- Model: `searchText(text, query, options)` → matches (line, column, length) with Match case,
  Whole word and Regular expression options (std::regex, ECMAScript).
- View-model: `FindInFilesViewModel`: query, options, include filter (`*.cpp;*.h`), results grouped by
  file with the line's text; runs over the workspace (skipping vendor/build folders, binary and very
  large files) on a background thread, results posted to the main thread; open documents are searched
  in their current (unsaved) text. Replace all in files, with a summary.
- View: a **Search** tab in the bottom panel: query box, option toggles, include filter, results
  tree (file → lines with the match highlighted); click opens the file at the match.

**Tests.** Unit: search options; workspace search finds matches in files and in unsaved buffers,
skips vendor folders; replace in files writes them. UI sweep: Search tab with results.
