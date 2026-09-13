# mnpdf

A very lightweight, local-only desktop PDF **reader + highlighter** for personal use.
No toolbar, no sidebar, no accounts, no network — just a clean page canvas and a
small set of interactions that stay out of your way.

Built with [Tauri 2](https://tauri.app) + [pdf.js](https://mozilla.github.io/pdf.js/) +
[pdf-lib](https://pdf-lib.js.org). Runs entirely on your machine.

## Using it

| Action | How |
| --- | --- |
| Open a PDF | Ctrl+O, right-click → *Open…*, drop a file on the window, or `mnpdf.exe path\to.pdf` |
| Scroll | wheel / two-finger touchpad scroll |
| Zoom | pinch (two-finger) or Ctrl+wheel, `+` / `-`, `Ctrl+0` fit width, `0` = 100% |
| Highlight text | select text → click a color on the floating bar (or right-click → *Highlight*) |
| Delete a highlight | right-click on the highlight → *Delete highlight* |
| Add a text note | right-click a page → *Add text here* (Ctrl+Enter commits, Esc cancels) |
| Move / edit / delete note | drag it / double-click it / right-click or select + Del |
| Undo / redo | Ctrl+Z / Ctrl+Y (covers highlights, notes, page ops) |
| Find text | Ctrl+F (or `/`), Enter / F3 / Shift+F3 to jump matches |
| Go to page | Ctrl+G, or click the page pill at the bottom |
| Thumbnails | F9 or right-click → *Thumbnails* (click to jump, drag to reorder, right-click to rotate/delete) |
| Rotate / delete / reorder pages | page or thumbnail right-click menus |
| Save (bakes edits into the PDF) | Ctrl+S |
| Save As… | Ctrl+Shift+S |

The interface follows the OS light/dark theme automatically. Pages stay as printed;
only the chrome changes.

Zoom level, last page, and *unsaved* highlights/notes are remembered per document
(a small JSON sidecar in the app data folder), so you never lose an annotation by
accidentally closing the window. **Save** bakes highlights and notes permanently
into the PDF content and reloads the document.

## Deliberate limitations

- Saving bakes annotations into page content — after saving they behave like any
  other printed ink (they can't be selected/removed in mnpdf anymore). Bookmarks,
  links and existing pages are preserved on save.
- Note text uses the built-in Helvetica; characters outside Latin-1 are replaced
  with `?` when saved.
- No signatures, OCR, forms, passwords, compression, drawing tools, multimedia,
  collaboration, cloud sync, accounts, or online services. By design.
- Password-protected PDFs are not supported.

## Building

Requirements: Node 18+, Rust toolchain (MSVC), WebView2 (preinstalled on Win 10/11).

```
npm install          # also copies pdf.js worker + cmaps into public/
npm run samples      # optional: generate public/sample.pdf for dev
npm run dev          # browser dev mode (file IO shimmed to localStorage/downloads)
npm run tauri dev    # desktop dev window
npm run tauri build  # release exe + NSIS installer
```

Artifacts: `src-tauri/target/release/mnpdf.exe` and
`src-tauri/target/release/bundle/nsis/`.

Automated GUI tests (needs Edge installed):

```
npm run dev          # in one terminal
node scripts/guitest.mjs   # in another
```

## Layout

```
src/            frontend (vanilla JS modules)
  viewer.js     page rendering, zoom, scroll, coordinate math
  annos.js      selection → highlights, notes, floating swatch bar
  save.js       pdf-lib baking (in-place page ops, copy fallback)
  commands.js   open/save/zoom/page actions + sidecar persistence
  search.js / thumbs.js / menu.js / main.js
src-tauri/      tiny Rust layer: file read/write, kv JSON store, CLI arg
scripts/        icon + sample generators, GUI test harness
```
