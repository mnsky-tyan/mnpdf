# Project agent memory

This file is the project's committed home for project-intrinsic agent knowledge: build, test, release, architecture, and sharp-edge notes that should travel with the code.

- Add durable project-specific notes here as they are discovered through real work.

## Maintaining this file

Keep this file for knowledge useful to almost every future agent session in this project.
Do not repeat what the codebase already shows; point to the authoritative file or command instead.
Prefer rewriting or pruning existing entries over appending new ones.
When updating this file, preserve this bar for all agents and keep entries concise.

## Build / test / verify

- `npm test` runs the whole suite (`node --test`, test/*.test.mjs, ~1s). There is no lint or typecheck step.
- `npm run build` (vite) is the only build check; the 500 kB bundle-size warning is pre-existing noise.
- Native visual (GUI) checks are the captain's Windows review: this WSL box lacks the native browser libraries (libnspr4/nss) needed for headless browser smoke, so any agent that tries it hangs in `find /` for browser libs. Do not start browser-smoke-style test agents here; verify selection geometry headlessly via the exported functions in `src/selection.js`.

## Selection architecture (sharp edges)

- Selection geometry is pure and unit-testable in `src/selection.js`; `src/annos.js` wires DOM events to it. Combined line offsets are in full-span coordinates (`vis.start` offsets the trimmed slice); every new mapping must keep paint and copy derived from the same window (`buildSegments` + `segText`).
- pdf.js splits one visual line into several spans sharing a vertical box; `groupLineSpans` merges only when overlap covers at least half the shorter box (tight leading must not merge). Change either side with the tests in `test/selection.test.mjs`.
