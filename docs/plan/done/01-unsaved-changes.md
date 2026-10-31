# 01 · Unsaved changes are never lost

**Problem.** Closing a tab (×, middle click, Ctrl+W) or the window drops unsaved edits without a word.
Bulk closes already keep modified documents open.

**Design.**
- View-model: `requestCloseDocument(i)` closes saved documents at once; for a modified one it raises
  `confirmClose` with the document instead. `resolveClose(Save | Discard | Cancel)` finishes it.
- Quitting (window close button, Exit, Alt+F4) goes through `requestQuit()`: with unsaved documents it
  raises a confirmation listing them: *Save all and quit*, *Quit without saving*, *Cancel*.
- An untitled document saved from the prompt goes through Save As (item 02); until then the prompt
  offers Discard / Cancel only for it.
- The GLFW window's close button is intercepted (`glfwSetWindowShouldClose` reset, then the prompt).
- Agents: the prompts are ordinary dialogs registered for automation (`dialog:save-changes:*`).

**Tests.** Unit: closing a modified tab asks; Save writes and closes; Discard closes; Cancel keeps it;
quit with unsaved documents asks; quit with none quits. UI sweep: both dialogs open.
