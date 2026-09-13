# mnpdf

A very lightweight, local-only desktop PDF **reader + highlighter** for personal use.
No toolbar, no sidebar, no title bar, no accounts, no network — just a clean page
canvas and a small set of interactions that stay out of your way.

Built with [Tauri 2](https://tauri.app) + [pdf.js](https://mozilla.github.io/pdf.js/) +
[pdf-lib](https://pdf-lib.js.org). Runs entirely on your machine.

## Using it

| Action | How |
| --- | --- |
| Open a PDF | Ctrl+O, right-click → *Open…*, drop a file on the window, or `mnpdf.exe path\to.pdf` |
| Scroll | wheel / two-finger touchpad scroll |
| Zoom | pinch (touch screen or touchpad), Ctrl+wheel, `+` / `-`, `Ctrl+0` fit width, `0` = 100% |
| Exact zoom | scroll a little, click the **right half** of the page pill, type a percent (25–600) |
| Go to page | Ctrl+G, or click the **left half** of the page pill |
| Highlight text | select text (nothing pops up), then **right-click** the selection → pick a color |
| Copy text | select text → right-click → *Copy text* (or Ctrl+C) |
| Delete a highlight | right-click on the highlight → *Delete highlight* |
| Text pins | right-click anywhere on a page → *Add pin here* → type → Ctrl+Enter. A small red dot stays on the page; **hovering shows the text**, clicking the dot re-opens the editor, right-click offers Edit/Delete |
| Undo / redo | Ctrl+Z / Ctrl+Y or right-click → *Undo* / *Redo* (covers highlights, pins, page ops) |
| Find text | Ctrl+F (or `/`), Enter / F3 / Shift+F3 to jump matches |
| Thumbnails | F9 or right-click → *Thumbnails* (click to jump, drag to reorder, right-click to rotate/delete) |
| Rotate / delete / reorder pages | page or thumbnail right-click menus |
| Save (bakes highlights into the PDF) | Ctrl+S |
| Save As… | Ctrl+Shift+S |
| Minimize / Maximize / Quit | right-click anywhere → bottom of the menu |
| Move the window | drag the thin invisible strip along the top edge (double-click = maximize) |

There is no title bar. The window chrome lives entirely in the right-click menu.

The interface follows the OS light/dark theme automatically. Pages stay as printed;
only the chrome changes.

## Where your edits live

- **Highlights** are baked into the PDF when you save (permanently, like ink).
- **Pins are not saved into the PDF at all** — their text is hover-only, so it
  lives in a small JSON sidecar per document (app data folder) together with your
  zoom level, page, and unsaved highlights. Pins survive closing, reopening, and
  saving, but they exist only inside mnpdf and won't appear in other viewers.

Unsaved highlights are also kept in the sidecar, so closing accidentally loses
nothing. On close with unsaved highlights you get one plain **"Save changes?" —
Yes/No** prompt.

## Deliberate limitations

- After saving, highlights behave like printed ink (not re-selectable/removable).
- Note (pin) text is mnpdf-only by design; it is never printed into the file.
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
  viewer.js     page rendering, zoom (wheel/pinch/pill), scroll, coordinates
  annos.js      selection → highlights, hover-only pins
  save.js       pdf-lib baking (in-place page ops, copy fallback)
  commands.js   open/save/zoom/page actions + sidecar persistence
  search.js / thumbs.js / menu.js / main.js
src-tauri/      tiny Rust layer: file read/write, kv JSON store, CLI arg
scripts/        icon + sample generators, GUI test harness
```
