# Changelog

What changed in each release of EasyNotepad. The version is in the `VERSION` file; see
[docs/RELEASING.md](docs/RELEASING.md) for how a release is made. The section of a release is
its release notes on GitHub.

## [1.3.0] - unreleased

### Added
- Encrypted PDFs open. One that only restricts printing or copying opens at once. One with a
  password shows a field to type it; a wrong one is said to be wrong, and either the user's or the
  owner's password opens it. Every method of PDF's standard security is read: RC4 (40 to
  128-bit keys), AES-128 and AES-256 (PDF 1.7 extension level 3, and PDF 2.0). The password is not
  kept: it is asked again the next time the file opens. Files encrypted for certain people (with
  their certificates) still cannot be opened, and say so.
- A font for Chinese, Japanese and Korean comes with the app (Noto Sans CJK, the 12,455 letters in
  common use, 2.5 MB). Documents use it when the system has no such font the app can read.
- Search in PDF and PowerPoint documents: Ctrl+F, or the magnifier on the viewer's toolbar (on a
  phone too), finds text on every page. Matches are highlighted where they are drawn, the current
  one outlined; Enter, F3 and Shift+F3 go to the next and previous, round the document, and the
  find bar counts them ("3 of 11", with "+" while pages are still being searched). Pages not
  shown yet are read for their text in the background, after the pages in view. Case is ignored
  unless "Aa" is on, ligatures match their letters, and a line break matches a space, so a phrase
  is found across lines. In two columns, the column with the keyboard goes to each match; the
  other, when it shows the same document, keeps its place and shows the highlights.
- The local agent works in the open folder with the app's own tools, and reads, looks and listens.
  It calls them by itself (each step shows in the chat): it lists the folder, finds text, reads
  files by line range, reads PDF, Word, Excel and PowerPoint text a few pages at a time, looks at a
  page in the viewer as a picture (zoomed, a region of it) for tables and charts, looks at pictures,
  listens to recordings (WAV, MP3, FLAC), and opens files to show you. Its edits are proposals,
  listed under the conversation to review side by side, accept or reject; with *Plan only* it has
  none. It cannot leave the open folder. Pictures and sound need its eyes and ears, Gemma 4's
  vision and audio encoders from Google (987 MB, *Let it see and hear* in the agent set-up),
  loaded only for a question that needs them. Skills in `resources/skills/` teach it each kind of
  task. Tested with Gemma 4 E2B on the CPU: it read a datasheet's numbers, described a chart on a
  slide and took down a voicemail. The engine library's interface is version 2: an
  `enp-engine-cuda` built before this is not loaded (the CPU answers) until it is replaced.
- The local agent does office work and remembers: `query_table` (filter, sort, group and total CSV
  and Excel sheets) and `calculate` (exact sums, percentages); `transcribe` writes a meeting down
  word for word into a text file next to it, with times, for minutes; `inspect_binary` and
  `read_bytes` read programs and libraries as the binary view does; how carefully it looks at a
  picture is chosen per look (low, normal, high for OCR); pictures show in the chat; a memory
  between conversations keeps your preferences, its lessons and notes, and is corrected and
  compacted by the agent. Small-model slips are caught: loose JSON, calls written as text, calls
  announced but not made, a preference said but not saved. Tested with Gemma 4 E2B on Microsoft's
  Financial Sample workbook, a photographed receipt, a mock meeting and a test DLL
  (`C:\cpp\agent-samples`, a small downloaded set, outside the repository).
- The local agent takes a question in steps: it first splits it into steps (the `planning` skill,
  at most six), then the app gives it one step at a time and asks for the answer from what the
  steps found. The plan and each step show in the chat; a simple question is one step and goes
  as before. With Gemma 4 E2B, "which country made the most profit, how much, and what share of
  the total" went from an invented figure to three tool-backed steps and the right answer (19 s
  on an RTX 5060).
- TIFF and WebP pictures open. TIFF is decoded by the app itself: strips and tiles, uncompressed,
  PackBits, LZW, Deflate and CCITT fax (Modified Huffman, Group 3, Group 4), 1- to 16-bit grey,
  palette, RGB(A), CMYK, the predictor; tested pixel for pixel against files from Windows' own
  encoder. WebP comes from libwebp 1.5.0 (a new submodule, decoder only): lossy, lossless, alpha.
  Not yet: JPEG inside TIFF, BigTIFF, animated WebP.
- The local agent zooms: `look_at_image` and `look_at_page` take a `region` (halves, thirds,
  quarters, corners, the centre, or a box in percent), a `zoom` and the point to zoom `at`. A
  picture is cut from the full-size file; a page is zoomed in the viewer until the part fills it,
  so small print is read from real pixels. Each picture is sized to the tokens it is given (70 to
  1,120, one per 48 x 48 pixels), and the agent set-up has a minimum for every look (up to always
  1,120). A look at an empty part says so. Page looks were also cut from the wrong place on
  screens where the window is not at the screen's corner, and missed a document opened by the
  same call: both fixed.
- The agent set-up shows "Take questions in steps"; both settings are saved.
- Retries: a call to the wrong tool for a file, a region it cannot read, or a filter that matches
  no rows (the reply now lists the values the column holds, and "Sunday" finds "Sun") is followed
  by "make the corrected call now" instead of a report of the failure.
- `tests/agent-bench`: 27 questions with known answers over the sample files, and a runner that
  asks them in the running app and grades the answers.
- The local agent works across the folder's documents: `search_documents` searches inside every
  PDF, Word, Excel, PowerPoint and text file (or lists them with kind, pages and title);
  `extract_from_documents` asks one question of many documents, each in a pass of its own (a long
  one part by part, what it found carried along), and returns a table; working notes (`note`,
  `read_notes`) hold what it found, are repeated at each step, and start again with a new chat.
  The `multiple-documents` skill teaches the way: find, extract per document, note, calculate,
  answer with sources. The planner sees what the folder holds (documents or tables) and plans with
  real names. Four invoices in different layouts: total and largest supplier right in 17 s.
- Three local models to choose from in the agent set-up: Gemma 4 E2B (4.3 GB, fastest), E4B
  (6.1 GB, better at questions with several parts) and 12B (7.2 GB, the most careful), each with
  its own eyes and ears and its own MTP drafter (speculative decoding), all pinned by commit and
  SHA-256. Google publishes the drafters (its "assistant" checkpoints) only as unquantized weights:
  E2B's, E4B's and 12B's are community GGUF conversions of them. On an RTX 5060 laptop GPU, E4B
  generates 65 tokens/s alone and 112 with its drafter (56% of drafted tokens accepted). The
  drafter is fetched with the model by itself and used: on for E2B and E4B; off for 12B, which
  with it no longer fits an 8 GB card and slows from 33 to 20 tokens/s. Power users turn it on or
  off per model in the Engine Lab (Agent uses the drafter).
- Scanning with the local model: `scan_document` shows a long document (or several) to the model
  in overlapping windows (small, normal or large; overlap in percent), one at a time with what is
  looked for, and returns the pages and quotes where it is; `scan_image` shows a picture in a grid
  of overlapping tiles and cuts again, finer, the tiles where it was seen (coarse to fine, up to 3
  levels), and returns the regions to look at closely. The planner is given the real columns of
  the tables a question names, and plans counts, totals and averages as one query_table step.
- Numbers: the local model no longer works figures out itself. An answer with a figure no tool
  gave and the user did not write goes back to it, to be done with `calculate`; `calculate` takes
  several sums at once, named ("total = 86.40 * 1.18; each = total / 3"). Counting stays its own.
- Tools: the file open in the editor is named, not pasted, into a question (it had taken over
  questions about other files); `read_document` lists a document's tables and figures with their
  pages and says which pages it left out; `query_table` reads conditions joined by commas, `&&`
  or `==`, "sum" of the one column named, and gives totals unsorted rather than failing on a sort
  it cannot do; a guessed file name gets "does not exist" with what the folder holds;
  `inspect_binary` shows where a forwarded export goes; each tool step in the chat shows the
  first line of what the tool answered. Benchmark: 16 of 25 before, 20 of 25 after the number and
  tool fixes.
- The local agent runs on an NVIDIA GPU when `enp-engine-cuda` (llama.cpp with its CUDA backend,
  a separate download, built by CI for Windows x64 and Linux x64) is next to the executable. On an
  RTX 5060 laptop it writes 105 tokens/s (185 with the drafter) against 19 on its CPU, and reads a
  prompt at about 7,400 tokens/s. It is optional: without it, or when it
  cannot start (no NVIDIA GPU, an old driver, a missing cuBLAS DLL) or fails while answering
  (a crash, a hang, out of memory), the CPU answers as before. It is loaded in a helper process
  (`EasyNotepad --local-engine`), so even a crash in it or in the driver leaves the editor running.
  The Engine Lab says whether it is used, and why not.

### Changed
- The app has its logo as its icon everywhere, instead of each system's generic one: the Windows
  executable, its windows and the taskbar; the macOS Dock; Linux windows (X11), and the menu
  entry and theme icons an installed copy has (a `.desktop` file; Wayland shows its windows with
  it); Android's launcher (an adaptive icon, round and themed where the launcher asks for them);
  the web page's tab. All are drawn from the logo by `tools/icons/make_icons.cpp`
  (`resources/icons`).
- Preferences are in tabs: Appearance, Editor, Files, Performance and Workspace. The Enhanced UI
  settings fold away under Appearance. The dialog keeps its size from tab to tab and fits a phone.
  The tab row scrolls with the mouse wheel or a finger, without arrow buttons; a sideways swipe
  across a page moves to the next or previous tab.
- Word, Excel, PowerPoint and PDF no longer show "Open as text" on their toolbar. Preferences >
  Files brings it back. A document that cannot be shown still offers its text.
- The ribbon's tabs in the Code workspace are in a new order: Home, Edit, View, Tools, Git, Read,
  Review, Document (then My Tools). An order you set yourself is kept.
- The Dear ImGui version is locked: configuring stops when its submodule is at another commit than
  `patches/imgui/BASE` names, and the submodule no longer follows a branch.

### Fixed
- The web version stopped at its start with a blank page (since 1.2.0 at least): reading a file
  put a 64 KB buffer on the stack, which is the browser build's whole stack. Files are now read
  straight into memory, and the web build has a 1 MB stack, a Windows main thread's.
- Open on the web shows the browser's file picker, so files on the user's computer can be opened;
  it showed the in-app browser, which sees only the page's own files.
- A pinch (or Ctrl+wheel) on Word, Excel, Markdown and pictures made the content jitter and slide
  away from the fingers; only PDF and slides zoomed about the point under them. Word and Excel
  laid out at the old size while scrolling for the new one, and none of them grows in proportion
  to its zoom (page gaps and headers keep their size; reflowed text wraps anew). Each now finds
  the point under the fingers in its own terms when the pinch starts (a page and a point on it, a
  place in the cells, a paragraph and how far into it, a pixel of the picture) and keeps it there
  at every size.
- Touch: a finger dragging a slider or a splitter in a view that scrolls the same way scrolled the
  view instead (the blame column's edge in the editor, for one). Sliders, splitters, the minimap
  and the map now keep a finger's drag along the way they move; a drag the other way still
  scrolls.
- A picture opened from outside the open folder (on a phone: from the Download folder while the
  sample workspace is shown) said "cannot be shown: not an image". It was given the sample
  workspace's text instead of the file. Files by their full path are now always read from disk.
- File types are recognised more carefully. A text file that starts with the letters of a
  format's signature ("BMW models", "MZ-80 notes", "ID3 tags") opens as text, not as a
  picture or a program; a file is taken for a COFF object (no signature of its own) only
  after every format that has one. An uncompressed TGA picture was taken for a cursor (its
  first bytes are the same) and could not be shown; icons and cursors now need a valid icon
  directory. Every picture format is tested opened from outside the folder: PNG (colour,
  transparent, palette, grey, 16-bit), JPEG (baseline, progressive, grey), GIF, BMP, TGA, PPM,
  PGM, QOI, ICO and Radiance HDR show; WebP, TIFF and JPEG 2000 say they are not supported yet.
- Android: the app closed a moment after showing a document with Chinese, Japanese or Korean
  text on phones whose system font for them it cannot read (Android 15 and later, such as
  Samsung's Galaxy S24: a variable font with CFF2 outlines). Every font is now checked before it
  is used, and one that cannot be read is skipped for another; at worst a letter shows as "?".
- On a phone, a finger that drags a list scrolls it and presses nothing: a touch reaches the app as a
  tap when the finger lifts without moving. Before, a file in the file list opened as soon as the
  finger touched it, so the list could not be dragged. Sliders and splitters still follow a drag.
- Word, Excel, PowerPoint and PDF in two columns: the viewer's toolbar was drawn over the
  column's name. It now starts below it, as it does below the find bar.
- The viewer's toolbar in a narrow column or window: the label (format, page) is cut short, then
  goes on a line of its own, and the buttons wrap within the column instead of covering the label
  or running past the edge. A column's name ends before its close button.
- PDF and slides: a filled shape of one concave outline (a logo's letters drawn as paths, such as
  SIMPLE·AI on an arXiv paper) was filled past its edges, as solid bars. Such outlines are now
  filled exactly, by the same sweep as shapes with holes.
- PDF: Chinese, Japanese and Korean text set in a fixed-pitch font (common in bank statements)
  showed as "????". It is now drawn with the system's CJK font, as other CJK text is.
- Two columns: the find bar's matches in the active document were also highlighted in the other
  column's document, on the same lines and columns.
- Two columns showing one PDF at pages far apart: each column let go of the pages the other was
  reading, so one of them could stay at "Reading the page..." or read its pages over and over. A
  page is now kept while either column is near it.

## [1.2.0] - 2026-11-14

### Added
- **Enhanced UI** (Preferences, or View > Theme): hints without a box, as translucent text with a glow
  in its shape, and a glow around the button, tab or toggle under the pointer, as in a game.
  The glow fades in and out and away from the control; a ribbon icon lights up from within (a
  richer colour, a soft light behind it) and its name glows; the big workspace button lights only
  its symbol and name. The glows breathe, swelling and ebbing slowly. Hints fade in, rising into
  place. Preferences > Enhanced UI sets the glow's intensity, reach and colour, the icon light,
  the fade times and the breathing.
- Agents can rest the pointer on a control (`hover`) to see its hover effect and hint, without
  moving the mouse.
- **Document viewer**: Word, Excel, PowerPoint and PDF files open read-only, drawn as their
  authors see them. Word: styles, numbering, run colours and sizes, alignment and indents,
  tables (aligned, cells of several lines), headers and footers, footnotes and comments, pictures
  and charts. Excel: number formats (dates, fractions, colour sections), fonts and sizes, rich
  text, fills, borders, wrapped and rotated text, merges, frozen panes, text running over empty
  cells, charts and pictures, and sheet tabs along the bottom. PowerPoint: masters and layouts,
  text styles, shapes with gradients, dashes and arrowheads, rotation, cropped pictures, tables
  with their table styles, charts and SmartArt. PDF: text with its fonts and spacing, ligatures,
  rotated text, paths, dashes, clipping, shadings and images, pages read as they come into view.
  Chinese, Japanese and Korean text uses a system font, loaded only while a document needs it.

### Changed
- Hints are a rounded, semi-transparent box without a border; a long hint wraps (about 55
  characters a line). The minimize, maximize and close buttons have no hint.

### Fixed
- Android: files and folders outside the app's own (Download, Documents, the whole shared
  storage, SD cards) could not be browsed or opened. The app now asks for "All files access"
  (Android 11 and later; the storage permission before), once at start (Allow, Not now, Don't
  ask again), and later in the Open and Open Folder dialogs, which say
  what is missing and open the system's page for it, then list the files when the user is back.
  Home is the shared storage, and its folders and SD cards are places of the dialog.
- Hints opened from the ribbon (and other places without padding) had their text touching the
  edges of a box too small for it.
- PDF and slides: a pinch (or Ctrl+wheel) did not zoom about the point under the fingers: the
  pages slid away while zooming, further the deeper into the document (a page and more, 90 pages
  down a long PDF), as the gaps between pages keep their size while the pages grow. The point
  under the fingers is now found on its page and kept there for the whole pinch.
- PDF pictures several times faster: JPEGs are decoded by the platform's own decoder straight at
  1/2, 1/4 or 1/8 of their size, about the size they are shown (Android 11 and later: the
  system's AImageDecoder, libjpeg-turbo with NEON; Windows: WIC; macOS: ImageIO; Linux: the
  system's libjpeg-turbo when installed, found at run time; elsewhere stb_image as before; all
  behind one function of the OS layer). Only grey and colour JPEGs drawn as stored go that way:
  CMYK JPEGs and ones an EXIF orientation would turn stay with stb_image, so a PDF's pictures look
  the same on every platform (Android's decoder always applies EXIF and inverts every CMYK JPEG).
  A JPEG soft mask is decoded the same way. Pictures of a page are decoded side by side on
  up to three threads while the page's text has a thread of its own; pictures are handed on as
  raw pixels (no encoding and decoding on the way) and shrunk with integer block sums; common
  colour spaces are converted without a per-pixel colour computation; compressed streams are
  inflated with libdeflate (pinned submodule). Shrunk pictures are kept per document (up to
  32 MB), so a page scrolled back to shows them at once, and the next page the reader goes to is
  read ahead.
- PDF: a picture still being read shows an hourglass, and one that cannot be shown a crossed-out
  picture, instead of the same empty box for both. A picture that two pages show, taken by one of
  them and let go, could stay an empty box on the other (seen on a phone): it is read again.
- PDF: scrolling fast up and down a long document left the page landed on at "Reading the
  page..." for a second or more, waiting behind every page scrolled past. A page's text now
  shows first (within a few milliseconds) and each picture follows on its own; the page being
  read is served first and reads no longer near the view are cancelled at once; nothing new is
  asked for while the pages fly past. On a 33-page paper with large photos, the page landed on
  shows its text in under 25 ms and its pictures in under half a second (desktop).
- Phones, the Open, Open Folder and Save As dialogs: the list had a sliver of the width beside a
  150 px places pane and the file name a sliver of the line. The places are now buttons above the
  list, which has the whole width and taller rows; the name has a line of its own, with the
  buttons below it; the dialog takes the whole screen.
- Phones: a tap on a folder in those dialogs goes into it (there is no double click on a finger).
- Picking a file in the Open dialog narrowed the list to that file (its name was taken for a
  typed filter), so a list of many files was left with one. The list keeps all its entries.
- Phones: the name of the open document in the top bar and the names in the list of open
  documents end in "..." when too long, instead of being cut off; a finger dragging the list of
  open documents scrolls it and never reorders it; the bottom bar steps aside while the on-screen
  keyboard is up, so the text keeps the height.

## [1.1.0] - 2026-11-06

### Added
- **Two columns**: two documents side by side (Ctrl+\\, or View > Columns). The column with the
  keyboard shows the active document; a click, or Ctrl+Alt+\\, moves to the other. Drag the
  divider to resize, double-click it for halves.
- **Keyboard Shortcuts**: every shortcut key in one list, filtered as you type (F1, or Help).
- **IntelliSense**: completion, Go to Definition (F12 or Ctrl+click) with Back and Forward,
  hover information, code from a comment (local or online model), C and C++ headers and
  standards.
- **VS Code editing keys**: multiple carets (Ctrl+D, Ctrl+Alt+Up/Down, Shift+Alt+I), line moves
  and copies, Ctrl+R recent folders and files, Ctrl+E terminal focus; VS Code workspaces and
  Visual Studio solutions open as a workspace.
- **Rendered Markdown**: a ruler along the top sets the text width (double-click restores it).
- Maximize the bottom panel with Ctrl+Shift+J.
- **Layout buttons** in the title bar, as in VS Code: show or hide the explorer, the bottom panel
  and the agent panel.
- Find in Files shows its progress (files searched of files to search); a new search or a
  clear stops the one still running.
- The mouse wheel scrolls the ribbon and its tab row sideways when they do not fit.
- The version of a build: the title bar says `v1.1.0`, or `v1.1.0-dev` for a build that is not
  the release; About shows the commit, with a button to copy it for a bug report. The Windows
  executable carries its version (Properties > Details).

### Changed
- Toggle line comment is Ctrl+/ (was Ctrl+Q).
- The workspace switch in the title bar is one icon: a click goes on to the next workspace, the
  chevron beside it lists them all.

### Fixed
- Windows: memory no longer grows while the app sits idle behind other windows, or when the
  same tabs are opened and closed again and again. The OpenGL driver kept memory for frames
  drawn to a hidden window, for every thread started and for every font atlas rebuild.
- Git no longer runs every three seconds for as long as the app runs once a git view was shown.
- Rendered Markdown: bold text has one weight; numbers in right-aligned table cells stay on one
  line.
- Rendered Markdown code blocks are a grid of cells, as in a terminal: box drawing (`┌─┐ │ ╔═╗`)
  and block elements (`█▓▒░`) are drawn to fill their cells, so borders are as wide as the text
  and lines join between rows; wide characters and emoji take two cells, tabs stop at multiples
  of 4, and a long line wraps at a whole character (it used to break characters apart).
- Rendered Markdown math on several lines: `\\` starts a new line and `&` aligns columns, and
  `aligned`, `align`, `cases`, `matrix`, `pmatrix`, `bmatrix`, `vmatrix`, `array` and `gather`
  are laid out with their delimiters drawn to full height (they used to run together on one
  line). A display formula too long for the page breaks before an operator instead of shrinking;
  `$$...$$` within a paragraph is set apart on its own lines; `\binom` is drawn.
- The menus of the ribbon and the title bar have room around their items.
- Editor: the caret no longer drifts from the text on long lines; suggestions keep their order.
- Memory goes back to the system when a folder closes, on Linux, macOS and Windows; every byte
  of the C++ heap is accounted to the part of the app that holds it.

## [1.0.0] - 2026-11-01

The first release.
