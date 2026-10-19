# mnpdf

A small, fast, native PDF reader with a highlighter. One executable plus PDFium,
no installer, no runtime, no framework.

Two platform builds live in this repository:

- **Windows** (`src/main.cpp`, `build.bat`) - the original Win32 + GDI build,
  shipped as `mnpdf-win-x64-<tag>.zip`.
- **Linux** (`src/linux/`, `build-linux.sh`) - the same document engine on GTK3 +
  cairo, shipped as `mnpdf-linux-x64-<tag>.tar.gz`. See `docs/linux.md`.

Both write the same sidecar and preference formats, so a document marked up on
one is understood by the other.

## Repo wiki

```
src/main.cpp          the whole Windows application (Win32 + GDI + GDI+)
src/linux/            the Linux port: one directory per concern
  doc.h               shared core state: Doc bundle, palette, geometry, undo,
                      the PDFium-facing API surface every module sees
  doc.cpp             state, geometry, palette, page cache, selection, marks,
                      undo/redo (ported from main.cpp)
  render.cpp          the screen-sized back buffer, overlays, night mode,
                      print raster (GTK/cairo replaces GDI)
  search.cpp          find: runSearch, nextMatch, match remapping
  sidecar.cpp         per-document sidecar, app.txt prefs, last.txt
  opendoc.cpp         open, save/bake, signature stamps, the JPEG guard
  surgery.cpp         delete/move page, merge, split (FPDF_ImportPages)
  main.cpp            the GTK3 platform layer and the ui_* seam
  ui.h                the platform seam: what the core may ask the UI for
  probe.cpp           toolchain probe: PDFium renders before the app is built
tests/core-test.cpp  headless test of the whole document engine
tests/mnpdf-baked.pdf  a PDF carrying mnpdf's own baked annotations, for the
                      Windows->Linux format interop check
scripts/linux-gate.sh  drives the real binary on Xvfb and asserts on the pixels
                      it paints and the state files it writes
third_party/pdfium/     Windows PDFium (headers, import lib, DLL, license)
third_party/pdfium-linux/  Linux PDFium (headers, libpdfium.so, license)
build.bat              Windows build recipe
build-linux.sh         Linux build recipe (cmake + the PDFium probe first)
CMakeLists.txt         the Linux build
.github/workflows/ci.yml  Windows CI (test-ci.ps1 on a Windows runner)
test-*.ps1             the Windows feature suites (drive the real binary)
scripts/test-ci.ps1    Windows/CI entry point
```

## Building

Linux needs cmake, g++, GTK3 dev headers and the vendored PDFium:

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
