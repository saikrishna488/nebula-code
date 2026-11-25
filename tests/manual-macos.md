# Native macOS editor acceptance

The Release x86_64 app ran in the native Cocoa desktop on macOS 15.8.3 with Qt 6.12.0, Scintilla 5.6.9, and Lexilla 5.5.5. The fixture was a temporary workspace containing one C++ file with a UTF-8 BOM, nine CRLF line endings, and café and 漢字 text, plus a nested plain-text file. Checks used the macOS menus, keyboard, file dialog, and editor window.

| Check | Observed result |
|---|---|
| Native editor launch and painting | Opened the Intel Release app and the C++ fixture through the native Open File dialog. Line numbers, indentation, syntax colors, glyphs, and minimap were visible. |
| Split and keyboard focus | Command–backslash opened a second pane. Clicking its text moved focus to that pane and updated the status bar. Folding the left pane left the right pane expanded. |
| Multiple selections and undo | Three Command–D presses selected three occurrences of the same identifier in the right pane. One key replaced all three in both panes. One Command–Z restored all three and cleared the dirty indicator. |
| Unicode and line-ending preservation | Pasted `Ω日本語` into the right pane and saw it in the left pane. Command–S saved the paste. A byte-level check found the original UTF-8 BOM, nine CRLF pairs, zero bare LF characters, and the inserted UTF-8 bytes. |
| Theme and command palette | Command–Shift–P found and opened Choose Theme. Both Dark and Light showed legible syntax, line numbers, and the active caret. Light mode changed the two panes and their minimaps together. |
| Find and replace | Command–F focused Find. Return selected a match in the active pane. Replace All updated three occurrences in both panes. |
| External edit protection | Modified the temporary file outside Nebula with unsaved local replacements. Save prompted for overwrite. Cancel kept the disk edit, which was visible when opening the file from Explorer. |
| Explorer | Opening the fixture folder created a new window with Explorer. Opening the C++ file through Explorer displayed its current disk contents. |

The first native run showed a tall minimap viewport highlight on empty and short documents. The editor now hides this highlight for empty buffers and clamps the short-document viewport to the document's line count. An automated raster test checks the short-document case.

Native session restoration remains unverified. During the initial desktop pass, the process exited when the automation queried the macOS accessibility hierarchy immediately after opening the second fixture file. The system crash report records a SIGSEGV in Qt Cocoa's `QMacAccessibilityElement accessibilitySelectedChildren`. This is an outstanding accessibility stability issue for that sequence.

The desktop capture showed sharp raster text at the current display scale. The native backing scale could not be read from the managed shell. The syntax, folding, and minimap tests passed offscreen with `QT_SCALE_FACTOR=2`. A Retina or fractional-scale display and live IME composition, candidate selection, and commit remain to be verified. The Unicode check used the clipboard.
