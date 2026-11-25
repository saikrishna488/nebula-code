# Nebula Code

Nebula Code is an offline-first Qt Widgets editor for Intel macOS. Each window contains a folder explorer, tabbed Scintilla editors, and an optional interactive terminal. A split shares its document and undo history with the original view. Each view keeps its own cursor and scroll position.

## Build and run

Install the Apple Command Line Tools, CMake 3.25 or newer, Ninja, and Qt 6 with Widgets, Core5Compat, and Test. The reference build uses macOS 15.8.3 x86_64, SDK 15.5, Apple Clang 17, Qt 6.12.0, CMake 4.5.0, and Ninja 1.13.2.

```sh
cmake --preset release
cmake --build --preset release --parallel 4
ctest --preset release
open build/release/NebulaCode.app
```

Open a specific workspace or file directly from a shell.

```sh
build/release/NebulaCode.app/Contents/MacOS/NebulaCode /path/to/project
```

The first configure downloads Scintilla 5.6.9, Lexilla 5.5.5, and QTermWidget 2.4.0. To build offline, supply copies of the pinned upstream archives. CMake checks their SHA-256 digests.

```sh
cmake --preset release \
  -DNEBULA_SCINTILLA_URL=/absolute/path/scintilla569.tgz \
  -DNEBULA_LEXILLA_URL=/absolute/path/lexilla555.tgz \
  -DNEBULA_QTERMWIDGET_URL=/absolute/path/qtermwidget-2.4.0.tar.xz
cmake --build --preset release --parallel 4
```

## Package for Intel macOS

Use Python 3.11 or newer and the Homebrew Qt toolchain from the Release build. Put the three pinned editor dependency archives in `build/vendor`. The packager also reads the corresponding Homebrew source archives and patches from the Homebrew download cache and `build/vendor`.

```sh
python3 scripts/package-macos.py
open build/dist/NebulaCode.app
```

`build/dist` contains the ad hoc signed app, its ZIP, and the corresponding source archive. To install from the ZIP, unzip it, move `NebulaCode.app` into Applications, and open it on an Intel Mac. The app includes dependency notices in `Contents/Resources/Notices`. The source archive includes Nebula's build files, pinned upstream archives, and the installed Homebrew recipes and patches.

## Editing

Use **File → Open Folder** or **Open Recent Folder** to switch workspaces in the current window. Nebula prompts for unsaved edits and restores the selected folder’s tabs and splits. Use the left activity bar to switch between Explorer and Source Control, find text in the current document, open the terminal, or open Settings. The **View** menu controls the split, minimap, terminal, theme, and source control panel. Search commands by name with **⌘⇧P**. **⌘D** selects the next occurrence for simultaneous editing. **⌘F** opens find and replace.

Files use UTF-8. Saving preserves existing BOMs and line endings, checks for changes since the file was loaded, and atomically replaces the target. **File → Reload from Disk** reads the latest contents. Closing a modified tab offers to save or discard its edits. Closing a window records its file tabs, active document, split sizes, and cursor positions for the next session.

Open or save Markdown documents with the `.md` or `.markdown` extension for highlighting of headings, emphasis, links, lists, quotes, and code.

The terminal opens at the bottom in the workspace directory. Drag its upper edge to resize. Drag its title to move or detach it, or use the title-bar menu to choose a dock position. Hiding the panel keeps the session alive. Closing the panel or exiting its shell ends the session. The terminal retains up to 5,000 lines of history.

## C/C++ and Git

Install clangd through the Apple Command Line Tools or set the path to another local clangd executable in the settings. Opening a supported C/C++ file in a workspace starts the language server on demand. Generate a compilation database for accurate project-specific results.

```sh
cmake -S . -B build -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

Place or link the resulting `compile_commands.json` at the project root, or configure its directory through the project's `.clangd`. Use the **Tools** menu for completion, definition, references, and language-server restart. **View → Toggle Language Results** opens diagnostics and navigation results. Files over 2 MiB skip clangd synchronization at open. Files over 8 MiB also skip syntax highlighting and folding at load.

The source control panel uses the local `git` executable. Open a repository folder, select a path, and choose its staged or unstaged diff. Stage or unstage the selected path, enter a message, and commit. Configure your Git author name and email before the first commit. Branch and remote operations are available through the terminal.

## Local preferences

Open the gear in the lower-left corner or press **⌘,** for Appearance Settings. Choose Light or Dark and adjust sidebar and editor colors, editor font and size, and minimap visibility. Save applies the changes to open windows and keeps them for the next launch. New configurations start in Light.

Nebula uses Qt's application configuration directory. Set `NEBULA_CONFIG_DIR` to choose an explicit directory. Settings, keybindings, named themes, and saved sessions are versioned JSON files. Restart the editor after editing settings and keyboard overrides.

**settings.json**

```json
{
  "version": 1,
  "theme": "dark",
  "minimap": true,
  "shell": "/bin/zsh",
  "clangd": "/usr/bin/clangd"
}
```

**keybindings.json**

```json
{
  "version": 1,
  "bindings": {
    "view.split": "Ctrl+Alt+S",
    "language.references": "Shift+F12"
  }
}
```

Binding strings use Qt portable notation. On macOS, `Ctrl` in this notation maps to Command. An empty string clears a command's shortcut. Command IDs are registered in [Workbench.cpp](src/app/Workbench.cpp).

To customize a theme, copy [dark.json](resources/themes/dark.json) or [light.json](resources/themes/light.json) into the configuration directory's `themes` folder under a new name. Edit the colors, font family, and point size, then select the filename in **View → Choose Theme**.

## Validation

The Release suite covers the editor, workbench and session restore, terminal PTY, local Git, and LSP. The LSP tests use both a fake server and a real clangd CMake fixture. Git and clangd must be installed for the corresponding integration checks to run. A smaller build can run workspace and settings tests with Qt Widgets and Test alone.

```sh
cmake --preset workspace-tests
cmake --build --preset workspace-tests
ctest --preset workspace-tests
```

Run `python3 scripts/benchmark.py` after a Release build to measure fresh launches, RSS, idle CPU, file opening, and key-to-paint latency on the current Mac.

## License

Nebula Code is distributed under the GNU General Public License version 3. The dependency versions and archive hashes are in [Editor.cmake](cmake/Editor.cmake) and [Terminal.cmake](cmake/Terminal.cmake). Each library retains its upstream license. See [LICENSE](LICENSE).
