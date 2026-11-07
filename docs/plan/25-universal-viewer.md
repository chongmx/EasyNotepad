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

Not yet: encrypted PDFs, ZIP64, the rendering gaps below, the workspaces themselves (25.0).

## Rendering gaps against Microsoft Office and LibreOffice

Listed 2026-11-07 from the code, screenshot tours of `C:\cpp\office-samples` (27 files) and the
challenge files, and one reference render made with Microsoft Office (Word, Excel and PowerPoint
16 export to PDF, rasterised with PyMuPDF). The side-by-side comparison of every sample against
Office's render is the next step (see **Reference renders** at the end of this section); this list
is to be checked and ranked against it. Priority: **A** visible in most real files, **B** common,
**C** occasional.

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
| A | **No pages:** the document is one reflowed column; page size, margins, page breaks, section breaks and columns are ignored | a paginated layout (25 "Later": print layout) on the display list; reflowed stays the reading default |
| A | **Floating pictures, shapes and text boxes** (anchored, with text wrapping) are put after their paragraph | position anchors relative to page / margin / paragraph and wrap text around them; needs pages |
| A | **Tab stops** become four spaces: right, centre and decimal tabs and dot leaders (tables of contents) are lost | read `w:tabs` per paragraph and lay text out to them |
| B | Table column widths come from the content, not `tblGrid` / `tcW`; text alignment inside cells is not applied; nested tables are flattened; pictures in cells move after the table; table style borders and shading partly | table layout from the grid; cells holding blocks (the shared renderer's table cell model, 25.1) |
| B | Headers and footers: only the last section's default, shown once; first-page and even-page headers and page-number fields per page | with pages |
| B | Footnotes at the end instead of at the page bottom; comments as a list instead of margin balloons; tracked changes: deletions hidden, insertions plain (Word shows markup) | a markup mode: insertions underlined and coloured, deletions struck |
| C | Paragraph borders, drop caps, `w:sym` symbol characters (drawn as a box), equations (OMML) laid out as math, line numbers, watermarks | per item |

### Excel

| Pri | Gap | What it takes |
|---|---|---|
| A | **Conditional formatting** is not applied: cell rules, colour scales, data bars, icon sets | evaluate `cfRule` against the cached values (no formula engine needed for cellIs, top/bottom, colour scales, data bars) |
| A | **Table styles** ("Format as Table": header colour, banded rows) are not drawn | read `xl/tables/*.xml` and the built-in table styles, like PowerPoint's table styles |
| B | Numbers too wide for their column are clipped; Excel shows `####` | measure, then `#` fill |
| B | Italic not drawn; font families ignored | the font work under All formats |
| B | Only solid fills: pattern fills (`gray125`, hatches) and gradient fills are not drawn | pattern bitmaps / the gradient code the page painter has |
| C | Centred and right-aligned text does not run over empty cells to the left; indent and shrink-to-fit ignored; diagonal borders; thick borders partly covered by the next cell's fill | per item |
| C | Chart sheets, sparklines, comment markers, data validation arrows, outline (grouping) bars, hidden-by-filter rows | per item |

### PowerPoint

| Pri | Gap | What it takes |
|---|---|---|
| A | **SmartArt without a cached drawing** is an outline of its text (38 of LibreOffice's samples; `lo-smartart1.pptx`: PowerPoint draws five blue blocks, we draw a bulleted list) | lay out from the data model by the layout's `uniqueId`: the common layouts first (basic block list, process, cycle, hierarchy, pyramid), the quick style and colour parts for the look |
| A | **Text autofit** (`normAutofit fontScale`, `lnSpcReduction`) and the substituted font make text overflow or break differently | apply the scale; with metric-compatible fonts, breaks match |
| B | 40 of about 180 preset shapes; the others fall back to a simpler outline | port the preset geometry definitions (`presetShapeDefinitions.xml` formulas) instead of hand-written outlines |
| B | Picture, texture and pattern fills inside shapes; picture bullets | image fill clipped to the shape path |
| B | Text effects: gradient or outlined text, letter spacing (`spc`), columns in text boxes, superscript / subscript offsets | per item |
| C | Table borders from the table style (`tcBdr`) are skipped; diagonal cell borders | read them with the fills |
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
| C | Vertical writing (`Identity-V`), Type 3 fonts as their glyph procedures, encrypted files | per item |

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
| A | Gap | Word table styles: the tables' orange header rows and lines (a built-in "Grid Table" style) are not drawn | poi-table-alignment |
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
- **More of the format:** the rendering gaps above, a paginated (print) layout for documents,
  comments and tracked changes shown. (Charts and PDF reading are done, with our own readers.)

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
