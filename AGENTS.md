# mnpdf

A small, fast, native PDF reader with a highlighter. One executable plus PDFium,
no installer, no runtime, no framework.

Two platform builds live in this repository:

- **Windows** (`src/main.cpp`, `build.bat`) - the original Win32 + GDI build,
  shipped as `mnpdf-win-x64-<tag>.zip`.
- **Linux** (`src/linux/`, `build-linux.sh`) - the same document engine on GTK3 +
  cairo, built into `build-linux/mnpdf`;
  `scripts/make-release-tarball.sh` packages the release asset.
  See `docs/linux.md`.

Both write the same sidecar and preference formats, so a document marked up on
one is understood by the other.

## Repo wiki

```
src/main.cpp             the whole Windows application (Win32 + GDI + GDI+)
src/linux/               the Linux port: one directory per concern
  doc.h                  shared core state: Doc bundle, palette, geometry,
                         undo, and the API surface every module sees
  doc.cpp                state, geometry, palette, page cache, selection,
                         marks, undo/redo (ported from main.cpp)
  render.cpp             the screen-sized back buffer, overlays, night mode,
                         print raster (GTK/cairo replaces GDI)
  search.cpp             find: runSearch, nextMatch, match remapping
  sidecar.cpp            per-document sidecar, app.txt prefs, last.txt
  opendoc.cpp            open, save/bake, signature stamps, the JPEG guard
  surgery.cpp            delete/move page, merge, split (FPDF_ImportPages)
  update.cpp             the update check: the web host, the version rules
  update.h               the check's API: the version rules, the feed parser,
                         the blocking half main.cpp runs on a worker thread
  instance.cpp           the shared-preference role: the lock file a second
                         window does not write past
  main.cpp               the GTK3 platform layer and the ui_* seam
  ui.h                   the platform seam: what the core may ask the UI for
  probe.cpp              toolchain probe: PDFium renders before the app is built
tests/core-test.cpp      the headless engine suite (CMake target
                         mnpdf-core-test); stub UI, no display needed
tests/mnpdf-baked.pdf    a PDF carrying mnpdf's own baked annotations, for the
                         Windows->Linux format interop check
tests/arc.pdf            the default fixture the suites open (13 pages)
tests/arc-annot.pdf      a fixture carrying annotations
tests/sig-sample.jpg     a real signature JPEG, used by the refusals test
tests/lib.ps1            shared Windows suite harness (window lookup, launch)
tests/watchdog.ps1        Windows suite watchdog
scripts/linux-gate.sh    the Linux gate: builds, runs the headless engine suite,
                         then drives the real binary on Xvfb
scripts/make-release-tarball.sh  the Linux release asset: mnpdf-linux-x64-<tag>.tar.gz
scripts/install-menu-entry.sh    ships in the tarball: writes the desktop entry
                         and icon so the unpacked reader is findable by name
scripts/gate-common.ps1  shared Windows gate recipe (one pass/fail rule)
scripts/gate-test.sh     the Windows gate's WSL entry point
scripts/test-ci.ps1      the Windows/CI entry point (same nine suites)
scripts/suites.txt       the Windows gate's suite roster
scripts/make-release-zip.ps1  the Windows release ZIP
build.bat                the Windows build recipe
build-linux.sh           the Linux build recipe (cmake, plus the PDFium probe
                         and the engine suite first)
CMakeLists.txt           the Linux build, including the mnpdf-core-test target
.github/workflows/ci.yml the Windows CI (test-ci.ps1 on a Windows runner)
.github/workflows/release.yml the release pipeline: both platforms built from
                         a v* tag, both assets asserted, then published
docs/linux.md            the Linux port: decisions, format contract, tests,
                         measured resource cost
docs/reviews/            codebase review reports (dated per pass)
  2026-10-19-codebase-review.md   the first review of the Linux port (30 findings)
  2026-10-21-pipeline-findings.md every pipeline finding, its fix commit and why
  2026-10-21-pipeline-findings.json  the rounds up to 11 only, machine-readable
test-*.ps1               the Windows feature suites (drive the real binary)
select-msg-test.ps1      a Windows suite driving the selection path
README.md / README.txt   the user README; README.txt is the version contract
README-linux.txt         the Linux reader's own readme, inside the Linux tarball
PDFIUM-LICENSE.txt       the PDFium license, shipped in release ZIPs
mnpdf.ico, mnpdf.rc, resource.h   the Windows icon resource
mnpdf.png               the Linux window/launcher artwork (512px PNG, the
                         same mn-brand source as mnpdf.ico)
third_party/pdfium/        Windows PDFium (headers, import lib, DLL, license)
third_party/pdfium-linux/  Linux PDFium (headers, libpdfium.so, license)
```

## Building

Linux needs cmake, g++, the GTK3 dev headers, libcurl
(`libcurl4-openssl-dev`; the reader links `libcurl.so.4` at runtime for the
update check) and the vendored PDFium:

```
./build-linux.sh        # -> build-linux/mnpdf (+ libpdfium.so beside it)
./scripts/linux-gate.sh # drives the built binary on a virtual display
```

The Linux build copies `libpdfium.so` next to the executable and sets an
`$ORIGIN` rpath, so `build-linux/mnpdf` runs with no `LD_LIBRARY_PATH` - the
same "keep pdfium next to the exe" rule the Windows release follows.

## Where things live (Linux)

`$XDG_CONFIG_HOME/mnpdf` (`~/.config/mnpdf` by default), the counterpart of the
Windows `%APPDATA%\mnpdf`, with byte-identical file formats:

| File | Contents |
|---|---|
| `app.txt` | tab strip, autosave, default highlight and pin colours, night mode, custom palette, window geometry |
| `doc-<hash>.txt` | per-document sidecar: zoom, fit, page, highlights, rotations, pins, signature stamps, deleted marks |
| `last.txt` | the document a plain launch reopens |
| `instance.lock` | the lock file that names the window allowed to write `app.txt` and `last.txt` |

The sidecar name is the same FNV-1a hash of the document path the Windows app
uses, so a shared config directory lines both readers up on the same document.

## Tests

```
bash scripts/linux-gate.sh     # the real binary on Xvfb: pixels + state files
```

`tests/core-test.cpp` links the platform-independent modules directly and drives
the whole document engine headlessly (open, geometry, selection, marks, undo,
sidecar round trip, save/bake, page surgery, format interop). It is built by
`build-linux.sh`-adjacent tooling; the GUI gate is `scripts/linux-gate.sh`.
