# Changelog

What changed in each release of EasyNotepad. The version is in the `VERSION` file; see
[docs/RELEASING.md](docs/RELEASING.md) for how a release is made. The section of a release is
its release notes on GitHub.

## [1.2.0] - unreleased

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

### Changed
- Hints are a rounded, semi-transparent box without a border; a long hint wraps (about 55
  characters a line). The minimize, maximize and close buttons have no hint.

### Fixed
- Android: files and folders outside the app's own (Download, Documents, the whole shared
  storage, SD cards) could not be browsed or opened. The app now asks for "All files access"
  (Android 11 and later; the storage permission before): the Open and Open Folder dialogs say
  what is missing and open the system's page for it, then list the files when the user is back.
  Home is the shared storage, and its folders and SD cards are places of the dialog.
- Hints opened from the ribbon (and other places without padding) had their text touching the
  edges of a box too small for it.

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
