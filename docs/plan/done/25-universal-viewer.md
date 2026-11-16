# 25 · A universal viewer: Word, Excel, PowerPoint and PDF, read in the Code workspace

**Done** (16 Nov 2026). Word, Excel and PowerPoint files (OOXML) and PDFs open read-only in a tab of
the Code workspace, as a picture does. They need no workspace of their own. The workspaces
(Page, Plot, Pitch), the shared editable renderer, editing, the old binary formats and the reading
gaps still open moved to [plan 27, a universal writer](../27-universal-writer.md). The feature as
users see it is in [FEATURES.md](../../FEATURES.md) › Documents.

**Why:** EasyNotepad reads code well; most of what people read is not code. Office documents,
spreadsheets and presentations open with the same reading-first, agent-first approach: fast to
open and comfortable to read.

**How:** we took LibreOffice's essence, not LibreOffice: its format knowledge and a few of its
algorithms, rebuilt small on what we already had (see plan 27 for what was taken from it, and what
is still to take). Readers are pure functions from bytes to a model, run off the UI thread; the
Markdown renderer draws flowing text, one page painter draws fixed pages.

## What was built

| Done | Where | Tested by |
|---|---|---|
| ZIP (stored, deflate; bounded), XML pull parser (no DTD, no entities but the five; depth limit) | `model/package/` | unit tests; zip bomb, entity bomb, depth, truncation |
| Word (.docx) into the Markdown renderer's blocks: styles through `basedOn`, headings, numbering, tables, links, images, footnotes; tracked deletions and field codes left out | `model/viewer/Docx.cpp` | unit test |
| Excel (.xlsx) into a workbook: shared strings, number formats (`NumberFormat`), dates, booleans, errors, formulas' text, fonts, fills, alignment, widths, heights, merges, frozen panes | `model/viewer/Xlsx.cpp`, `NumberFormat.cpp` | unit tests |
| PowerPoint (.pptx) into fixed pages: shapes, custom paths, text boxes, pictures, tables, groups; layout and master placeholders, title / body styles, theme colours, backgrounds, notes | `model/viewer/Pptx.cpp` | unit test |
| PDF (added to the plan): xref tables and streams, object streams, a rebuilt xref for damaged files, Flate / LZW / ASCII85 / hex / run-length with predictors; paths, text placed by the fonts' widths (ToUnicode, standard encodings, glyph names, CID widths), images (JPEG, raw samples, masks, soft masks), forms; pages read when shown | `model/viewer/PdfObjects.cpp`, `Pdf.cpp` | unit tests |
| Reading off the UI thread, in MVVM layers: `IDocumentReaderService` / `DocumentReaderService` read and parse on one kept thread (whole documents, then PDF pages on request; cancellable); `ViewerViewModel` owns the documents by id, asks for the pages near the view and drops far ones; the UI thread reads only a file's first 4 KB to know it is a document | `services/DocumentReaderService.cpp`, `viewmodel/ViewerViewModel.cpp` | unit test (loading, failure, close while reading, reopen, destroyed with reads in flight); UI test waits for the reads |
| One renderer for flowing text (`MarkdownView::renderBlocks`), one painter for pages (`PagePainter`), a cell grid; `ViewerView` only draws ("Reading" while a document or a page is read) and keeps view caches by id | `view/MarkdownView.cpp`, `view/viewer/` | UI test: drawn, swept, closed, nothing held in the view, the view-model or the service |
| Robustness: LibreOffice's own test documents (`ENP_VIEWER_SAMPLES`) | `tests/ViewerTests.inc` | 3,269 of 3,285 read, none crashed or hung (the rest: encrypted PDFs, ZIP64, fuzzed files); the run also counts, by kind, what each file says it did not show |
| Charts in all three formats, from the values the file caches: column, bar, line, area, pie, doughnut, scatter, radar; clustered, stacked and 100% stacked; title, legend, value axis with gridlines and its number format, category labels; theme colours. Drawn as page items, so the page painter draws them | `model/viewer/Chart.cpp` | unit test (geometry, every kind and grouping, a broken chart) |
| Word: charts as drawing blocks between the paragraphs (a fixed page in the flow), text boxes as quoted blocks after their paragraph, comments listed at the end under their marks | `model/viewer/Docx.cpp` | unit test, UI test |
| Excel: pictures, charts and shapes over the cells, placed by their anchors (two-cell, one-cell, absolute) | `model/viewer/Xlsx.cpp`, `model/viewer/Shapes.cpp` | unit test, UI test |
| PowerPoint: charts in their frames; SmartArt from the drawing PowerPoint keeps of it, or, when there is none, its text as an outline from the data model; shape rotation; 40 preset shapes shared with Excel | `model/viewer/Pptx.cpp`, `model/viewer/Shapes.cpp` | unit test, UI test |
| Word formatting: run colour, highlight, size, underline styles; paragraph alignment, indents, first line, spacing, shading; tables aligned by `tblPr/jc`, cells of several paragraphs and line breaks on their own lines; the last section's default header and footer | `model/viewer/Docx.cpp`, `view/MarkdownView.cpp` | unit tests, `viewer_reads_the_challenge_files` |
| Excel cell look: number format colour sections and fractions, rich text runs, font sizes, strikeout, borders (thin to double, dashed, dotted, hair), wrap, vertical alignment, rotation and stacked text; text running over empty cells; merges across frozen panes; grid lines under fills; sheet tabs along the bottom | `model/viewer/Xlsx.cpp`, `NumberFormat.cpp`, `view/viewer/ViewerView.cpp` | unit tests, challenge files |
| PowerPoint text and lines: the list-style cascade (master text styles -> master / layout placeholder -> shape -> paragraph), line spacing and space before, table styles (`tableStyles.xml`, else Office's default), slide-number fields, gradients (linear, radial), dashes, arrowheads, cropped and flipped pictures, vertical text | `model/viewer/Pptx.cpp`, `Shapes.cpp`, `Ooxml.cpp` | unit tests, challenge files |
| PDF: axial and radial shadings and shading patterns, dashes, clipping (by the clip path's bounds), ligatures, rotated text, words fitted to their width | `model/viewer/Pdf.cpp`, `view/viewer/PagePainter.cpp` | unit tests, challenge files |
| Chinese, Japanese and Korean text: a system font merged into the UI font while an open document needs it, removed after; the file mapped, not read | `view/Fonts.cpp` (`ScriptFonts`), `os::fs::MappedFile` | unit test (script detection), memory cycle |

What LibreOffice's sample documents still report as not shown: Word charts 2 files (of 52), Excel
charts or drawings 8 files (of 75; group shapes and chartex charts), PowerPoint SmartArt laid out
only as text 38 files (they carry no drawing, only data), videos and sounds 7, PDF annotations 4.

Also done since: encrypted PDFs (the standard security handler: RC4, AES-128, AES-256; a
password field when one is needed); search in PDFs and slides, in reading order, two columns
included (`model/viewer/PageSearch.cpp`); a pinch or Ctrl+wheel zooms every kind about the point
under the fingers. Not done, and moved to plan 27: ZIP64, certificate-encrypted PDFs, the reading
gaps below.

## How close it comes to Office

Measured 2026-11-07 against screenshot tours of `C:\cpp\office-samples` (27 files), the challenge
files, and Microsoft Office's own render (Word, Excel and PowerPoint 16 export to PDF, rasterised
with PyMuPDF). What each format draws as Office does, by the work that made it so; the gaps still
open, ranked, are in plan 27 › Reading gaps.

### Word

Done on `feature/word-rendering`:

- **The document's own fonts:** `rFonts` and the theme's fonts, from the system when installed,
  else the bundled metric-compatible pack (`resources/fonts/document-fonts.zip`: Carlito for
  Calibri, Caladea for Cambria, Liberation for Times New Roman / Arial / Courier New), bold and
  italic faces, loaded when first drawn and given back when idle (`view::DocumentFonts`).
- **Tab stops:** left, centre, right and decimal, with dot, dash and line leaders (tables of contents).
- **Tables:** column widths from `tblGrid`, `gridSpan`, `vMerge`, cell shading, borders from the
  table, the cell and the table style's conditional parts (`tblLook`: header row, banded rows, first
  column...), cell alignment per paragraph, Word's automatic white text on dark shading.
- **Spacing:** before / after, line spacing (multiple, exact, at least), contextual spacing, list
  level indents with the marker at the hanging indent.
- **Pages** (the default; the toolbar's Reflow button reflows to the window): page size and margins,
  page breaks, section breaks, keep with next, headers and footers on every page with PAGE and
  NUMPAGES fields, footnotes at the foot of their page, page navigation.
- **Floating pictures:** placed from their anchor (page, margin or paragraph; offset or aligned), in
  front of or behind the text; text beside them (square, tight, through) is narrowed to the wider
  side, or goes under them (top and bottom, or no room beside).

### Excel

Done on `feature/word-rendering`:

- **The sheet as Excel draws it:** white paper, Excel's grey grid, black automatic text; cells and
  rich-text runs in their own fonts (the document fonts, real bold and italic faces).
- **Fills and lines:** hatch patterns, grey patterns as their mix, linear and centre gradients,
  diagonal borders; the palette's indexed colours 8-63.
- **Text:** `####` for numbers too wide for their column, right-aligned and centred text running
  over the empty cells beside it, indent, shrink to fit.
- **Conditional formatting** (`model/viewer/ConditionalFormat.cpp`), on the saved values: cell
  rules (against numbers, text or cells), top / bottom, above / below the average, text, blanks,
  errors, duplicates, colour scales, data bars, icon sets; priority order and stopIfTrue. Rules
  with formulas (expressions) and time periods are reported as not shown.
- **Table styles** (`model/viewer/SheetTable.cpp`): the built-in Light, Medium and Dark styles from
  the theme's accents (checked against Excel's own render of Medium 2 and 9, Light 1 and 9), and
  the workbook's own styles from its dxfs; header, total, banded rows and columns, first and last
  column. A cell's own fill, colour and borders win over the style, and conditional formats win
  over both.

### PowerPoint

Done on `feature/word-rendering`:

- **The deck's own fonts:** each run's latin typeface through the list styles and the theme (`+mj-lt`,
  `+mn-lt`), drawn in the document fonts with real bold and italic faces; charts and sheet shapes
  in the theme's minor font.
- **SmartArt without a cached drawing** (`model/viewer/SmartArt.cpp`): laid out from the data model
  by the layout's kind (block list, list, horizontal list, process, vertical process, chevrons,
  cycle, hierarchy, pyramid; others as a block list) in the default look: the first accent, white
  text, one text size for all. `lo-smartart1.pptx` now matches PowerPoint's render (five blocks, 2-2-1).
- **Autofit:** `fontScale` and `lnSpcReduction` as PowerPoint last fitted the text.
- **Table lines:** the cell's own (`lnL`...) over the table style's (`tcBdr`, by part outline and inside lines).
- **Fills:** picture fills in rectangular shapes; pattern fills as their two colours mixed.
- **Text:** superscript, subscript, letter spacing (`spc`).

### PDF

Done (2026-11-13, an image-heavy arXiv paper, 33 pages, 22.6 MB, against pdfium):

- **Outlines with holes**: a filled path of several outlines (a picture's frame, a ring, letters
  drawn as outlines) is cut into bands by its fill rule (non-zero or even-odd, `f*`) when the
  page is read; frames were drawn as solid boxes over the pictures they framed.
- **Soft masks of JPEGs**, and masks compressed or of another size: the picture and its mask
  are put together off the UI thread (`model/viewer/ImagePack.h`, `services::fitImage`).
- **Clips of pages read on demand** reached the view: the view-model copied the page's items
  but not its clip rectangles (nor the scripts of its text), so pictures spilled out of theirs.
- **Pictures at the size shown**, decoded on the reader's thread: a 1920 x 1536 camera frame
  shown as a 90-point thumbnail is kept at 270 x 216, and every page picture reaches the UI
  thread as QOI (unpacked several times faster than a JPEG is decoded). Paging through the
  paper peaked at 90 MB over the empty app instead of 350 MB; the 96 MB picture budget was
  running out and pictures past it showed as grey boxes.

### Checked against Office's render (2026-11-07)

Microsoft Office 16 exported 23 of the 27 samples (40 pages compared side by side with the
viewer's screenshots; the comparison sheets are made by the scripts under **Reference renders**).
What the comparison showed, most visible first. **Bugs** are things the viewer reads but draws
wrong; the rest confirm gaps listed above. **Fixed** rows were fixed the same day and are covered by
`viewer_draws_slides_as_office_does` and `viewer_numbers_word_headings_through_their_styles`.

| Pri | Kind | What Office shows / what we show | Sample |
|---|---|---|---|
| A | Fixed | PowerPoint subtitles and secondary text are light grey (`tx1` with `lumMod` / `lumOff` or a tint in the master's styles); ours are near black, on every deck | poi-SampleShow 1, poi-layouts 1, poi-shapes 2, poi-themes 1 (fixed: DrawingML tint and shade in linear light (Ooxml.cpp); colour modifiers in order, satMod added) |
| A | Fixed | Text in a shape takes its colour from the shape's style (`p:style/a:fontRef` with a scheme colour): white text on a green box; ours black | poi-themes 4 ("Text in a autoshape is white") (fixed: fontRef colours the shape's text over the master's text styles (Pptx textBody)) |
| A | Fixed | Italic text in slides is upright (title placeholders of the Austin theme, "This bit is in italic green") | poi-SampleShow 1, poi-themes 4-5 (fixed: italic runs slanted by the painter (styledText)) |
| A | Gap (fonts) | Calibri / Century Gothic are lighter and narrower than our Roboto Medium: every slide's text looks bolder and breaks lines elsewhere ("Nunc at risus vel erat tempus / posuere." breaks after "tempus" in Office) | all decks, poi-sample 1 |
| A | Fixed | Word table styles: the tables' orange header rows and lines (a built-in "Grid Table" style) are not drawn | poi-table-alignment (fixed: table styles with their conditional parts (Docx tableStyle)) |
| A | Fixed | A layout's radial gradient background is drawn as one flat colour | poi-themes 3 ("Radial Gradient Fill On Slide Layout") (fixed: bgRef / fillRef resolve the theme's fill style lists with phClr (themeFill); radial centre from fillToRect; gradient fills drawn on a subdivided mesh (PagePainter subdivide)) |
| B | Fixed | Comparison layout: the right column's bullet sits apart from its text, which is pushed right, and its heading is centred; Office left-aligns both columns | poi-layouts 5 (fixed: a placeholder matches the master by type only, the layout by idx) |
| B | Fixed | Elbow connectors (`bentConnector3`) are drawn as one straight line | poi-shapes 1 (cloud to picture) (fixed: bent and curved connectors routed (dml::connectorPath)) |
| B | Fixed | Hyperlinks in slides are not blue and underlined (the theme's `hlink` colour) | poi-shapes 5 (fixed: hlinkClick runs in the theme's hlink colour, underlined) |
| B | Fixed | `cap="all"`: Office shows "SECTION TITLE" in capitals | poi-layouts 3 (fixed: cap from rPr and list styles) |
| B | Fixed | Word heading numbering from the styles (`A.1 Introduction`, `A.2.1 DESCRIPTION`): ours drops the numbers, and the chapter heading is right-aligned instead of centred | lo-tdf95495 (fixed: numbered headings take their marker; a style without ilvl takes the level whose pStyle names it (the right alignment is the style's own jc, as in Word)) |
| B | Fixed | Empty Word paragraphs keep their height in Office (spacing between list groups); ours collapse them | poi-Numbering (fixed: an empty paragraph keeps its line) |
| B | Gap | A chart's plot area filled with a picture (`blipFill`), white gridlines over it and the default "Chart Title" are missing | poi-chart-picture-bg |
| C | Fixed | Gradient backgrounds of the Austin theme run darker at the edges in Office; ours lighter and flatter (stops or path) | poi-themes 4, 6 (fixed: with the theme's fill styles and the mesh, the edges darken as in Office) |
| C | Gap | Title slide of a layout without master graphics: Office's title is larger and anchored at the bottom of its box | poi-themes 6 |
| C | Gap | Table lines in slides are white in Office (the style's `tcBdr`); ours grey | poi-table_test |
| - | Same | Excel samples (values, formats, colours, theme colours), slide masters and layouts, level indents, table merges, bullets, pictures, texture fills, Word header / footer / table alignment: match, apart from fonts and our dark grid | 15 samples |

### Reference renders

Microsoft Office is installed on the development machine (Word, Excel and PowerPoint 16).
`File > Export > PDF` through COM gives the reference for every OOXML sample; PyMuPDF rasterises
it, and the viewer's own screenshot (automation `screenshot`) sits beside it. Learned so far:

- Office must open each file in its own process with a time limit: one of the samples left Word
  behind a hidden dialog and stopped a batch export. The exporter starts Word, Excel or PowerPoint
  per file and ends only the processes it started.
- **Office refuses the challenge files** (`tests/data/viewer/challenge.*`): "the file appears to be
  corrupted". The generator declares only `document.xml` (or the workbook / presentation) in
  `[Content_Types].xml` and lets the other parts fall back to `application/xml`; Office needs each
  part's own content type (styles, numbering, footnotes, headers, sheets, slides, charts...). The
  viewer reads them because it follows relationships, not content types. Fix the generator so the
  challenge files are valid for Office too, then use them as references.
- `lo-tdf100709.xlsx` does not open in Excel either (a LibreOffice regression file).
- With a per-file process and a 90 s limit, 23 of 27 samples exported (the 3 challenge files and
  `lo-tdf100709.xlsx` failed to open). Pages are rasterised at 55 dpi and paired with the viewer's
  screenshots (the document area of each), three pairs a sheet, Office on the left.

## Rules followed

- Readers are pure functions from bytes to model: no I/O, no UI, fuzzable, run on a background
  thread with progress and cancel.
- The parts a reader skips are counted, so a document says what it did not show.
- Memory: documents, pages and pictures are freed with their document; the view keeps caches by id
  and nothing after a close (`docs/MEMORY.md`).
- Hostile files: total and per-entry inflated size, compression ratio, entry count, XML depth and
  attributes are limited; a broken part shows what was read and says what was not.
- No new third-party library (stb's inflate; our own XML pull parser).
