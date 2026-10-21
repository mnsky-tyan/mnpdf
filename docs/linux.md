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
`messageBox()` takes an `HWND`). On Linux the core calls `ui_invalidate()`,
`ui_message()`, `ui_open_file()` and never names a widget. That is also what
makes the headless test possible: `tests/core-test.cpp` supplies stubs and drives
the engine with no display at all.

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
- **Custom titlebar.** The Windows build can hide its caption and still be
  dragged and resized from the edges; GTK has no equivalent hook, so the Linux
  build always shows the standard frame. Nothing in the document engine depends
  on it.
- **Note editor.** Windows floats a popup editor beside the pin; the Linux build
  uses a modal dialog. The text, the length limit and the undo op are the same.
  An open editor is settled at a different moment: Windows commits an open note
  box at a tab switch and at window close (`settleActiveDoc`, src/main.cpp:5277,
  and the WM_CLOSE handler), while the Linux dialog is modal, so it must be
  saved or cancelled before the switch can happen at all. Cancelling a NEW pin
  drops it on both platforms.
- **Thumbnail and outline drawers.** Not ported yet. The hook
  `refreshDrawers()` exists and is a no-op.
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
- **Update check and install.** Not ported: it is WinHTTP, COM and
  `ShellExecuteW`. The Linux build has no updater.
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
  on the pixels it paints (the page is mostly paper and carries text) and on the
  state files it writes.

The app exposes the same kind of seam the Windows suites use: `MNPDF_SHOT`
writes its back buffer to a PNG after the first render, and `MNPDF_SHOT_EXIT`
quits after, so a pixel assertion needs no harness clicking a window.

## Resource cost

Measured on this machine (`/usr/bin/time`, and RSS sampled from `/proc`):

- Headless core test, 146 checks including two full save/bake round trips:
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

Thumbnail and outline drawers, the update check, and a proper right-click menu
for a mark under the cursor (the context menu currently offers page-level
actions; the pin and highlight entries exist and work). The Windows build's
print dialog is replaced by the GtkPrintOperation dialog.
