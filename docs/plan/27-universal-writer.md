# 27 · A universal writer: Page, Plot and Pitch workspaces for editing

**Why:** reading Office documents and PDFs is done ([plan 25](done/25-universal-viewer.md)): they
open in a tab of the Code workspace, and need nothing more to be read. Editing them is different:
a document being written wants its own ribbon (styles, fonts, paragraph, insert), its own side
panels (outline, cell inspector, slide list) and a renderer that knows where the caret is. So the
separate workspaces are for **editing only**:
- **Viewing stays in Code.** Opening a `.docx`, `.xlsx`, `.pptx` or `.pdf` keeps opening the
  viewer in a Code tab, as today.
- **Edit** on the viewer's toolbar (and in its tab's menu) opens the same document in its
  workspace: **Page** for `.docx`, **Plot** for `.xlsx` (and a CSV on request), **Pitch** for
  `.pptx`. PDFs stay read-only.
- The model, readers and painters are plan 25's; the workspaces add editing on top of them.

**Status:** planned. Moved here from plan 25 on 16 Nov 2026: the workspaces (25.0), the shared
renderer (25.1), agents reading and changing documents as structure (25.6), stage 2 (limited
editing), stage 3 (the old binary formats), ODF, and the reading gaps still open.

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
is ported (copied and adapted, MPL-2.0 kept; 27.6 below). What we do not take at all: macros,
databases, printing, page layout, and the plumbing.

## Starting point

- **Readers and painters exist** (plan 25): `model/package/` (ZIP, XML), `model/viewer/` (Docx,
  Xlsx, Pptx, Pdf, charts, shapes, number formats, conditional formats), `view/viewer/`
  (`ViewerView`, `PagePainter`), on `IDocumentReaderService` and `ViewerViewModel`. Word's flowing
  text is drawn by `MarkdownView::renderBlocks`, which measures by drawing and keeps no positions:
  nothing can be hit-tested, so there is no caret and no selection.
- **Workspaces:** `WorkspaceMode { Agent, Code }` is a two-value switch used across the view-models
  and views; most uses test `== Agent`, ribbon tab settings are per workspace, and automation
  reports `"agent"` or `"code"`.
- Nothing writes a zip or OOXML yet.

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
             run, table, image)    Styles: named, inheriting    RichEditor   (27.3) caret,  
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
  and back: 27.1 uses it to select and copy text, 27.3–27.5 to place a caret and edit.

## Workspaces

The switch becomes five: **Code**, **Agent**, **Page**, **Plot**, **Pitch**. Page, Plot and Pitch
are entered by **Edit** on a document (or by opening a recent document there), not by opening a
file: a file always opens for reading in Code first. Each has its own ribbon tabs (customisable, as
now) and recent files; the explorer, search, tabs, the agent panel and automation are shared.

| Workspace | Edits | Editing view | Side panel |
|---|---|---|---|
| **Page** | `.docx` | the document reflowed to the window or as pages, with a caret | outline (headings), styles |
| **Plot** | `.xlsx`; a CSV on request (**Edit in Plot**) | the grid with a selected cell and an edit line; formulas shown and edited in the cell inspector | sheets, cell inspector (value, formula, format) |
| **Pitch** | `.pptx` | one slide with its shapes selectable, a thumbnail strip, notes below; full-screen show with keys and swipes | slide list with titles, notes |

Plain text and source files, `.md` included, stay in Code. **CSV stays in Code** too, with what it
has today: the table view and editing as text. Plot adds the spreadsheet features (number formats,
sorting and filtering, formulas, frozen panes, several sheets).

## Steps

| # | Step | Done when |
|---|---|---|
| 27.0 | **Five workspaces.** `WorkspaceMode` gets three values; its uses stop assuming two; ribbon tabs per workspace; a switcher (desktop: title bar segmented control; phone: a sheet of five); settings and automation by name; **Edit** on the viewer opens the document in its workspace, and closing it there returns to the viewer | switching, ribbon customisation and automation work for all five on desktop and phone; UI tests per workspace |
| 27.1 | **Shared renderer.** `model/rich`, `RichBuilder`, `MarkdownReader`, `RichLayout`, `RichPainter`, `RichHit` (select and copy); `MarkdownView` and the Word viewer on top of them | every existing Markdown and viewer test passes; agent answers, `.md` files and Word documents look the same (screenshot comparison on Linux); hit-test unit tests |
| 27.2 | **Agents read and change structure.** `get_state` names the workspace; MCP tools `read_document_outline`, `read_paragraphs`, `read_range`, `read_slide` (the plain text of a viewer document is plan 26 step 1); later the matching edits | an agent reads a document, a range and a slide without screenshots |
| 27.3 | **Page editing** (below) | edit-save-reopen tests pass; Word and LibreOffice open the saved samples |
| 27.4 | **Plot editing** (below) | as above for Excel |
| 27.5 | **Pitch editing** (below) | as above for PowerPoint |
| 27.6 | **Old binary formats** (below): `.doc`, `.xls`, `.ppt`, `.rtf` read into the same models | their LibreOffice sample sets read as text dumps; fuzzed |

## Editing (27.3–27.5)

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

## Old binary formats, ported from LibreOffice (27.6)

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
- **More of the format:** the reading gaps below, comments in the margin and tracked changes shown
  as markup.

## Reading gaps (carried over from plan 25)

Not editing, but the same documents: what the viewer still draws differently from Office, ranked.
Priority: **A** visible in most real files, **B** common, **C** occasional. Also open: ZIP64, and
PDFs encrypted with certificates. The four **Gap** rows of [plan 25's check](done/25-universal-viewer.md) against Office's
render (fonts in slides, a chart's picture background, a title slide's anchoring, white table
lines in slides) belong here too.

### All formats

| Pri | Gap | What it takes |
|---|---|---|
| A | **One font for everything.** Text is drawn in Roboto (bold by drawing twice, no true italic in cells); Calibri, Cambria, Times New Roman, Arial and PDFs' embedded fonts are not used, so text widths and line breaks differ from Office | load the document's fonts on demand from the system (as `ScriptFonts` does for CJK), with metric-compatible substitutes when missing (Carlito for Calibri, Caladea for Cambria, Liberation for Times / Arial / Courier); bold and italic faces; PDF embedded TrueType / CFF outlines through the font loader |
| B | **No text shaping:** Arabic and Hebrew come out in logical order without joining; Thai, Devanagari and other scripts that need shaping are wrong | a bidi pass (UAX #9) and shaping (HarfBuzz is the standard; a new dependency, so a decision first) |
| B | CJK only renders when a system CJK font is installed | ship or download a small CJK subset font, or say so in the document's "not shown" note |
| C | Shadows, glow, reflection, soft edges, 3D bevels | outer shadow first (an offset, blurred copy of the shape); the rest later |

### Word

| Pri | Gap | What it takes |
|---|---|---|
| B | Text beside a floating picture is narrowed for the whole paragraph: Word narrows only the lines beside the picture; floating shapes and text boxes are still put after their paragraph | line-by-line wrap in the paragraph layout (each line's room from the floats it meets); shapes through the slide painter |
| B | Nested tables are flattened; pictures in cells move after the table | cells holding blocks (the shared renderer's table cell model, 27.1) |
| B | Headers and footers: only the last section's default; first-page and even-page headers, columns | per section |
| B | Comments as a list instead of margin balloons; tracked changes: deletions hidden, insertions plain (Word shows markup) | a markup mode: insertions underlined and coloured, deletions struck |
| C | Paragraph borders, drop caps, `w:sym` symbol characters (drawn as a box), equations (OMML) laid out as math, line numbers, watermarks | per item |

### Excel

| Pri | Gap | What it takes |
|---|---|---|
| B | Conditional formats with formulas (`expression`), time periods; Excel 2010 data bars (`x14`: solid, negative bars, axis) | a small formula evaluator for the common forms (`MOD(ROW(),2)=0`, comparisons with `$A1`) |
| B | Table styles' borders from a custom style's dxfs; Dark 8-11 and Medium 15-21 only approximated | read dxf borders; check against Excel's render |
| C | The top border of row 1 sits under the column headers; thick borders partly covered by the next cell's fill | draw borders after all fills |
| C | Chart sheets, sparklines, comment markers, data validation arrows, outline (grouping) bars, hidden-by-filter rows | per item |

### PowerPoint

| Pri | Gap | What it takes |
|---|---|---|
| B | SmartArt: the exact layout (sizes, spacing, connector shapes), quick styles and colour variations, layouts outside the families above | the layout part's own algorithm and constraints (`dgm:layoutNode`), a large piece of work |
| B | 60 of about 180 preset shapes; the others fall back to a rectangle | port the preset geometry definitions (`presetShapeDefinitions.xml` formulas) |
| B | Picture fills in non-rectangular shapes (clipped to the shape), tiled pictures, picture bullets | a clip to the shape's path in the painter |
| B | Gradient or outlined text, columns in text boxes, shadows and glow | per item |
| C | Video and audio show at most their poster frame; OLE objects their preview image | say so in "not shown" (done for media) |
| C | Charts are drawn flat: 3D charts, chart styles, data labels positions, trendlines and error bars, `chartex` (waterfall, treemap, sunburst, histogram, box) | per chart kind |

### PDF

| Pri | Gap | What it takes |
|---|---|---|
| A | **Glyph shapes** come from Roboto, words stretched to their width: serif, monospace and display fonts all look sans | draw the embedded font programs (TrueType, CFF, Type 1) through the font loader, or rasterise glyph outlines into the atlas |
| B | **Clipping** uses the clip path's bounding box, not its outline (rounded corners, circles, text clips) | stencil the path, or clip polygons on the CPU for simple paths |
| B | JPEG 2000, JBIG2 and CCITT images (scanned documents) are not decoded | decoders (OpenJPEG and jbig2dec are the usual ones; new dependencies) |
| B | Annotations and form fields (their appearance streams) are not drawn | draw `/AP` normal appearances as forms |
| C | Mesh shadings (types 4-7), tiling patterns, blend modes, transparency groups and soft masks on content | per item |
| C | Vertical writing (`Identity-V`), Type 3 fonts as their glyph procedures, files encrypted with certificates (public-key security) | per item |
| C | Pictures are kept at 3 pixels per point of the size the page shows them (216 dpi at 100 %): sharp up to about twice the size on a high-density screen, soft beyond | read a page's pictures again at the zoom shown once it settles (levels of detail) |

## Rules for every step

- Readers are pure functions from bytes to model: no I/O, no UI, fuzzable, run on a background
  thread with progress and cancel (like the symbol index).
- Nothing is lost on save: anything the model does not understand is kept and
  written back. The parts a reader skips stay counted, so a document says what it did not show.
- Memory scopes `rich`, `office-page`, `office-plot`, `office-pitch`; models, layouts and textures
  freed with their document; the open/close memory test opens one file of each kind.
- Hostile files: total and per-entry inflated size, compression ratio, entry count, XML depth and
  attributes are limited; a broken part shows what was read and says what was not.
- A new third-party library (HarfBuzz, OpenJPEG, jbig2dec...) is a decision of its own, first.

## Settled

- Viewing needs no workspace: documents are read in Code (plan 25). The workspaces exist for editing.
- The workspaces are **Page** (documents; "Text" was dropped as it reads like plain-text files),
  **Plot** (spreadsheets, and later their charts), **Pitch** (presentations).
- CSV keeps its basic view and editing in Code; advanced spreadsheet features are Plot's.
- The old binary formats are ported from LibreOffice's code (27.6), not converted by an
  installed LibreOffice.
