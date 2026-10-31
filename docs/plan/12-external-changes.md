# 12 · Files changed on disk

**Problem.** When another program (git, a formatter, an agent) changes an open file, the editor keeps
showing and later overwrites the old text.

**Design.** `os::fs::modificationTime(path)`; each document remembers the time and size it was
loaded or saved with. A poll (every 2 s, only when the window has focus or the app is idle) checks
open documents: an unmodified document reloads silently (caret line kept); a modified one shows a
bar "Changed on disk: Reload / Keep mine / Compare". A deleted file is marked "deleted on disk".
Saving over a file changed since loading asks first.

**Tests.** Unit (temp folder): external write reloads an unmodified document; a modified one raises
the conflict; Compare shows the diff; save over a changed file asks.
