# Project agent memory

This file is the project's committed home for project-intrinsic agent knowledge: build, test, release, architecture, and sharp-edge notes that should travel with the code.

- Add durable project-specific notes here as they are discovered through real work.

## Verify

- Unit tests (pure selection geometry, no DOM): `npm test` (node --test, `test/selection.test.mjs`).
- GUI end-to-end: `npm run samples` first (writes public/*.pdf), `npm run dev` on port 5173, then `node scripts/guitest.mjs [t17|t19|…]` — defaults to Windows Edge; set `MNPDF_BROWSER` to another chromium binary.
- Native Windows review from WSL: Windows node can run `scripts/guitest.mjs` (copy it to a Windows temp dir with `npm i playwright-core`) against the WSL vite server — Windows reaches it on `localhost:5173`.
- `src/selection.js` must stay DOM-free (pure geometry); the mnpdf-drawn selection pipeline is: `annos.js allLines()` → `buildSegments()` → `drawSelection()` → `renderSelection()`.

## Maintaining this file

Keep this file for knowledge useful to almost every future agent session in this project.
Do not repeat what the codebase already shows; point to the authoritative file or command instead.
Prefer rewriting or pruning existing entries over appending new ones.
When updating this file, preserve this bar for all agents and keep entries concise.
