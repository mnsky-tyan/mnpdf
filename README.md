# mnpdf

A small, fast, native PDF reader for Windows. One executable plus PDFium, no
installer, no runtime, no framework.

It reads, searches and marks up PDFs without leaving marks on the page: text
you select stays selectable, highlights and pins can be undone and re-done
individually, and the file you opened is never rewritten unless you ask.

## Download

Portable ZIP: [**mnpdf-win-x64-v2.1.2.zip**](https://github.com/mnsky-tyan/mnpdf/releases/latest)
(`mnpdf.exe`, `pdfium.dll`, `README.txt`, `PDFIUM-LICENSE.txt`).

Extract it anywhere and run `mnpdf.exe`. Keep `pdfium.dll` next to the
executable - the two are a matched pair. There is no installer, nothing is
registered, and the folder can be copied to another machine or a USB stick
as-is.

To move to a newer version, extract the new ZIP over the old folder. The app
tells you when a newer release exists (at most one check per hour) and
explains that updating is a manual, portable step - it never downloads or
overwrites anything itself.

## What it does

**Reading.** Pages scroll continuously in a single column. Zoom with the
mouse wheel while holding Ctrl, or `+` / `-`, or fit the page width with
`Ctrl+0`. The scroll bars move in both directions.

**Text.** Select with the mouse and copy with `Ctrl+C`. `Ctrl+F` searches,
`F3` and `Shift+F3` step forward and backward through matches, `Esc` closes
the search bar.

**Marks.** Right-click to highlight selected text or to add a pin with a
note. Highlights and pins share one palette of six presets plus up to three
custom `#rrggbb` colours you enter yourself. Everything is individually
undoable (`Ctrl+Z` / `Ctrl+Y`) and redoable, and delete is an undoable
operation rather than a loss of information.

**Pages.** Rotate the current page clockwise or counter-clockwise from the
right-click menu; the rotation persists with the document.

**Printing.** `Ctrl+P` opens the standard Windows print dialog and prints
clean pages. Annotations, highlights and pins stay in the file and on screen
and are simply not printed.

**The window.** Hide the titlebar and the caption is gone for good - it does
not come back when another window takes focus. The window can still be moved
by dragging its top edge, and resized from the left, right and bottom edges
and the corners, over a band exactly one window frame thick so a click just
inside the edge resizes and a click just outside reaches the page. Size,
position and maximised state are remembered and restored on the next launch,
including after quitting while minimised.

## Keyboard and mouse

| | |
|---|---|
| `Ctrl+O` | open a document |
| `Ctrl+S` / `Ctrl+Shift+S` | save / save as |
| `Ctrl+P` | print |
| `Ctrl+F`, `F3`, `Shift+F3` | find, next, previous |
| `Esc` | close the search bar, or quit if it is not open |
| `Ctrl+C` | copy selected text |
| `Ctrl+Z`, `Ctrl+Y` (`Shift+Ctrl+Z`) | undo, redo |
| `Ctrl+0` | fit width |
| `+` / `-` | zoom in / out |
| Ctrl + wheel | zoom at the pointer |
| arrows, `PgUp`/`PgDn`, `Space` | scroll, page down/up |
| right click | context menu for the mark under the cursor |

## Where things live

Everything the app remembers is under `%APPDATA%\mnpdf`:

| File | Contents |
|---|---|
| `app.txt` | titlebar, autosave, default highlight and pin colours, custom palette, window geometry (`winx`, `winy`, `winw`, `winh`, `winmax`), last update check |
| `doc-<hash>.txt` | per-document sidecar: zoom, fit, current page, highlights, page rotations, pins, deleted marks |
| `last.txt` | the document that was open when you quit |

With **autosave** on (the default) edits are written to the sidecar and the
PDF on disk is left untouched. `Ctrl+S` is the point where the marks move
*into* the file: the highlights, pins and rotations are written into the PDF
and the file is reloaded from what was saved. Undo therefore works on your
session's edits, and the sidecar keeps them recoverable while autosave is on.

## Building

Windows, MSVC and PDFium. `build.bat` compiles `src/main.cpp` against
`third_party/pdfium` (headers, the import library and the runtime DLL) and
links the icon from `mnpdf.rc`:

```
build.bat
```

It needs the Visual Studio developer command prompt paths, which the script
sets up itself with `VsDevCmd.bat -arch=x64`. Output lands in `build/`.

Tests are PowerShell suites driven by `scripts/gate-test.sh`, which runs each
one against the freshly built binary with its own isolated preferences and
temp directory. Run the release-contract check on its own with
`test-release.ps1`.

## Repository layout

```
src/main.cpp          the whole application
mnpdf.rc              icon and version resources
resource.h
third_party/pdfium/   PDFium headers, import library, DLL and license
tests/lib.ps1         shared test harness (window lookup, launch, waiting)
test-*.ps1            feature suites
test-release.ps1      ZIP contract: version agreement with README.txt, file list
scripts/gate-test.sh  runs the build and every suite
```

## Release contract

Two places must agree on the version, and `test-release.ps1` fails if they do
not:

- `kAppVersion` in `src/main.cpp:49`
- the first line of `README.txt` - `mnpdf vX.Y.Z` - which is also the file the
  portable ZIP carries

A release is a tag plus a ZIP built from exactly four files: `mnpdf.exe`,
`pdfium.dll`, `README.txt` and `PDFIUM-LICENSE.txt`.

## License

PDFium is used under the BSD-3-Clause license; see
[`PDFIUM-LICENSE.txt`](third_party/pdfium/LICENSE) in this repository, which
is also shipped inside every release ZIP.
