# mnpdf

[![ci](https://github.com/mnsky-tyan/mnpdf/actions/workflows/ci.yml/badge.svg)](https://github.com/mnsky-tyan/mnpdf/actions/workflows/ci.yml)

A small, fast, native PDF reader for Windows. One executable plus PDFium, no
installer, no runtime, no framework.

It reads, searches and marks up PDFs without leaving marks on the page: text
you select stays selectable, highlights and pins can be undone and re-done
individually, and the file you opened is never rewritten unless you ask.

## Download

Portable ZIP: the [**latest release**](https://github.com/mnsky-tyan/mnpdf/releases/latest)
(`mnpdf.exe`, `pdfium.dll`, `README.txt`, `PDFIUM-LICENSE.txt`).

Extract it anywhere and run `mnpdf.exe`. Keep `pdfium.dll` next to the
executable - the two are a matched pair. There is no installer, nothing is
registered, and the folder can be copied to another machine or a USB stick
as-is.

The app tells you when a newer release exists (at most one check per hour),
and the check button then becomes an update button: it downloads the official
ZIP, replaces `mnpdf.exe` and `pdfium.dll` in the folder the app runs from and
restarts. An unsaved document blocks the swap until you save it, and a failed
download changes nothing. Extracting the new ZIP over the old folder by hand
works exactly as before.

## What it does

**Reading.** Pages scroll continuously in a single column. Zoom with the
mouse wheel while holding Ctrl, or `+` / `-`, or fit the page width with
`Ctrl+0`. The scroll bars move in both directions.

**Text.** Select with the mouse and copy with `Ctrl+C`. `Ctrl+F` searches,
`F3` and `Shift+F3` step forward and backward through matches, `Esc` closes
the search bar.

**Marks.** Right-click to highlight selected text or to add a pin with a
note. Highlights and pins share one palette of six presets plus up to three
custom `#rrggbb` colours you enter yourself: the entry box opens with the `#`
in place, an entry that is not a colour is explained straight away, and Clear
custom colours hands the slots back. Everything is individually
undoable (`Ctrl+Z` / `Ctrl+Y`) and redoable, and delete is an undoable
operation rather than a loss of information.

**Pages.** Rotate the current page clockwise or counter-clockwise from the
right-click menu; the rotation persists with the document.

**Advanced.** The *Advanced* submenu in the right-click menu holds everything
past plain reading. The tools that shipped live there, in two groups.

*Page drawer.* *Thumbnails* opens a strip of page previews beside the reader.
Click one to jump to that page, `Ctrl+Up` / `Ctrl+Down` to move the selected
page, `Delete` to remove it, and `R` to rotate it. *Outline panel* opens the
document's own bookmarks, indented, and a click goes to the page it names.

*Dark reading.* *Night mode* inverts the page for a dark screen and is
remembered for the next start.

*Rearranging and combining.* *Merge PDFs* adds other files to the end of the
document you are reading. *Split pages* exports a range - `2-5` or `1,3,8` -
into a new PDF without touching the original.

*Signing.* *Insert signature* takes a JPEG and asks where to put it; the
next click on a page drops the stamp there. A file that is not a JPEG is
refused rather than decoded. *Clear signatures* takes them all
away again. A stamp lives in the file once you save, and in the
sidecar until then, exactly like a highlight.

*Coming back.* *Reopen last document* opens the document the current one
replaced - the one you had a moment ago - so opening the wrong file by
mistake costs one click to undo. It spans this session only.

Below those, the same submenu lists the heavy options still to come - OCR,
Word export, in-place text editing, form creation, encryption, side-by-side
tabs. They are listed and greyed: they need a different kind of program, and
this one stays a reader you can copy onto a USB stick.

**Printing.** `Ctrl+P` opens the standard Windows print dialog and prints
clean pages. Annotations, highlights and pins stay in the file and on screen
and are simply not printed.

**Tabs.** One window can hold several documents at once. `Ctrl+T` opens a new
tab, `Ctrl+W` closes the current one, `Ctrl+Tab` cycles to the next, and
clicking a tab in the strip along the top of the window switches to it - the
`+` at the right end opens one too. Every tab carries its own document, page,
zoom, marks and undo history, so switching away and back leaves everything
exactly where it was. A document you open fills an empty tab, or replaces what
the current tab is showing, and closing the last tab quits like closing the
window. The strip shows each document's name, with a dot in front while that
tab has unsaved changes.

**Several windows.** Run `mnpdf.exe` twice and you get two independent
windows, each reading its own document, so two PDFs can sit side by side. The
window that started first owns the remembered settings and the document a
plain launch reopens; later windows read those shared settings but start empty
and write none of them. A plain second launch starts empty rather than
reopening what the first window already has.

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
| `Ctrl+T`, `Ctrl+W`, `Ctrl+Tab` | new tab, close tab, next tab |
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
| `app.txt` | titlebar, autosave, default highlight and pin colours, custom palette, night mode, window geometry (`winx`, `winy`, `winw`, `winh`, `winmax`), last update check (`updcheck`) and the newest tag it found (`updtag`) - the tag the update button offers and the one an install fetches |
| `doc-<hash>.txt` | per-document sidecar: zoom, fit, current page, highlights, page rotations, pins, signature stamps, deleted marks |
| `last.txt` | the document that was open when you quit |

With **autosave** on (the default) edits are written to the sidecar and the
PDF on disk is left untouched. `Ctrl+S` is the point where the marks move
*into* the file: the highlights, pins, signature stamps and rotations are
written into the PDF and the file is reloaded from what was saved. Undo
therefore works on your session's edits, and the sidecar keeps them
recoverable while autosave is on.

## Building

Windows, MSVC and PDFium. `build.bat` compiles `src/main.cpp` against
`third_party/pdfium` (headers, the import library and the runtime DLL) and
links the icon from `mnpdf.rc`:

```
build.bat
```

It needs the Visual Studio developer command prompt paths, which the script
sets up itself with `VsDevCmd.bat -arch=x64`. Output lands in `build/`.

## Tests

Nine PowerShell suites drive the real application: they launch the built
binary, send it the same messages a reader's mouse and keyboard generate, and
assert on the app's own state files, its window, its frame, the pages it
renders (as per-page raster checksums, so "that page moved" is a fact rather
than a guess) and, for a custom colour and for night mode, the pixels it
paints.

```
bash scripts/gate-test.sh     # from WSL: builds, then runs all nine suites
```

Each suite gets its own `APPDATA` and `TEMP`, so nothing written by one can
reach the next. Thirteen environment variables are the seams of this setup -
seven read by the app, seven set by the runner plumbing (`MNPDF_WINDOW_DESKTOP`
is both, so the table counts it once) - and none of them does
anything unless it is set:

| | |
|---|---|
| `MNPDF_BACKGROUND` | app: start minimised, without stealing the foreground - every suite but `test-colors.ps1`. The app only checks that the variable exists, so a foreground suite needs it *removed*, not set to `0` |
| `MNPDF_VERBOSE` | app: put the document name and page in the window title |
| `MNPDF_HOOK` | app: at launch, run one page-editing step (`merge\|<pdf>`, `sig\|<jpeg>\|<page>`, `split\|<range>\|<out.pdf>`, `delpage\|<n>`, `movepage\|<n>\|<m>`, `rotatepage\|<n>\|<+1\|-1>`, `open\|<pdf>`, `clearsigs`, `pagehash\|<file>`, `night\|<0\|1>`; `;;` chains steps) instead of waiting for a modal file dialog a posted-message harness cannot click |
| `MNPDF_PRINT_PROBE` | app: at print time, write raster statistics to the given file instead of opening the modal dialog |
| `MNPDF_UPDATE_ZIP` | app: the update payload as a local ZIP path (or URL) instead of the GitHub download, so an install can be driven with no network |
| `MNPDF_UPDATE_DIR` | app: the folder an update swaps into, instead of the folder the running executable is in - the seam that points an install at a scratch copy |
| `MNPDF_GATE_EXE` | runner: the binary the suites drive, so a suite never hardcodes a path |
| `MNPDF_GATE_STATE` | runner: the suite's private state directory (`APPDATA`/`TEMP` live under it) |
| `MNPDF_GATE_SUITE` | runner: which suite script to run |
| `MNPDF_SKIP_SUITES` | runner: a comma-separated list of suites to drop from the roster for this run, for a suite that is environmentally broken on one machine |
| `MNPDF_FORCE_BACKGROUND` | runner: present = start every suite's app minimised, the foreground one included, so nothing appears on the desktop someone is using |
| `MNPDF_WINDOW_DESKTOP` | app and runner: the 0-based virtual desktop every app window is moved to before any restore or measurement, so a local gate keeps its windows off the desk in use - the app reads it too, to start without ever taking the foreground |
| `MNPDF_VD_DLL` | runner: the `VirtualDesktopAccessor.dll` path the move-to-desktop above needs |

The WSL runner forwards its four runner seams (`MNPDF_GATE_EXE`,
`MNPDF_GATE_STATE`, `MNPDF_GATE_SUITE`, `MNPDF_BACKGROUND`) through `WSLENV`
(`/w`, Windows-side only), which is what lets `gate-test.sh` hand each suite
its paths without quoting a Windows path into a command line.

Every suite but `test-colors.ps1` keeps its window minimised: the colour suite
really types into the app's colour box, and a minimised owner can never give
that popup the keyboard.

On a plain Windows machine - and in CI - there is no `wslpath`, so
`scripts/test-ci.ps1` is the same build and the same nine suites with no WSL in
the way. Both runners share one roster (`scripts/suites.txt`), one build recipe
and one pass/fail rule (`scripts/gate-common.ps1`), so a suite cannot pass in
WSL and behave differently in CI. It is what `.github/workflows/ci.yml` runs on
a Windows runner, so every push and pull request carries a real check.

```
powershell -ExecutionPolicy Bypass -File scripts\test-ci.ps1
```

`test-release.ps1` on its own is the release contract: the binary's version,
the README's first line, the four files a release ZIP must contain, and the
update flow itself - what the dialog answers, a full install against a scratch
copy, and an update that fails with a reason.

## Repository layout

```
src/main.cpp          the whole application
mnpdf.rc              icon and version resources
resource.h
third_party/pdfium/   PDFium headers, import library, DLL and license
tests/lib.ps1         shared test harness (window lookup, launch, waiting)
test-*.ps1            feature suites (select-msg, features, suite2,
                      continuous, release, captionless, tab, pageedit, colors)
test-release.ps1      release contract: version agreement with README.txt, the
                      ZIP file list, the update dialog, a full self-install
scripts/gate-test.sh  WSL entry point: build and all nine suites
scripts/test-ci.ps1   Windows/CI entry point: the same build and suites
.github/workflows/ci.yml  runs test-ci.ps1 on a Windows runner
```

## Release contract

Two places must agree on the version, and `test-release.ps1` fails if they do
not:

- `kAppVersion` in `src/main.cpp:55`
- the first line of `README.txt` - `mnpdf vX.Y.Z` - which is also the file the
  portable ZIP carries

A release is a tag plus a ZIP built from exactly four files: `mnpdf.exe`,
`pdfium.dll`, `README.txt` and `PDFIUM-LICENSE.txt`. The ZIP is published as
`mnpdf-win-x64-<tag>.zip` - that name is what the updater requests, so it is
part of the same contract.

`test-release.ps1` installs through both update seams (`MNPDF_UPDATE_ZIP` and
`MNPDF_UPDATE_DIR`, in the table above) against a scratch copy, so the whole
flow runs with no network and without the machine's own reader as its subject.

## License

PDFium is used under the BSD-3-Clause license; see
[`PDFIUM-LICENSE.txt`](PDFIUM-LICENSE.txt) in this repository, which
is also shipped inside every release ZIP.
