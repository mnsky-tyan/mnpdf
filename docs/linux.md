# The Linux port

The Windows reader (`src/main.cpp`, 6310 lines) is one Win32 file: GDI painting,
Win32 windowing, WinHTTP update checks and PDFium calls, all intertwined. This
port keeps what is genuinely reusable and replaces what is not.

## What was kept, and how

The document engine - the `Doc` bundle, the mark and undo model, the page
geometry, hit-testing, the sidecar, save/bake, page surgery - is about half the
Windows file and is portable C++ that touches PDFium and plain stdlib only. It
is ported essentially unchanged into `src/linux/doc.cpp` and friends, so the two
readers behave the same on the same input.

The seam is `src/linux/ui.h`. On Windows those calls are inlined GDI calls made
straight from the "pure" functions (`renderPage()` writes into an `HDC`,
`messageBox()` takes an `HWND`). On Linux the core calls `ui_message()` and
`ui_save_file()` and never names a widget; `ui.h` names, for every seam member,
whether the core or the GTK layer calls it. That is also what makes the
headless test possible: `tests/core-test.cpp` supplies stubs and drives the
engine with no display at all.

### The scroll model is deliberate, not a shortcut

The Windows app scrolls the document itself (`gScrollX`/`gScrollY`) and paints a
back buffer. GtkScrolledWindow would have been the idiomatic GTK choice, but
every piece of geometry and hit-testing code - `yTopPx`, `docHpx`, `pageAtDocY`,
`charIndexAtDoc`, `clampScroll` - is expressed in those coordinates and is the
code the Windows suites test. Keeping the model means the ported logic is the
same logic, so the port is reviewable and the geometry cannot silently drift.
GTK supplies the window, the menus, the dialogs and the tab strip; the canvas
paints the composited frame and draws its own scrollbars.

### What is not the same

- **Tab strip.** GtkNotebook draws it, so the canvas reserves no band for it.
  `tabStripH()` returns 0 and the ported code still calls it.
- **Custom titlebar.** Windows hides its caption and then re-implements
  `WM_NCCALCSIZE` and `WM_NCHITTEST`, because removing the caption there also
  removes the frame's own drag and resize behaviour. GTK keeps the window
  manager in charge of all of that, so the port uses
  `gtk_window_set_decorated` and needs no hit-testing code: the window still
  resizes, snaps and maximises, and only the caption strip goes. The state is
  the Windows `titlebar=` key in app.txt, so a shared app.txt hides the
  titlebar on both platforms.
- **Note editor.** Windows floats a popup editor beside the pin; the Linux build
  uses a modal dialog. The text, the length limit and the undo op are the same.
  An open editor is settled at a different moment: Windows commits an open note
  box at a tab switch and at window close (`settleActiveDoc`, src/main.cpp:5279,
  and the WM_CLOSE handler), while the Linux dialog is modal, so it must be
  saved or cancelled before the switch can happen at all. Cancelling a NEW pin
  drops it on both platforms.
- **Window rect while maximized.** On Windows the saved `app.txt` rect is
  `rcNormalPosition`, the pre-maximize frame; on Linux the configure handler
  records the live allocation, so quitting while maximized writes the
  maximized size. Relaunching maximized is correct on both; only the size the
  window returns to on un-maximize differs.
- **Units of the saved window size.** The same `winw`/`winh` keys mean a
  different rectangle on the two platforms: Windows stores the whole frame from
  `GetWindowPlacement` (caption and resize border included), Linux stores the
  client size the window manager handed GTK. Each platform round-trips its own
  value, so the file stays readable on both; a window sized on one just appears
  a frame's worth larger or smaller on the other.
- **The update check.** It is the same feature, but it cannot follow the same
  route. The Windows app reads `/releases/latest` - the newest
  NON-prerelease - and fetches `mnpdf-win-x64-<tag>.zip` by name; a Linux-only
  release is a prerelease until the Windows archive is published beside it, and
  that endpoint names the Windows asset. The port asks the releases API for the
  newest release that actually carries `mnpdf-linux-x64-<tag>.tar.gz` and
  compares versions with the Windows rules. See "The parity work" above for
  the asset that publishes that name. There is no Linux install step yet: the
  check reports what is available and hands the reader the release page.
- **Tabs are GtkNotebook pages**, and each tab's own document, zoom, scroll and
  marks live in its `Doc` bundle exactly as on Windows.

## The file formats are the contract

`src/linux/sidecar.cpp` writes the same bytes the Windows app writes:
`fmt=`, `zoom=`, `fit=`, `page=`, `hl=`, `rot=`, `dl=`, `pin=`, `sig=` and the
same `app.txt` keys. The escape tables match (`\n` in pin text, `\c` in stamp
paths). The sidecar filename is the same FNV-1a hash of the UTF-16 path bytes,
so even a shared config directory resolves to the same file on both platforms.

One divergence is deliberate and recorded rather than hidden: after an undone
pin deletion the Linux engine puts the pin back in its original slot, where the
Windows engine appends it back, so the `pin=` lines of the same edit history
can be written in a different order on the two platforms. Every line carries
its own page and position, so both readers parse either file and lift the same
marks.

A quirk both engines inherit unchanged from the Windows original: undoing the
deletion of a baked mark puts the mark back in memory but never removes its
`dl=` tombstone, so a sidecar written in that state carries both the restored
mark and the tombstone, and the tombstone is applied last on reload - the mark
is gone again after a restart.

`app.txt` is one shared file, so a Linux write emits its own keys in the
Windows canonical order and carries every key it does not own (the Windows
reader's `titlebar=`, `updcheck=` and `updtag=`) after them, verbatim and in
their original order: a launch on one platform never loses the other's
preferences or its update-check clock.

`tests/mnpdf-baked.pdf` is a PDF carrying mnpdf's own baked annotations
(`/Source (mnpdf 0 45)` on a Highlight, `/Source (mnpdf)` on a Text note) - the
exact shape `doSave()` writes on Windows. The test opens it and asserts the
marks lift back into live, undoable marks on Linux.

## PDFium

`third_party/pdfium-linux/` holds the Linux build of PDFium (headers,
`libpdfium.so`, BSD-3-Clause license), the counterpart of the Windows
`third_party/pdfium/`. The two vendored builds are not the same version -
Windows ships 155.0.8057.0, Linux 157.0.8100.0 - so an edge-case document can
render a little differently on the two platforms; the port uses only long-stable
PDFium APIs, so nothing here requires the newer build. `build-linux.sh` renders
`tests/arc.pdf` with it before compiling the app, so a broken vendored library
fails at the real cause instead of at a linker error three hundred lines away.

PDFium's string API is UTF-16 (`unsigned short`), while `wchar_t` is 32-bit on
Linux. The port converts explicitly at every boundary (`wideToU16`/`u16ToWide`)
rather than casting: an early draft cast a `std::wstring*` straight to
`FPDF_WIDESTRING` and PDFium read the first half of every character, so a search
for "Hello" silently became a search for "H".

## Tests

Two layers, both run by `scripts/linux-gate.sh`, mirroring how the Windows suites
drive the real binary:

- `tests/core-test.cpp` links the platform-independent modules and exercises the
  engine with stubbed UI. It is a real CMake target (`mnpdf-core-test`,
  registered with `add_test`), built by `cmake --build`, run by
  `build-linux.sh` (a core regression fails the build) and by the gate's first
  layer. It covers open, geometry, selection, search, marks, undo/redo, the
  sidecar round trip, save/bake and re-read, page surgery, merge, tabs, prefs
  (including the `app.txt` keys only the Windows reader owns), the JPEG guard and
  the format interop.
- `scripts/linux-gate.sh`'s second layer runs the real binary on Xvfb and asserts
  on the pixels it paints (the page is mostly paper and carries text), on the
  state files it writes, and on the quit path logging no `Gtk-CRITICAL`.

The app exposes the same kind of seam the Windows suites use: `MNPDF_SHOT`
writes its back buffer to a PNG after the first render, and `MNPDF_SHOT_EXIT`
quits after, so a pixel assertion needs no harness clicking a window.

## Resource cost

Measured on this machine (`/usr/bin/time`, and RSS sampled from `/proc`):

- Headless core test, 154 checks including two full save/bake round trips:
  **0.26 s wall, 15.1 MB peak RSS** (measured, `/usr/bin/time -v`).
- The GUI with a 13-page document open, idle after 20 s: **98.6 MB RSS, steady
  state, no growth** (measured, sampled from `/proc/<pid>/status`). Of that,
  the vendored `libpdfium.so` file itself is 7.9 MB (measured, `ls`); how much of
  it is resident is an estimate, and GTK accounts for the rest. The only
  allocation that scales with the window is one client-sized BGRA buffer, the same
  discipline as the Windows build, and the print path allocates exactly one page
  raster at a time, sized from the page's own box at 300 dpi and shrunk to the
  same 16-megapixel budget the Windows print path applies.

## What is still missing

The Windows build's print dialog is replaced by the GtkPrintOperation dialog.
The update check differs in one deliberate way: the Windows app reads
GitHub's `/releases/latest` and downloads `mnpdf-win-x64-<tag>.zip` by name,
while this one asks the releases API for the newest release that actually
carries a Linux archive - see "The update check" below.

## The parity work (2026-10-21)

The port now carries the same command set as the Windows build, under the same
menu ids, so the two readers can be driven the same way.

**Advanced.** Windows keeps everything past plain reading in an "Advanced"
cascade (src/main.cpp:5449). The port has the same cascade in the menu bar and
in the right-click menu: Thumbnails, Outline panel, Night mode, Merge PDFs,
Split pages, Insert signature, Clear signatures, Reopen last document.

**The titlebar.** Windows clears `WS_CAPTION` and then re-implements
`WM_NCCALCSIZE` and `WM_NCHITTEST` (src/main.cpp:5825, 6014), because
removing the caption there also removes the frame's own drag and resize
behaviour. GTK keeps the window manager in charge of all of that, so the port
uses `gtk_window_set_decorated` and nothing else: the window still resizes,
snaps and maximises, and only the caption strip goes. The state is the Windows
`titlebar=` key in app.txt, written in the Windows field order, so a shared
app.txt means the same thing on both sides.

**The drawers.** The thumbnails and outline panels are side drawers beside the
reader, the same place Windows puts them (src/main.cpp:4886 placeDrawer, side 0
for the thumbnails and side 1 for the outline). The content is the same code
path: a thumbnail is a page rasterized through `renderPageToBuffer`, and the
outline is the bookmark tree collected with `FPDFBookmark_GetFirstChild`, with
the same nesting and the same "This document has no bookmarks." fallback.

One GTK detail worth recording, because it is invisible until the drawer is
missing: a widget packed into a box that has already negotiated its size keeps
a 1-px allocation until BOTH the strip and the window are told to lay
themselves out again and the request is then drained. Without that, the
drawer is created, visible, and invisible on screen. Measured with a GtkBox
probe of the app's own shape (menu bar, strip, overlay).

**The colours.** Windows offers a colour submenu wherever a colour is chosen -
a mark, a default, a pin - with the six presets, the reader's own slots, a way
to hand the slots back and a way to type a new one. The port has the same
submenus, with the same id scheme (presets at 120/140/150/160, custom slots at
126/146/156/166), so the same command carries the same number on both
platforms.

**The ids.** The whole command space was realigned with the Windows one: 118
is Save, 119 is Save As, 114 is Undo, 115 is Redo, 113 is the titlebar toggle,
133 and 134 are the rotations, 137 is Print, 170 is the update check, 180
clears the custom colours. The entries Windows has no menu for - Delete this
page, Move page up, Move page down - moved into a free range (310-312).

**The update check.** This is the one place the port deliberately does not
follow the Windows route. The Windows app reads `/releases/latest`, which
answers with the newest NON-prerelease, and then fetches
`mnpdf-win-x64-<tag>.zip` by name. A Linux-only release is a prerelease until
the Windows archive is published beside it, and the asset that endpoint names
is the Windows one. So the port asks the releases API for the list and picks
the newest release that actually carries
`mnpdf-linux-x64-<tag>.tar.gz`, then compares versions with the Windows rules
(src/main.cpp:2334). The request runs on its own thread and is bounded by a
timeout, so a slow network cannot freeze the window.

The version the port reports is generated at build time from the Windows
`kAppVersion` constant (CMakeLists.txt reads src/main.cpp), so the two readers
can never claim different versions - which matters, because they share one
app.txt.

## The test seams

`MNPDF_CMD` drives one command by its menu id, the way the Windows suites post
WM_COMMAND, so a suite can assert on chrome the page back buffer never holds.
`MNPDF_SHOT_WINDOW` captures the whole window instead of only the page buffer.
`MNPDF_MENU_DUMP` prints the menu tree as text, each entry as `label: id`, which
is how the parity comparison is made without clicking anything: the dump is
asserted against the Windows command table in src/main.cpp, so a missing or
reworded command fails the gate.

`MNPDF_SHOT_EXIT` is a fourth seam, and it is a window-level one rather than a
fourth independent thing: it appends the quit command to whatever else the run
asked for. A run that asks only for a quit therefore quits, and a run that asks
for a command and a quit drives the command and then quits.

## Releasing

`.github/workflows/release.yml` builds both platforms from a `v*` tag and
publishes them as one release. The Windows asset is not optional even for a
Linux release: the Windows app discovers updates through GitHub's
`/releases/latest` and then fetches `mnpdf-win-x64-<tag>.zip` by name
(`src/main.cpp:2572`), so a release carrying only a Linux archive would tell
every installed Windows copy that a newer version exists and then fail to
download it. Both assets are therefore asserted to exist before the release is
created.

Cutting one:

```sh
# the tag, kAppVersion and README.txt must agree or the contract job fails, so
# bump both: kAppVersion in src/main.cpp and the first line of README.txt
git commit -am "release: v2.7.0" && git push origin main
git tag v2.7.0 && git push origin v2.7.0
```

The Linux asset is a plain tarball, and `scripts/make-release-tarball.sh`
produces exactly the layout that is published: the binary, the vendored
`libpdfium.so` (found through an `$ORIGIN` rpath), `README.txt`,
`PDFIUM-LICENSE.txt` and `README-linux.txt`, named
`mnpdf-linux-x64-<tag>.tar.gz`. It needs no install step and no root; the
system dependencies are GTK3, which every desktop already has, and libcurl
(`libcurl4-openssl-dev` to build, `libcurl.so.4` at runtime) for the update
check. `ldd` on the unpacked binary resolves `libpdfium.so` from beside it, so
there is no `LD_LIBRARY_PATH` to set.
