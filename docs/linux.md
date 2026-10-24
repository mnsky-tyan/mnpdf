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
- **Custom colour entry.** Windows opens a small popup box beside the reader
  that takes hex digits only; the Linux build asks in a modal dialog. What the
  reader can type is the same: six hex digits, with or without the `#`, and a
  colour that is already in one of the reader's own slots lands in that slot
  instead of taking a new one.
- **Print dialog.** Windows opens the standard Windows print dialog; the Linux
  build opens the GTK one (`GtkPrintOperation`). What is printed is the same:
  each page rendered clean, no annotations, highlights or pins, at the same
  16-megapixel budget the Windows print path applies (see "Resource cost"
  below).
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
- **The update check.** It is the same feature, and it reads the tag the same
  way: `/releases/latest` answers with a redirect to the newest
  NON-prerelease, and the tag is read off that redirect, exactly as the
  Windows build does (src/main.cpp:2466-2479). What differs is the asset.
  The Windows app then fetches `mnpdf-win-x64-<tag>.zip` by name; a Linux-only
  release is a prerelease until the Windows archive is published beside it, and
  that endpoint names the Windows asset. The port reads the release feed for
  the newest release that actually carries `mnpdf-linux-x64-<tag>.tar.gz` and
  compares versions with the Windows rules. See "The parity work" below for
  the asset that publishes that name. There is no Linux install step yet: the
  check reports what is available in a message box, and it has no "open the
  release page" button - the Windows build has one.

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
reader's `updcheck=` and `updtag=`) after them, verbatim and in their
original order: a launch on one platform never loses the other's
preferences or its update-check clock. The two update-check pairs are kept
apart on purpose: `updcheck=`/`updtag=` are the Windows reader's stamp and
mean the newest non-prerelease overall there (src/main.cpp:2435), while the
Linux check answers a different question (the newest release carrying a
Linux archive) and keeps its own stamp in `lnxupdcheck=`/`lnxupdtag=`. One
key that changed meaning per platform would make a shared file tell each
reader the other's answer, so each reader reads and writes only its own
pair, and each pair survives the other platform's rewrites (the Linux
writer carries the Windows pair forward; the Windows writer drops the Linux
pair, which only costs the Linux reader one fresh check next time).

Because the file is shared, only one window may write it. The Windows build
claims a named mutex at startup (src/main.cpp:6138) and a window that does not
own it writes neither app.txt nor last.txt. The port takes the same role with
an advisory lock on `instance.lock` in the same directory: the first reader
holds it for as long as it runs, and a second one reads the preferences but
writes nothing, so two open readers cannot overwrite each other's settings.
The guard is per platform, though - the flock cannot see the Windows mutex,
and the mutex cannot see the flock - so it holds for two Linux readers or
two Windows readers, not for one of each with the directory open at the same
instant; a synced directory is normally open on one machine at a time, which
is why the guarantee is stated per platform here and in README-linux.txt.
The kernel drops the lock when the process dies, so a crashed reader does not
leave the role locked.

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
  (including the `app.txt` keys only the Windows reader owns), the update
  check's version rules, feed parsing and release lookup, the JPEG guard and
  the format interop.
- `scripts/linux-gate.sh`'s second layer runs the real binary on Xvfb and asserts
  on the pixels it paints (the page is mostly paper and carries text), on the
  state files it writes, and on the quit path logging no `Gtk-CRITICAL`.

The app exposes the same kind of seam the Windows suites use: `MNPDF_SHOT`
writes its back buffer to a PNG after the first render, and `MNPDF_SHOT_EXIT`
quits after, so a pixel assertion needs no harness clicking a window.

## Resource cost

Measured on this machine (`/usr/bin/time`, and RSS sampled from `/proc`):

- Headless core test, 171 checks including two full save/bake round trips:
  **0.26 s wall, 21 MB peak RSS** (measured, `/usr/bin/time -v`; the suite
  links libcurl and the update and instance modules, and exercises the
  update check's parsing, comparison and release lookup).
- The GUI with a 13-page document open, idle after 20 s: **98.6 MB RSS, steady
  state, no growth** (measured, sampled from `/proc/<pid>/status`). Of that,
  the vendored `libpdfium.so` file itself is 7.9 MB (measured, `ls`); how much of
  it is resident is an estimate, and GTK accounts for the rest. The only
  allocation that scales with the window is one client-sized BGRA buffer, the same
  discipline as the Windows build, and the print path allocates exactly one page
  raster at a time, sized from the page's own box at 300 dpi and shrunk to the
  same 16-megapixel budget the Windows print path applies.
- The thumbnails drawer holds one page raster at a time, where the Windows
  drawer holds one bitmap per row (src/main.cpp:4803). The cost is invisible
  but real: scrolling down and back up re-rasterizes every row that left the
  viewport, and each raster is a `FPDF_LoadPage` plus `FPDF_RenderPageBitmap`
  on the UI thread inside the draw signal, so a long document's drawer scrolls
  more slowly than the Windows one. The bound is the reason: a per-row cache
  would grow with the document, and the page cache the engine already keeps is
  six pages deep (`PCACHE 6` in src/linux/doc.cpp).

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
the same nesting and the same "This document has no bookmarks." fallback. The
thumbnail drawer answers the same keys the Windows one does (src/main.cpp:5031):
click a preview to jump there, Ctrl+Up and Ctrl+Down move the selected page,
Delete removes it and R turns it - on the drawer's own selection, not on the
page the document happens to show.

One GTK detail worth recording, because it is invisible until the drawer is
missing: a widget packed into a box that has already negotiated its size keeps
a 1-px allocation until BOTH the strip and the window are told to lay
themselves out again. Without that, the drawer is created, visible, and
invisible on screen. Measured with a GtkBox probe of the app's own shape (menu
bar, strip, overlay). The layout itself is left to the main loop, so a command
handler never runs one from inside itself.

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
page, Move page up, Move page down - moved into a free range (310-312), and
"Open in new tab..." (108) has no Windows counterpart either, so it is
Linux-only in the same way.

**The greying.** Windows greys a command out when it cannot run
(src/main.cpp:5543-5560 for the context menu, advancedMenu :5450-5461 for the
cascade): New Tab with no room left, Close Tab with one tab, Save and Save As
when there is nothing unsaved, Print and the rotations without a document,
Undo and Redo with an empty stack, and the cascade's own rules. The port has
one predicate for all of it, `commandUsable`, and both menus read it: the
context menu when it is built (as Windows builds its menu per open), and the
menu bar from `refreshMenuLabels` plus a `show` handler on each submenu, which
is the closest GTK gets to rebuilding it per popup. Two copies of those rules
had drifted before - the context menu greyed nothing at all while the cascade
greyed its own entries.

The label of 111 carries the window's state too, as the Windows one does
(src/main.cpp:5565: `IsZoomed(h) ? "Restore" : "Maximize"`), and 131 reads
"Edit text" like Windows rather than the port's old "Edit note".

**The wording.** The whole menu is worded as the Windows menu is, capitalization
and accelerator hint included: "New Tab\tCtrl+T", "Close Tab\tCtrl+W",
"Save\tCtrl+S", "Save As...\tCtrl+Shift+S", "Print...\tCtrl+P",
"Fit Width\tCtrl+0", "Zoom In\t+", "Zoom Out\t-", "Undo\tCtrl+Z",
"Redo\tCtrl+Y", "Find...\tCtrl+F", "Open...\tCtrl+O" and "Copy\tCtrl+C"
(src/main.cpp:5530, :5542-5557). The port implements those keys, so the hint is
true here as well. GTK renders the `\t` as whitespace in the label, so the hint
sits beside the words rather than in a right-aligned accelerator column - the
wording is identical, the alignment is the toolkit's.

**The launch.** Windows offers the Open dialog whenever nothing opened at launch (src/main.cpp:6251): a plain second launch, which opens nothing by design, a first launch with no remembered document, and a launch whose remembered document no longer exists - the existence is checked before opening (src/main.cpp:6248), so a reader whose file was moved is offered a chooser rather than an error. The port does the same: every launch that opened nothing ends in the Open dialog, and cancelling it leaves the empty reader. The gate observes that chooser by its title, not by a window count: the plain-second-launch section greps the "Open PDF" toplevel name (every count in the gate runs with a document argument, so no count can see a chooser that only a document-less launch opens).

**The update check.** This is the one place the port deliberately does not
follow the Windows route. The Windows app reads `/releases/latest`, which
answers with the newest NON-prerelease, and then fetches
`mnpdf-win-x64-<tag>.zip` by name. A Linux-only release is a prerelease until
the Windows archive is published beside it, and the asset that endpoint names
is the Windows one. So the port reads the release feed, picks the newest
release that actually carries `mnpdf-linux-x64-<tag>.tar.gz`, then compares
versions with the Windows rules (src/main.cpp:2334). The request runs on its
own thread and the whole check - the feed, the asset probes and the release
lookup - fits in one budget, so one click cannot turn into minutes of waiting
and a slow network cannot freeze the window. Every run that waits for the
answer waits longer than that budget - the shot-and-quit seam
(`MNPDF_SHOT_EXIT`) and the two launch runs alike - so a working but slow
network still gets its answer instead of a quit.

Where the release list comes from, and where the asset list does not, was
measured rather than guessed:

- The **releases page** carries the release NOTES but not the assets. GitHub
  renders the asset list lazily, so a scan of the page for the archive name
  reads the notes - and the notes are prose, which a release is free to
  mention or not mention. An earlier version of this check did exactly that,
  and it would have read the wrong version from the first release whose notes
  mention no archive.
- The **release feed** (`/releases.atom`) is GitHub's own Atom rendering of
  the releases. It lists the prereleases as well as the releases, newest
  first, which is what a Linux-only release needs. It carries no asset list
  either - it says which releases exist, not what they hold.
- The **download URL** answers the asset question. A HEAD on
  `/releases/download/<tag>/mnpdf-linux-x64-<tag>.tar.gz` answers 302 when
  the archive is there and 404 when it is not, so the check confirms the
  asset the same way a reader would - by asking for the file. Measured
  against the live site: `v2.7.0` answers 302, and `v2.6.0`, `v2.5.0` and
  `v2.4.0` answer 404, so exactly one release carries a Linux build.

The feed's own id ends in `releases`, not in a tag, and is skipped rather than
read as one. The parsing is split from the request so it can be tested
without a network: the engine suite covers the feed's entry id, the feed's
own id, an id that is not a version, and an empty feed.
The version the port reports is generated at build time from the Windows
`kAppVersion` constant (CMakeLists.txt reads src/main.cpp), so the two readers
can never claim different versions - which matters, because they share one
app.txt.

Five parity rules the Windows build taught the port, each easy to get wrong,
and two more of the port's own that the check needs:

- **The check runs at launch too, silently.** The Windows build starts a
  check before its message loop (startUpdateCheck(false), src/main.cpp:6286)
  and only speaks when a newer release exists: a check with nothing to say,
  or one that could not reach the host, says nothing, and a launch inside the
  cooldown says nothing even when the cached answer names a newer one - the
  cached answer is only surfaced by a click, because a click is the reader
  asking. The port runs the same silent check before its main loop; a modal
  at every launch would be a nag, and a launch that answered from the cache
  would offer the same release at every start of a run that already knows
  about it.
- **The attempt is stamped on the UI thread.** The worker thread makes the
  request; the window procedure stamps the attempt into app.txt
  (src/main.cpp:6085). The port stamps it in the reply handler, for the same
  reason: `writeAppPref` writes the shared app.txt, and every other writer of
  it runs on the GTK main thread. Only an attempt that really asked counts:
  an answer taken from the cache spends no request, so it must not push the
  cooldown's end away, or a reader who clicks once an hour would never ask
  again and would keep being told what the first attempt saw.
- **A click inside the cooldown is answered from the cache**, not refused. The
  Windows build reports what the last attempt learned
  (src/main.cpp:3156-3180), so the reader gets a true answer and no request
  is spent. The port does the same, and says when the last attempt could not
  reach GitHub and how long is left.
- **One check at a time.** The Windows build gates the click on the UI thread
  before it spawns anything (src/main.cpp:3188), so a second click inside the
  first request's own window makes one request, not two. The port gates it the
  same way, and the gate asserts it by counting the requests the reader
  actually made.
- **A click during a check is answered, not dropped.** Windows records the
  click in `gManualUpdateRequested` before its in-flight test
  (src/main.cpp:3183), the worker takes the flag at its tail (:3201), and the
  result handler re-posts command 170 for anything that arrived after (:6089).
  The port keeps the same flag (`gUpdateCheckWanted`) in the same places, so a
  click during the silent launch check gets its answer instead of vanishing -
  which is what it did while the flag was only a function argument.
- **The outcome reaches stdout, not only a dialog.** A dialog is a GTK window a
  headless run can neither read nor dismiss, so the reply handler also prints
  `update check: outcome current|newer <tag>`, or `outcome error` plus the
  reason, before the silent filter drops the dialog. That is what lets the gate
  tell a host that did not answer from a host that answered and has no Linux
  release - and from a check that is simply broken.
- **The asset check is three-valued.** "The release carries the archive", "it
  does not" and "the host did not answer" are three different answers, and the
  third is not the second: a timeout is not evidence that the archive is
  missing. The port says so rather than reporting a missing build.

The rotation commands act on the page the context menu was opened on, not on
the page the document happens to be showing, the way the Windows build does
with `gMenuRotPage` (src/main.cpp:5514, :5748). A right-click on page 3
rotates page 3 even when page 7 is on screen.

### The one place the port is cheaper than the Windows build

The thumbnail drawer keeps ONE page raster, where the Windows drawer keeps a
picture per row (src/main.cpp:4805-4859). Scrolling down and back up
re-rasterizes the rows that left the viewport, and each raster is a
`FPDF_LoadPage` plus a `FPDF_RenderPageBitmap` on the UI thread inside the
draw signal, so a 100-page document's drawer scrolls more slowly than the
Windows one does.

The trade was made on purpose: a picture per row for a 100-page document is
100 page-sized bitmaps held at once, and the box this runs on has 15 GiB of RAM
with about 10 GiB already in use. One raster costs one page. The Windows
build's own row cache is not carried over until the port can hold it.

Everything else about the drawer follows the Windows shape: a refresh rebuilds
the row heights and drops the pictures on both platforms - the Windows
refreshThumbDrawer calls layoutThumbs, which opens with clearThumbCache
(src/main.cpp:4813-4817, :4825, :4968), and the port drops its single raster in
the `gThumbPix.clear()` that opens `layoutThumbs` (src/linux/main.cpp) - the
only difference is the granularity, one raster here against a picture per row
there. The selection stays on the page it named unless that page is gone
(src/main.cpp:4964-4967), and the scroll offset is clamped when the list
shrinks.

## The test seams

`MNPDF_CMD` drives one command by its menu id, the way the Windows suites post
WM_COMMAND, so a suite can assert on chrome the page back buffer never holds.
It runs on its own turn of the main loop and the picture is taken on the next
one, so a drawer a command opened is laid out before it is captured. It may
carry more than one id (`MNPDF_CMD=170,170`), and they all run in that one
turn: a suite uses that to click twice inside one request's own window and
see that the second click spends no request.
`MNPDF_SHOT_WINDOW` captures the whole window instead of only the page buffer.
`MNPDF_MENU_DUMP` prints the menu bar and the context menu as text, every
entry that posts a command as `label: id`, then quits through the app's own
quit path. An entry that cannot run carries ` [greyed]` after its id, so the
suite can assert the greying as well as the command set: a menu that greyed
everything and a menu that greyed nothing are both wrong, and both are visible
here. The context menu
is printed in every shape it has - nothing under the cursor, a highlight, a
pin, a live selection - because the mark-specific commands exist only in
their own shape. The dump is asserted against the command table the Windows
suites already own (`tests/lib.ps1`'s `CMD_*` ids), so a missing command or a
wrong id fails the gate; the menu bar's entries post the same commands and
carry the same ids, so they are asserted there too. The gate then drives ids
113, 170, 201 and 202 and
asserts what each wrote to app.txt or to the pixels.

The update check also prints what it concluded - `update check: outcome
current|newer <tag>`, or `outcome error` and the reason - on stdout, before
the silent-launch filter drops the dialog. A dialog is a GTK window a headless
run can neither read nor dismiss, so without this trace a suite could only
grep the source, which proves what the source says rather than what the reader
did. The three answers it keeps apart are "a release was found", "the host
answered and has none" and "the host did not answer": the last is a network
fact, and the gate reports it as untested rather than as a failure.

The trace is a production behaviour, not only a seam: the Windows reader prints
nothing at all, while a Linux launch from a terminal shows a line per request
(the method and the URL) and then the outcome - one check makes several
requests, the feed, the asset probes and the release lookup among them. It is
deliberate and diagnostic - a reader who wonders whether the check ran can see
it did - and the lines carry only the request URL and the outcome; the error
text is the fixed sentences `src/linux/update.cpp` builds from curl codes and
HTTP statuses, never a body read from the host.

`MNPDF_SHOT_EXIT` is a fourth seam, and it is a window-level one rather than a
fourth independent thing: it appends the quit command to whatever else the run
asked for. A run that asks only for a quit therefore quits, and a run that asks
for a command and a quit drives the command and then quits.

`MNPDF_NO_UPDATE` is the fifth, and it exists because the port does at every
launch what the Windows build does (a silent update check before the message
loop, `startUpdateCheck(false)`, src/main.cpp:6286): a suite that is not
about the update check sets it so its runs stay off the network. It skips only
the launch check - a suite that clicks "Check for updates" gets the real
answer, because a click is the reader asking.

## Device scale and input

The frame is a device-pixel buffer: the drawing area's allocation is logical, so
`onAreaSize` multiplies it by `ui_scale()` and every geometry function in
`doc.cpp` works in device pixels (`px()`/`designPx()` for the layout constants,
`pagePtToPx`/`pagePxToPt` for page coordinates). GTK3 only reports an integer
scale factor, and the default desktop launch is a native Wayland client: KDE
publishes wl_output scale 2 for its 1.6 desktop (measured with wayland-info), so
the toolkit's own factor already renders the frame sharp - the compositor
downsamples the 2x buffer onto the 1.6x panel. A backend without a usable factor
falls back to the X screen's published DPI: XWayland's millimetre size works out
to exactly 96 dpi whatever the desktop scale is (measured: 2880 px over 762 mm),
so it is only trusted when it is not that default, and the desktop's Xft.dpi -
153 here, 1.6 x 96 - carries the fraction. `MNPDF_SCALE=<factor>` overrides every
source when a setup publishes nothing usable. The compositor then receives a
native-resolution frame instead of stretching a 1x one - the "low resolution"
report. `ui_paint` sets the surface's device scale so the device-pixel buffer
maps onto the widget's logical coordinates, and the scale is re-derived when the
window moves to an output with a different one (`notify::scale-factor`).

Pointer coordinates arrive in logical pixels and are converted once, at each
handler (`devX`/`devY`), so the hit-test code stays in one space.

Touchpad scrolling takes GDK's smooth deltas directly (one wheel notch is still
one step) instead of accumulating them to a fixed 120 px jump, which is what made
a two-finger scroll feel unresponsive and coarse. A scroll that arrives while a
context popup is open is ignored rather than repainting under the menu.

## Desktop integration

An unpacked tarball is runnable but invisible: nothing in an application menu
lists it, and pinning its window to a taskbar leaves an icon that cannot
re-launch it (measured on KDE: the menu search found nothing, and the pinned
entry went blank once the window closed - a pinned window resolves through a
desktop file, and there was none). `scripts/install-menu-entry.sh`, which ships
in the archive, fixes that in one command: it writes
`~/.local/share/applications/mnpdf.desktop` pointing at the copy it sits beside,
converts the Windows icon (`mnpdf.ico`, PIL if present) into themed PNGs, and
refreshes the desktop's caches. The entry carries
`StartupWMClass=Mnpdf` - the class the port's windows report (measured with
`xwininfo`: `("mnpdf" "Mnpdf")`) - so the taskbar groups the running window with
the pinned launcher instead of leaving it dead.

## Releasing

A release is one GitHub release carrying both platform assets. The Windows
asset is not optional even for a Linux release: the Windows app discovers
updates through GitHub's `/releases/latest` and then fetches
`mnpdf-win-x64-<tag>.zip` by name (`src/main.cpp:2572`), so a release carrying
only a Linux archive would tell every installed Windows copy that a newer
version exists and then fail to download it. Both assets therefore have to be
attached to the same release.

Cutting one: push a matching tag and let the release workflow build and
publish both platforms from it (`.github/workflows/release.yml`). The tag,
`kAppVersion` in `src/main.cpp` and the first line of `README.txt` must agree -
the workflow's contract job enforces exactly that before either build runs,
because the updater downloads `mnpdf-win-x64-<tag>.zip` by name and a release
whose version nobody built is worse than no release.

```sh
# bump both version lines (kAppVersion in src/main.cpp, README.txt), commit
git commit -am "release: v2.8.1" && git push origin main
git tag v2.8.1 && git push origin v2.8.1
# the workflow builds Windows (build.bat + the nine suites) and Linux
# (build-linux.sh + the gate) on their own runners, asserts both assets
# exist, and creates the release with both of them - nothing to run by hand
```

Building the assets by hand still works the same way, and a Linux-only
release (no Windows zip yet) must stay a prerelease: `/releases/latest` skips
prereleases, so it keeps naming the newest release a Windows reader can
actually fetch.

```sh
./build.bat                                     # -> build\mnpdf.exe
powershell -File scripts/make-release-zip.ps1   # -> build\mnpdf-win-x64-<tag>.zip
./build-linux.sh
./scripts/make-release-tarball.sh               # -> build-linux/mnpdf-linux-x64-<tag>.tar.gz
# hand-publish only when the workflow cannot run; a Linux-only hand release:
gh release create v2.8.1 --prerelease --title "mnpdf v2.8.1" \
    build-linux/mnpdf-linux-x64-v2.8.1.tar.gz
# then when the Windows zip ships, upload it and clear the flag:
#   gh release upload v2.8.1 build/mnpdf-win-x64-v2.8.1.zip
#   gh release edit v2.8.1 --prerelease=false
```

The Linux asset is a plain tarball, and `scripts/make-release-tarball.sh`
produces exactly the layout that is published: a `mnpdf-linux-x64-<tag>/`
directory holding the binary, the vendored `libpdfium.so` (found through an
`$ORIGIN` rpath), `PDFIUM-LICENSE.txt` and `README-linux.txt`. (The published
v2.7.0 asset also carries the Windows `README.txt`, whose first lines tell
the reader to run `mnpdf.exe`; from the next release the Linux asset ships
`README-linux.txt` alone, and the Windows ZIP keeps `README.txt` as its
version contract.)
It needs no install step and no root; the system dependencies are GTK3, which
every desktop already has, and libcurl (`libcurl4-openssl-dev` to build,
`libcurl.so.4` at runtime) for the update check. `ldd` on the unpacked binary
resolves `libpdfium.so` from beside it, so there is no `LD_LIBRARY_PATH` to
set.
