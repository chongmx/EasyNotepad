# The app's icons

All drawn from the logo, Font Awesome's solid "pen-to-square" (U+F044, as the title bar shows it) in
the dark theme's accent blue `#3B82F6`, by `tools/icons/make_icons.cpp`. Do not edit them by hand:
change the program and run it again.

| File | Used by |
|---|---|
| `easynotepad-16.png` ... `-512.png` | The logo on transparent. Linux: the window's icon on X11 (`MainGlfw.cpp`) and the installed theme icons (`share/icons/hicolor`); the web's favicon (inline in `web/shell.html`) |
| `easynotepad-tile-512.png` | macOS: the Dock's icon, set at run time (`os/unix/AppIconApple.cpp`) |
| `windows/easynotepad.ico` | Windows: the executable's icon and its windows' (`cmake/EasyNotepad.rc.in`, `GLFW_ICON`) |
| `linux/easynotepad.desktop` | Linux: the menu entry, installed to `share/applications` (Wayland: the windows' icon too, by the app id `easynotepad`) |
| `android/` | Android: a resource folder (`android/app/build.gradle`): the adaptive launcher icon (the logo over `#181B1F`, the title bar's colour; monochrome for themed icons) and, before Android 8, square and round tiles |

The web build leaves this folder out of its data, and the Android build out of its assets.

To draw them again, from the repository's root:

```bash
g++ -std=c++17 -O2 -I3rdparty/imgui -I3rdparty/stb tools/icons/make_icons.cpp -o make_icons && ./make_icons
```

Then update the two inline icons in `web/shell.html` (`base64 -w0 resources/icons/easynotepad-32.png`
and `-128.png`).
