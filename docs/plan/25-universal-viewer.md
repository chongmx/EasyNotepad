# 25. A universal viewer: Page, Plot and Pitch workspaces on one shared renderer

**Why:** EasyNotepad reads code well; most of what people read is not code. Three more workspaces,
beside Code and Agent, open word-processor documents, spreadsheets and presentations, with the same
reading-first, agent-first approach: fast to open, comfortable to read, editable in the simple
ways, and readable by an agent as structure (headings, cells, slides), not pixels.

**How:** we take LibreOffice's essence, not LibreOffice: its document models, its format knowledge
and a few of its algorithms, rebuilt small on what we already have. The Markdown renderer becomes
the one rich-text renderer every workspace shares.

**Status:** in progress on `feature/universal-viewer` (from `feature/enhanced-ui`). First, the
readers and renderers, tested before any workspace exists; for now every viewer lives in the Code
workspace (an Office file or a PDF opens in a tab like an image does), in modules that move to the
Page / Plot / Pitch workspaces unchanged:

| Done | Where | Tested by |
|---|---|---|
| ZIP (stored, deflate; bounded), XML pull parser (no DTD, no entities but the five; depth limit) | `model/package/` | unit tests; zip bomb, entity bomb, depth, truncation |
| Word (.docx) into the Markdown renderer's blocks: styles through `basedOn`, headings, numbering, tables, links, images, footnotes; tracked deletions and field codes left out | `model/viewer/Docx.cpp` | unit test |
| Excel (.xlsx) into a workbook: shared strings, number formats (`NumberFormat`), dates, booleans, errors, formulas' text, fonts, fills, alignment, widths, heights, merges, frozen panes | `model/viewer/Xlsx.cpp`, `NumberFormat.cpp` | unit tests |
| PowerPoint (.pptx) into fixed pages: shapes, custom paths, text boxes, pictures, tables, groups; layout and master placeholders, title / body styles, theme colours, backgrounds, notes | `model/viewer/Pptx.cpp` | unit test |
| PDF (added to the plan): xref tables and streams, object streams, a rebuilt xref for damaged files, Flate / LZW / ASCII85 / hex / run-length with predictors; paths, text placed by the fonts' widths (ToUnicode, standard encodings, glyph names, CID widths), images (JPEG, raw samples, masks, soft masks), forms; pages read when shown | `model/viewer/PdfObjects.cpp`, `Pdf.cpp` | unit tests |
| One renderer for flowing text (`MarkdownView::renderBlocks`), one painter for pages (`PagePainter`), a cell grid; `ViewerView` holds a document's state by id | `view/MarkdownView.cpp`, `view/viewer/` | UI test: drawn, swept, closed, nothing held |
| Robustness: LibreOffice's own test documents (`ENP_VIEWER_SAMPLES`) | `tests/ViewerTests.inc` | 3,269 of 3,285 read, none crashed or hung (the rest: encrypted PDFs, ZIP64, fuzzed files) |

Not yet: encrypted PDFs, ZIP64, PDF clipping / shadings / embedded font outlines, charts, the
workspaces themselves (25.0), reading on a background thread.

## LibreOffice: what it is, and the essence worth extracting

`ref/libreoffice-core` (read 2026-11-06) is about 10 million lines of C++ in about 200 modules and
108 bundled libraries: Writer (`sw`) 1.2 million lines, Calc (`sc`) 0.97 million, Impress/Draw
(`sd`) 0.34 million, the toolkit (`vcl`) 0.59 million. Its layers, bottom to top:

| Layer | Modules | What it does |
|---|---|---|
| OS abstraction | `sal` (`osl`, `rtl`), `o3tl` | one C API over every OS |
| Component model | `cppu`, `cppuhelper`, IDL in `udkapi`/`offapi` | UNO: every service an IDL interface, created by name, callable from any language or process |
| Helpers | `tools`, `svl` (`SfxItemSet` property bags; the number formatter), `comphelper`, `i18npool` | strings, streams, attributes, locale data |
| Toolkit | `vcl` and its backends (win, osx, qt, gtk, android, headless) | windows, widgets, `OutputDevice`, fonts, printing |
| Graphics | `basegfx`, `drawinglayer` | geometry; a display list of *primitives* between layout and painting |
| Shared engines | `editeng` (paragraphs of text with attribute runs: text in shapes, cells, notes), `svx` (`SdrObject` shapes on pages), `docmodel` (themes, colours) | what Writer, Calc and Impress share |
| Framework | `sfx2` (document, view, `SfxMedium` load/save, slot dispatch), `framework` (`.uno:` commands, menus, toolbars) | model / view / controller and commands |
| Applications | `sw` (model, page layout tree `SwRootFrame` -> pages -> paragraphs -> lines), `sc` (columns of typed cell blocks, formula engine), `sd` (pages of shapes; `slideshow` plays them) | the editors |
| Filters | `filter/source/config` (189 types, detected by content), `xmloff` (ODF), `oox` (DrawingML, OOXML shared), `sw/source/writerfilter` (DOCX tokens -> `dmapper` -> Writer), binary `ww8`/`excel`/`ppt` | file <-> model, through the model's API |
| Programs | `desktop`, `libreofficekit` | the app; LOK: load, `paintTile`, post input, invalidation callbacks |

A file opens as: stream -> type detected by content -> filter builds the model through its API ->
the application lays it out -> layout emits primitives -> the device paints them.

The bloat is the plumbing (UNO, VCL, `sfx2`, item pools, 108 libraries, binary formats, macros,
databases, printing). The essence, and where it lives:

| Essence | In LibreOffice | What we take | Where it goes here |
|---|---|---|---|
| One rich-text model for every app: paragraphs, each a string plus attribute runs; paragraph and character styles that inherit | `editeng` (`EditDoc`, `ContentNode`, `CharAttribList`), `sw` styles | the shape of the model: text + runs + style chain, resolved once at load | `model/rich/` (below) |
| Layout separate from painting: layout produces a list of positioned primitives; painting and hit-testing read it | `drawinglayer` primitives, `sw` layout frames | a display list of lines and glyph runs with their source positions | `view/rich/RichLayout` |
| Content-based type detection, one table | `filter/source/config/fragments/types` | zip entries, `mimetype`, `[Content_Types].xml`, magic numbers | `model/FileKind` |
| Importers that build the model through a small builder API, never its internals | `xmloff` (SAX + UNO API), `oox`, `writerfilter/dmapper` | a `RichBuilder` interface every reader calls; Markdown is one reader | `model/office/*Reader` |
| OOXML / ODF format knowledge: style inheritance (docDefaults -> style -> direct), numbering (`abstractNum`/`num`/`lvl`), DrawingML placeholders (master -> layout -> slide), theme colours and fonts | `writerfilter/dmapper` (86 files), `oox/source/drawingml`, `oox/source/ppt`, `xmloff/source/style` | rules only, rewritten; their tests' sample files as a guide to the corner cases | the readers |
| Sheet storage: per column, runs of same-typed cells | `sc` `ScColumn` + `mdds::multi_type_vector` | sparse columns of typed runs; shared strings | `model/office/Workbook` |
| Number formats: Excel/ODF codes (`0.00`, `#,##0`, `d mmm yyyy`, `0%`, sections `;`) | `svl/source/numbers` (`zformat.cxx`, `zforscan.cxx`) | the format-code grammar and rendering, a small subset first | `model/office/NumberFormat` |
| Lossless round trip of what is not understood | ODF/OOXML "preserve" grab-bags in `writerfilter` and `oox` | unknown XML kept as opaque bytes beside the model and written back as read | the readers and writers |

For the OOXML formats we take the rules, not the code: that code is built on UNO, VCL and item
pools, and nothing lifts out alone. **The old binary formats are the exception:** their parsers are
decades of reverse engineering that cannot be rewritten from a specification, so their format layer
is ported (copied and adapted, MPL-2.0 kept; stage 3 below). What we do not take at all: macros,
databases, printing, page layout, and the plumbing.

## Where EasyNotepad stands

```
src/os/        files, processes, memory scopes, per-OS code (win32, posix, android, web)
src/model/     TextBuffer, Document, FileKind, Markdown, Csv, Json, Math, Git, ...
src/services/  I*Service implementations: files, processes, agent CLI, terminal, images, automation
src/viewmodel/ NotepadViewModel (+ Agent, Git, Review, Intellisense, ...), CommandId, Settings
src/view/      Dear ImGui views: MainView, RibbonView, EditorView, MarkdownView, DataViews, ...
src/app/       Application (frame loop), GLFW / Android / headless mains, automation, MCP bridge
```

- **The Markdown renderer** is most of a rich-text renderer already. `model::markdown` parses into
  `Block`s (paragraph, heading, bullet, numbered, task, code, rule, table, math) of styled `Span`s
  (bold, italic, code, strike, link, image, math, emoji): 939 lines. `view::MarkdownView` lays
  blocks out to a width, measures once per width and font size, skips blocks off screen, draws
  highlighted code, tables, images, emoji and LaTeX: 738 lines. It serves the agent's answers and
  `.md` files. What it lacks for documents: colour, size, font, underline, alignment, indents and
  spacing per paragraph, cell and image sizes, and, for editing, a laid-out result it can hit-test
  (it measures by drawing with `draw = false`, so there is no caret position or selection).
- **Workspaces:** `WorkspaceMode { Agent, Code }` (`NotepadViewModel.h:45`) is a two-value switch,
  used 48 times in 13 files: most test `== Agent`, ribbon tab settings are an array of 2, the toggle
  flips between the two, and the automation state reports `"agent"` or `"code"`.
- **Open files** are text (`TextBuffer`) with views on it; nothing reads a zip or XML yet. `stb`'s
  inflate is already built in (images), so a zip reader needs no new library.
- **Memory:** every subsystem has a scope, and closing a folder gives everything back
  (`docs/MEMORY.md`, the 30-cycle test). Builds for Windows (x64, x86, arm64, 7), Linux, macOS, the
  web and Android.

## The shared renderer

One model, one layout, one painter, used by Markdown, the agent's answers, documents, cell text and
slide text boxes:

```
 readers (bytes -> model)          model/rich                   view/rich
 ------------------------          ----------                   ---------
 MarkdownReader  (today's parser)  RichDocument                 RichLayout   width, fonts, scale ->
 DocxReader      word/document.xml   blocks: Paragraph,           lines of glyph runs, boxes, images;
 XlsxReader      cell text           Table (cells hold blocks),   each run knows its source position
 PptxReader      slide text boxes    Image, Rule, Code, Math    RichPainter  layout -> ImDrawList,
       |                           Paragraph: text + runs         only what is visible
       +--> RichBuilder ---------->  (attribute ranges) +       RichHit      point <-> position,
            (begin paragraph,        paragraph style            caret rect, selection rects
             run, table, image)    Styles: named, inheriting    RichEditor   (stage 2) caret,
                                   Keep: opaque XML to write    typing, delete, Enter, B/I/U,
                                     back unchanged             heading style, undo
```

- `model/rich` generalises `model::markdown`'s `Block`/`Span`: a paragraph is UTF-8 text plus
  attribute runs (bold, italic, underline, strike, code, colour, highlight, size, font, link,
  baseline) over byte ranges, and a paragraph style (heading level, list level and marker,
  alignment, indents, spacing). Styles are resolved at load into a small table of distinct run
  formats (LibreOffice's pooled items, without the pool), so a run is two integers.
- `MarkdownReader` is today's parser emitting into `RichBuilder`. `MarkdownView` becomes a thin
  wrapper (parse when the text changes, then `RichLayout` + `RichPainter`); the agent panel and `.md`
  rendering keep working, pixel for pixel where possible. This step has its own
  tests before any Office file is read.
- `RichLayout` keeps today's virtualisation (measure per width and size, skip what is off screen)
  and records the position of every line and run, so `RichHit` maps a point to a text position
  and back: stage 1 uses it to select and copy text, stage 2 to place a caret and edit.

## Workspaces

The switch becomes five: **Code**, **Agent**, **Page**, **Plot**, **Pitch**, each with its
own ribbon tabs (customisable, as now) and recent files; the explorer, search, tabs, the agent panel
and automation are shared. Opening a file goes to its workspace (setting: switch / ask / stay).

| Workspace | Stage 1 opens | Reading view | Side panel |
|---|---|---|---|
| **Page** | `.docx` | reflowed, like the Markdown view: headings, paragraphs with character styles, lists with numbering, tables, inline images, links, footnotes as end notes; width and font size the reader's | outline (headings), the layers map |
| **Plot** | `.xlsx` | a virtual grid (only visible cells drawn): cell values as saved, a first set of number formats, column widths, merged cells, frozen panes, sheet tabs; formulas shown in the cell inspector, not recalculated | sheets, cell inspector (value, formula, format) |
| **Pitch** | `.pptx` | one slide fitted to the window, a thumbnail strip, notes below; full-screen show with keys and swipes | slide list with titles, notes |

Plain text and source files, `.md` included, stay in Code. **CSV stays in Code** too, with what it
has today: the table view and editing as text. The more advanced spreadsheet features (number
formats, sorting and filtering, formulas, frozen panes, several sheets) belong to Plot, which opens
`.xlsx` and, on request (**Open in Plot**), a CSV file.

## Stage 1: the minimum viable product, read-only

A user opens a `.docx`, an `.xlsx` or a `.pptx` from the explorer or Open, and reads it
comfortably in its workspace: the document reflowed, the sheet as a grid, the deck slide by slide.
Text can be selected and copied; nothing is edited or saved. An agent reads the same files as
structure through the automation connector and MCP. OOXML only (Word, Excel, PowerPoint 2007 and
later; also what Google Docs and LibreOffice export to these formats).

| # | Step | Done when |
|---|---|---|
| 25.0 | **Five workspaces.** `WorkspaceMode` gets three values; its 48 uses stop assuming two; ribbon tabs per workspace; a switcher (desktop: title bar segmented control; phone: a sheet of five); settings and automation by name; empty states with "Open..." | switching, ribbon customisation and automation work for all five on desktop and phone; UI tests per workspace |
| 25.1 | **Shared renderer.** `model/rich`, `RichBuilder`, `MarkdownReader`, `RichLayout`, `RichPainter`, `RichHit` (select and copy); `MarkdownView` on top of them | every existing Markdown test passes; agent answers and `.md` files look the same (screenshot comparison on Linux); hit-test unit tests |
| 25.2 | **Package reading.** `ZipArchive` (central directory, stored + deflate via stb, size and ratio limits), `XmlReader` (pull parser: no DTD, no external entities, the five predefined entities; depth limit), OOXML content detection in `FileKind` (`Document`, `Spreadsheet`, `Presentation` from `[Content_Types].xml`), relationships (`_rels`) and the theme part (`theme1.xml`: colours and fonts) shared by the three readers | unit tests on files the tests build; zip-bomb, entity and depth tests fail safely; fuzz targets on Linux CI |
| 25.3 | **Page.** `DocxReader` into `RichBuilder`: styles with inheritance (docDefaults -> style -> direct), headings, numbering, tables, inline images, hyperlinks, footnotes; the Page workspace's reading view and outline; anything not understood shown as a marker, never dropped silently | a sample set of real files (`tests/data/office/`, licences checked) matches expected text dumps |
| 25.4 | **Plot.** `XlsxReader` into `Workbook` (sparse columns of typed runs, shared strings, styles, column widths, merged cells, frozen panes), `NumberFormat` (general, fixed decimals, thousands, percent, currency, the common dates and times, sections), the grid view and cell inspector; **Open in Plot** for a CSV document (read through today's `Csv` parser into `Workbook`; Code keeps the file and its editing) | sample set reads; a CSV shows the same cells in Code's table view and in Plot; a 1 million-cell sheet opens in under a second and scrolls at frame rate; memory under 2x the cell text |
| 25.5 | **Pitch.** `PptxReader` into `Presentation`: slides of positioned shapes (text boxes as `RichDocument`s, pictures, tables, rectangles and ellipses with fill and line), placeholders inherited from layout and master, theme colours, notes; slide view, thumbnails, show mode | sample set reads; slides match a reference rendering of each sample by eye, then by a stored screenshot |
| 25.6 | **Agents.** `get_state` names the workspace; MCP tools `read_document_outline`, `read_paragraphs`, `read_range`, `read_slide`; automation elements per workspace | an agent reads a document, a range and a slide without screenshots |

## Stage 2: limited editing

- **Page:** `RichEditor`: caret, selection, typing, Backspace/Delete, Enter (split paragraph),
  Bold/Italic/Underline, paragraph style (Normal, Heading 1-3), undo/redo; saving rewrites
  `word/document.xml` from the model, every part and element the model kept as opaque written back
  unchanged. Done when edit-save-reopen tests pass, an untouched document saves to the same XML
  after canonicalisation, and Word and LibreOffice open the saved samples.
- **Plot:** edit constants in cells, insert or delete a row; the workbook marked for full
  recalculation on open by Excel or LibreOffice; then a small formula engine (arithmetic, `SUM`,
  `AVERAGE`, `IF`, references) for the values we change; sort and filter. A CSV opened in Plot
  saves back as CSV (values only; Plot says what CSV cannot keep).
- **Pitch:** edit text in text boxes with `RichEditor`; then move and resize shapes.

## Stage 3: old binary formats, ported from LibreOffice

`.doc` (Word 97-2003), `.xls` (Excel 97-2003, BIFF8), `.ppt` (PowerPoint 97-2003) and `.rtf`, read
into the same models as their OOXML counterparts, so the views, agents and tests are the same.
These parsers cannot be rewritten from the specifications alone ([MS-DOC], [MS-XLS], [MS-PPT] are
incomplete and real files depart from them); LibreOffice's are the result of decades of real files.
So their **format layer is ported**: copied, then adapted to our types and builders. Only the
import side; LibreOffice's export, the application glue and the UI stay behind.

| Part | LibreOffice source (lines, upstream `9aad61820990`) | What is ported | Replaced by ours |
|---|---|---|---|
| OLE2 compound file (the container of all three) | `sot/source/sdstor` (9,600) | storage, directory, FAT/mini-FAT streams | `SvStream` -> a byte reader over the file in memory |
| `.doc` | `sw/source/filter/ww8`: `ww8scan`, `ww8struc`, `sprmids` (12,200: FIB, piece table, PLCFs, SPRMs); `ww8par*`, `ww8graf*` (31,100: mapping onto Writer) | the format layer whole; from the mapping, the rules only | `SwDoc` calls -> `RichBuilder` |
| `.xls` | `sc/source/filter/excel` and `inc`: `xi*`, `xl*`, `impop`, `read`, `excform*` (44,600: records, SST, XF styles, formula tokens, names) | records, strings, styles, number formats, formula tokens to text | `ScDocument` calls -> `Workbook` |
| `.ppt` | `svx/source/svdraw/svdfppt.cxx`, `filter/source/msfilter/msdffimp.cxx` (Escher / OfficeArt shapes), `sd/source/filter/ppt` (14,200) | records, text atoms, Escher shapes and pictures | `SdrObject` -> `Presentation` shapes |
| `.rtf` | `sw/source/writerfilter/rtftok` (17,100: tokenizer and destinations) | the tokenizer and the destination state machine | `dmapper` -> `RichBuilder` |

How:

- **Where:** `src/model/office/legacy/`, one folder per format. A file ported from LibreOffice
  keeps its MPL-2.0 header (and the Apache-2.0 notice it carries from OpenOffice.org), adds one
  line naming the upstream file and commit, and stays MPL-2.0; that is file-level copyleft, so the
  rest of EasyNotepad is unaffected, but these files' source must stay available (it is, in the
  repository). `NOTICE` lists them. **Before this stage: EasyNotepad's own licence must be settled**
  (the repository has no `LICENSE` yet).
- **A thin shim, then rewrite inward:** `legacy/LoCompat.h` maps the few `sal`/`tools` types the
  format layer uses (`sal_uInt16`..., `OUString` to UTF-16 strings, `SvStream` reads) so the first
  port is close to the original and diffs against upstream stay readable; the shim shrinks as files
  are adapted. No UNO, VCL, `SfxItemSet` or `comphelper` comes along.
- **Order:** OLE2 storage, then `.xls` (most self-contained), `.doc`, `.ppt`, `.rtf`. Encrypted files
  (RC4, XOR obfuscation) say so and are not opened.
- **Upstream fixes:** the upstream commit is recorded; a script lists upstream changes to the ported
  files since then, to take security and correctness fixes by hand.
- **Tests:** LibreOffice's own sample files for these filters (`sw/qa/extras/ww8import/data`, `sw/qa/core/data/ww8`,
  `sc/qa/unit/data/xls`, `sd/qa/unit/data/ppt`, `sw/qa/extras/rtfimport/data`: about 250 files; MPL-2.0 test data) as the regression set, compared
  as text dumps; the ported parsers fuzzed like the others (they read untrusted bytes, and the
  original code had its share of fuzzing bugs).

## Later

- **ODF** (`.odt`, `.ods`, `.odp`) readers into the same models.
- **More of the format:** charts (drawn from their cached data), more shapes, a paginated (print) layout for
  documents, comments and tracked changes shown, PDF reading (PDFium).

## Rules for every step

- Readers are pure functions from bytes to model: no I/O, no UI, fuzzable, run on a background
  thread with progress and cancel (like the symbol index).
- From stage 2, nothing is lost on save: anything the model does not understand is kept and
  written back. Stage 1 keeps the parts it skips counted, so a document says what it did not show.
- Memory scopes `rich`, `office-page`, `office-plot`, `office-pitch`; models, layouts and textures
  freed with their document; the open/close memory test opens one file of each kind.
- Hostile files: total and per-entry inflated size, compression ratio, entry count, XML depth and
  attributes are limited; a broken part shows what was read and says what was not.
- No new third-party library in stage 1 (stb's inflate; our own XML pull parser).

## Settled

- The workspaces are **Page** (documents; "Text" was dropped as it reads like plain-text files), **Plot** (spreadsheets, and later their charts), **Pitch** (presentations).
- Stage 1 is read-only for all three, and reads `.docx`, `.xlsx` and `.pptx` only.
- CSV keeps its basic view and editing in Code; advanced spreadsheet features are Plot's.
- The old binary formats are ported from LibreOffice's code (stage 3), not converted by an
  installed LibreOffice.
