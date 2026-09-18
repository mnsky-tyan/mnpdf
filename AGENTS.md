# Project agent memory

This file is the project's committed home for project-intrinsic agent knowledge: build, test, release, architecture, and sharp-edge notes that should travel with the code.

- Add durable project-specific notes here as they are discovered through real work.

## Verify

- Unit tests (pure selection geometry, no DOM): `npm test` (node --test, `test/selection.test.mjs`).
- GUI end-to-end: `npm run samples` first (writes public/*.pdf), `npm run dev` on port 5173, then `node scripts/guitest.mjs [t17|t19|…]` — defaults to Windows Edge; set `MNPDF_BROWSER` to another chromium binary. While a dev app occupies port 5173, point the suite at another vite with `MNPDF_GUITEST_BASE`.
- Native Windows review from WSL: Windows node can run `scripts/guitest.mjs` (copy it to a Windows temp dir with `npm i playwright-core`) against the WSL vite server — Windows reaches it on `localhost:5173`.
- `src/selection.js` must stay DOM-free (pure geometry); the mnpdf-drawn selection pipeline is: `annos.js allLines()` → `buildSegments()` → `drawSelection()` → `renderSelection()`.
- Electron harness: `/tmp/pw/*.cjs` via playwright-core + nix electron, app host `/tmp/eapp` (fresh userData per launch — the app persists zoom/page per doc, a reused profile restores stale view state), `MNPDF_URL=http://localhost:5199/?file=arc.pdf`.
- Memory checks: `/tmp/pw/mem2.cjs` reports in-page canvas/heap per scenario; process-tree working set double-counts shared pages across the WebView2 tree — read private commit instead.

## Maintaining this file

Keep this file for knowledge useful to almost every future agent session in this project.
Do not repeat what the codebase already shows; point to the authoritative file or command instead.
Prefer rewriting or pruning existing entries over appending new ones.
When updating this file, preserve this bar for all agents and keep entries concise.
