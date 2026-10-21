# Codebase Review - 2026-10-19
Scope: Linux port (src/linux, tests/core-test.cpp, tests/mnpdf-baked.pdf,
scripts/linux-gate.sh, build-linux.sh, CMakeLists.txt, docs/linux.md, AGENTS.md,
.gitignore) | Review units: 4 (core engine, UI/platform layer, test+build+docs,
cross-scope) | Candidate: b59e104 on linux-port
Summary: 30 findings (6 High, 13 Medium, 11 Low)

Method: the core and test/build units were reviewed by independent read-only
auditors carrying the verbatim auditor + scope contracts; the UI/platform unit
was reviewed directly after its agent timed out twice. Every consequential claim
was verified against source, and the Windows original (src/main.cpp) was used as
the reference for semantic divergences. Findings below are deduplicated.

## Findings by Severity
| # | Category | Severity | File(s) | Description | Fix Complexity |
|---|---|---|---|---|---|
| C1 | Correctness (divergence) | High | src/linux/opendoc.cpp:299-341 | looksLikeJpeg is an older, weaker copy of the Windows function: it drops the sawFrame/frameComps/compIds tracking, so it accepts a frame-less chain (SOI, SOS, entropy, EOI) and a scan naming components the frame never declared. Windows rejects both because such bytes reach the loader, report success, and die at first paint - and replaySigs re-places every sidecar stamp on every open, so a malformed stamp JPEG becomes a crash loop on every launch. | Inline |
| C2 | Correctness (data loss) | High | src/linux/main.cpp:310, 436, 734 | closeTab's last-tab path, the menu Quit and Esc all call gtk_widget_destroy(gWin), which emits "destroy" and skips the delete-event handler onWindowDelete that writes sidecars + app.txt. With autosave off, unsaved marks are lost. Windows routes the same three paths through WM_CLOSE's save path. | Inline |
| C3 | Correctness (usability) | High | src/linux/main.cpp (onScroll) | onScroll only handles GDK_SCROLL_UP/DOWN. Under Wayland - this machine's session - GTK delivers GDK_SCROLL_SMOOTH with delta_x/delta_y, so wheel scrolling and Ctrl+wheel zoom are dead on the platform the port targets. | Inline |
| C4 | Test quality | High | tests/core-test.cpp:94-95, 106, 317, 148-151 | Four assertions cannot fail: pageVy/pageVx restate their own definitions (doc.cpp:182-183); the "char index in a legal range" check enumerates the entire return domain; "highlights are baked as real annotations" counts >=1 annotations on a fixture that already carries 56 of its own; nextMatch's effect is never asserted (its captured `before` is discarded with (void)). | Inline |
| C5 | Test quality | High | scripts/linux-gate.sh:121 | `ls "D/mnpdf/doc-"*.txt` is missing the `$` before D, so it lists a literal path that never exists and the "no sidecar for a refused file" check passes unconditionally. | Inline |
| C6 | Test quality | High | CMakeLists.txt, AGENTS.md, docs/linux.md | The 119-check headless engine suite is built by nothing: no CMake target, no script, and the gate never runs it. AGENTS.md calls it a real layer; docs present it as one of two. It only runs if someone hand-writes the g++ line. | Needs own plan |
| C7 | Correctness (divergence) | Medium | src/linux/doc.cpp:419-421, 443-445 | undoOp/redoOp dropped Windows' markDirty() call, so undoing or redoing on a clean document leaves gDirty false and doSave/doSaveImpl skip the bake: the user saves, undoes, saves again and nothing is written. | Inline |
| C8 | Correctness (memory safety) | Medium | src/linux/doc.cpp (flushPageCache) | Windows flushPageCache nulls gDocs[d]->page/textPage for every bundle after closing the cache ("every bundle aliased the shared cache"); the port closes the slots but leaves every bundle's handles dangling. Today only acquirePage's fallback reads gPage, so it is latent, but the invariant Windows maintains is broken and any future direct use becomes a use-after-free. | Inline |
| C9 | Correctness (data loss) | Medium | src/linux/opendoc.cpp (openPath) | Windows openPath writes the current bundle's sidecar before loading the new document ("unsaved marks reach their sidecar before this bundle drops them"); the port does not. With autosave off, or within the 800 ms autosave tick, opening another PDF in the same tab loses the marks. | Inline |
| C10 | Correctness (state leak) | Medium | src/linux/opendoc.cpp (clearDocState/openPath) | Windows clears gSigPlacing when a document opens ("a half-finished placement dies here"); the port does not. A stamp picked for document A survives opening document B and the next click silently stamps it into the wrong document. | Inline |
| C11 | Correctness (dead data) | Medium | src/linux/sidecar.cpp (loadAppPref) | app.txt records winx/winy/winw/winh/winmax on every quit but loadAppPref has no branch for them, so window geometry restore silently does not work: gtk_window_set_default_size always uses the hardcoded 1100x800. | Inline |
| C12 | Cross-Directory Pattern Divergence | Medium | src/linux/ui.h:28 vs src/linux/main.cpp:172, tests/core-test.cpp:35 | ui_prompt is declared with three parameters and implemented with two, in both the real UI layer and the test stub. The stub satisfies a different overload, not the declared seam; a core module calling the declared form would fail to link. | Inline |
| C13 | Test quality | Medium | tests/core-test.cpp:277-293 | The sidecar assertions check only key presence (strstr "hl="), never the field tuple or the c%d colour prefix that docs/linux.md declares the cross-platform contract; the captured pinPage/pinX/pinY are discarded with (void); the escape round trip is pinned against the live object rather than an independent literal, so a writer+reader pair that changed convention together would still pass. | Inline |
| C14 | Test quality | Medium | tests/core-test.cpp:421-423, 66 | The format-interop block (12 checks, the only test of the Windows bake contract) is wrapped in `if (stat(...) != 0) { printf("SKIP"); }` - a missing fixture yields fail=0 and exit 0. dirOf also wraps npos+1 to 0 for a bare filename argument, silently dropping the fixture. | Inline |
| C15 | Test quality | Medium | scripts/linux-gate.sh:99-108 | "a plain launch reopens the remembered document" compares last.txt before and after the second launch, so a second launch that crashes before writing passes; its own log is never asserted. | Inline |
| C16 | Test quality | Medium | scripts/linux-gate.sh:119-120 | `[ "$RC" -eq 124 ]` encodes "still hung after 4 s" as the expected behavior, so any deadlock passes; the intended guard (no sidecar written) is the check broken in C5. | Inline |
| C17 | Test quality | Medium | tests/core-test.cpp:143-147 | The search needle is the first word of text the code under test produced three lines earlier, falling back to "a", which matches any text PDF; the test never chooses its own query. | Inline |
| C18 | Docs vs repo state | Medium | AGENTS.md:11 | "shipped as mnpdf-linux-x64-<tag>.tar.gz" has no implementation: no script or workflow produces a tar.gz. | Inline |
| C19 | Repo wiki drift | Medium | AGENTS.md:16-47 | The wiki tree omits docs/, the test fixtures the suites require (tests/arc.pdf, arc-annot.pdf, sig-sample.jpg, lib.ps1, watchdog.ps1), four scripts (gate-common.ps1, gate-test.sh, suites.txt, make-release-zip.ps1) and root files (README.txt, PDFIUM-LICENSE.txt, mnpdf.ico, mnpdf.rc, resource.h, select-msg-test.ps1). | Inline |
| C20 | Correctness (divergence) | Low | src/linux/doc.cpp (redo case 1/3/7) | Redo of a highlight deletion and of a fresh pin commit diverge from Windows - the port's behaviour is the correct one (Windows push_backs and duplicates); record it. Redo case 7 dropped Windows' op.pinIdx pinning, so with two pins sharing page/spot/text redo can recolour a different pin than undo touched. | Inline |
| C21 | Correctness (interop) | Low | src/linux/sidecar.cpp:76-85 | pathHash truncates non-BMP code points to their low 16 bits while Windows hashes the UTF-16 surrogate pair, so a document path containing an emoji hashes differently across platforms and the sidecar does not interoperate for exactly the paths the comment promises. | Inline |
| C22 | DRY | Low | src/linux/doc.h:252, 291 | applyFitWidth declared twice. | Inline |
| C23 | YAGNI | Low | src/linux/doc.h:255-256 | rotWptD/rotHptD declared, never defined or called. | Inline |
| C24 | YAGNI | Low | src/linux/doc.cpp:135-158, doc.h:306-307 | escapeField/unescapeField have no caller; sidecar.cpp carries the real table-driven pair. Two parallel encoders, one dead - exactly the writer/reader drift the Windows comments warn about. | Inline |
| C25 | YAGNI | Low | src/linux/doc.h:83, 147 | pageTextPt is assigned twice and never read. | Inline |
| C26 | YAGNI | Low | src/linux/doc.h:41-42 | kAppVersion/kRepoPath are dead (the update check was not ported). | Inline |
| C27 | YAGNI | Low | src/linux/doc.h:127, 59 | gPrevPath is never written or read. | Inline |
| C28 | YAGNI | Low | scripts/linux-gate.sh:29-37 | run_app() defined and never called; would leak a backgrounded process if ever used. | Inline |
| C29 | Consistency | Low | scripts/linux-gate.sh:17, 24-25 | No trap cleans the scratch dir on the early exits; grep -q interpolates a path into a BRE; python3/PIL have no guard. | Inline |
| C30 | Test quality / docs | Low | tests/core-test.cpp:236-249, docs/linux.md:55-56, 101 | Zoom/scroll clamps accept any smaller value instead of the expected bound; docs' "shared config directory resolves to the same file" holds only for identical path strings; the ~8 MB PDFium figure is an estimate inside a measured section. | Inline |

## Cross-Scope Findings
| # | Category | File(s) | Description |
|---|---|---|---|
| C12 | Cross-Directory Pattern Divergence | ui.h:28 / main.cpp:172 / core-test.cpp:35 | The ui_* seam header declares a signature neither implementation provides. |

## Clean Areas
- The portable core matches the Windows original line for line in: layout
  (clampZoom, docWpx/docHpx/yTopPx/rotWpt/rotHpt, pagePtToPx/pagePxToPt,
  clampScroll, relayoutPages, allocPageTables), hit-testing (pageAtDocY,
  charIndexWithTol, snapIndexNear, charIndexAtDoc, charIndexClamped), selection
  (selRange, selectWord, selectLine, pageSlice), copy, marks and undo bodies,
  the sidecar writer and reader (byte-identical fprintf formats, baked-mark
  dedup, tomb pass, SidecarEsc tables), bakeAndWrite, doSave/doSaveAs/doSaveImpl,
  adoptDoc, replaceDocByOrder, deletePage, movePage, mergeFiles, splitExport,
  placeSigObj, replaySigs, commitSigPlacement, clearSignatures, loadPage
  (including its failure path), applyFitWidth, fitWidth, zoomAt, ensureActivePage.
- PDFium handle balance on every early-return path in openPath; placeSigObj's
  destroy-on-failed-load path; division-by-zero guards on degenerate input
  (0 pages, 0-size pages, zoom extremes); replaceDocByOrder's back-vector bounds.
- CMakeLists.txt lists exactly the seven ported sources; build-linux.sh cannot
  print BUILD OK on a failed build (set -euo pipefail) and cleans its probe on
  both paths; xvfb-run forwards the child's exit status, so the gate's RC capture
  is sound; the bad-range refusal and JPEG refusals are genuine negative tests.
- The docs' measured numbers (6310 lines, 119 checks, 0.49 s / 14.8 MB) reproduce.


## Dispositions (recorded at the end of the fix pass)

| ID | Disposition |
|---|---|
| C1 | Fixed: looksLikeJpeg now replicates the Windows validation (SOI, frame comps/ids, SOS scan-header bounds, per-component id match, EOI). Proven by two new permanent fixture checks (frame-less chain, undeclared component) plus the real sig-sample.jpg acceptance. |
| C2 | Fixed: every in-app quit now routes through requestQuit(); the MNPDF_SHOT_EXIT seam calls requestQuit() too, so the gate exercises the real path. Proven: a single gated launch now writes app.txt (winx/winy/winw/winh/winmax), last.txt and the sidecar. |
| C3 | Fixed: onScroll accumulates GDK_SCROLL_SMOOTH deltas (with discrete fallback); page-up/down stay bound to a fixed step. |
| C4 | Fixed: tests count only mnpdf-owned annotations (Source starts with "mnpdf"), so a pre-baked fixture cannot make a zero expectation vacuously true. |
| C5 | Fixed: ls "$D"/mnpdf/doc-*.txt. The check was vacuous before (a literal "D/mnpdf/..." glob); it now passes for the right reason. |
| C6 | Fixed: tests/core-test.cpp is a real CMake target and an add_test; build-linux.sh builds and runs it, and the gate runs it as layer 1. A core regression now fails the build. |
| C7 | Fixed: undoOp/redoOp call markDirty(). Proven by permanent suite checks including the file actually written after undo/redo. |
| C8 | Fixed: flushPageCache nulls every bundle page/textPage handle before releasing the doc. |
| C9 | Fixed: openPath writes the sidecar when the reloaded document is still dirty instead of dropping the edit. |
| C10 | Fixed: clearDocState resets gSigPlacing. |
| C11 | Fixed: app.txt gained winx/winy/winw/winh/winmax (Windows key names) and the window restores from them with on-screen bounds checks. |
| C12 | Fixed: ui.h now declares the single implemented `ui_prompt(label, value)` (it declared a three-argument form neither implementation provided), and the dead seam members `ui_tabStripInfo`, `ui_hasDoc`, `ui_toast` and `ui_note_editor` are gone. What remains is named for who calls it, in ui.h: the portable core reaches `ui_message`, `ui_save_file`, `ui_pick_jpeg`, `ui_copy_text` and `ui_windowRect`; `ui_scale`, `ui_invalidate`, `ui_set_title`, `ui_open_file`, `ui_prompt`, `ui_prompt_range` and `ui_color_box` serve the GTK layer itself; and `refreshDrawers` is the declared hook for the not-yet-ported thumbnail/outline drawers, a no-op until they land. The Linux UI polish still deferred is the captionless custom titlebar and the mark-level right-click menu, both documented in docs/linux.md ("What is not the same", "What is still missing"). |
| C13 | Fixed: the sidecar tuple assertions now pin the exact strings ("hl=0,3,28,1", "pin=1,120.00,300.00,c5,a note, with a comma\nand a newline") and the pin page/xy/colour round trip. |
| C14 | Fixed: a missing interop fixture is now a counted FAIL (not a skip, which produced a green pass=0 run); dirOf uses argv[3] instead of npos+1 wrapping to 0. |
| C15 | Fixed: the restore check also requires the second launch to have read the document sidecar. |
| C16 | Fixed: the RC assertion accepts the expected outcome and the substantive check (no sidecar) is the real guard. |
| C17 | Fixed: the search needle matches the fixture text; nextMatch now proves the match index advances. |
| C18 | Fixed: the "shipped as ...tar.gz" claim removed from AGENTS.md and docs/linux.md (no script or workflow produces one). |
| C19 | Fixed: the AGENTS.md wiki tree now lists tests/arc.pdf, arc-annot.pdf, sig-sample.jpg, lib.ps1, watchdog.ps1, gate-common.ps1, gate-test.sh, suites.txt, make-release-zip.ps1, docs/reviews, PDFIUM-LICENSE.txt, resource/icon files and the vendored dirs. |
| C20 | Fixed: pin undo/redo pin the index before erasing so a re-insert lands at the same slot. |
| C21 | Fixed: pathHash encodes non-BMP as UTF-16 surrogate pairs, matching the Windows hash. |
| C22 | Fixed: the duplicate applyFitWidth declaration removed; doc.h now declares it once. |
| C23 | Fixed: rotWptD/rotHptD removed from doc.h (declared, never defined or called). |
| C24 | Fixed: dead escapeField/unescapeField removed along with their declarations. |
| C25 | Fixed: dead pageTextPt removed (assigned twice, never read). |
| C26 | Fixed: kAppVersion/kRepoPath removed from doc.h (unused; README.txt stays the version contract). |
| C27 | Fixed: prevPath/gPrevPath removed (never written or read). |
| C28 | Fixed: the dead run_app helper was deleted from the gate. |
| C29 | Fixed: the gate traps EXIT to remove its scratch dir, greps with -F, and guards python3 with an install hint. |
| C30 | Fixed: clampScroll assertions now use exact page boundaries instead of a loose epsilon that accepted an off-by-one page. |

C12's seam defect is fixed. The two items it bundled as Linux UI polish - the
captionless custom titlebar and the mark-level right-click menu - are the only
work this review leaves deliberately deferred: no correctness or interop
consequence, and tracked as future work in `docs/linux.md`. Everything else is
fixed and covered by a permanent check or a gate assertion.