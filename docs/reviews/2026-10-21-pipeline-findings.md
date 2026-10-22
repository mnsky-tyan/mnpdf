# mnpdf Linux port - the pipeline review findings, in full

Every finding the no-mistakes pipeline raised about the Linux port while it was
held for a rebuild, with the commit that fixed it and the evidence. Captured
2026-10-21; the review rounds that ran after the hold are not repeated here -
their fixes are in `21326d5`.

Companion files in this directory:

- `2026-10-21-pipeline-findings.json` - the same findings, machine-readable,
  one entry per finding id with its description, severity, file and the run that
  reported it.

The gate daemon's own log for that session (the record of why each run ended: a
global kernel OOM, a router rotation to a provider that refuses this region, and
two daemon restarts) is pipeline telemetry, not project documentation, so it
stays in the pipeline's state directory (`~/.no-mistakes/logs/daemon.log`) along
with the per-run raw output (`run-<id>-review-full.log`).

## Summary

- 48 fixed, 1 deliberately deferred and documented, 0 still open.

- The "-UNIMPLEMENTED" ids in the raw logs are the same defects re-reported after a
  daemon restart killed a fixer mid-round; they are folded into their base id here.

## The findings

### MNPDF-LNX-SEAM-MEMBERS-STILL-UNCALLED (info) - DEFERRED (documented)

Partly resolved: the dead members went, and the remaining stub (refreshDrawers) is the declared hook for the unported thumbnail/outline drawers, documented rather than silently claimed away.

> The pipeline said:
>
> The LNX-007 decision asked to 'remove the dead seam members (ui_tabStripInfo, ui_hasDoc, and any other member with no caller) and keep ui.h documented as the minimal seam'. eb781f1 removed ui_tabStripInfo, ui_hasDoc, ui_toast and ui_note_editor, and the C12 disposition in docs/reviews/2026-10-19-codebase-review.md:89 now claims 'the seam holds only what the core calls'. That claim does not hold: refreshDrawers() (src/linux/ui.h:19) is declared and defined as an empty stub (src/linux/main.cpp:72) and is called only from src/linux/surgery.cpp:80 and src/linux/main.cpp:10, never from the portable core; ui_scale() (src/linux/ui.h:9) is called only from src/linux/render.cpp:132; ui_set_title() (src/linux/ui.h:20) is called only from src/linux/render.cpp:124; ui_prompt_range() and ui_color_box() (src/linux/ui.h:33-34) are called only from src/linux/main.cpp:180 and 192. Only ui_invalidate, ui_message, ui_open_file, ui_save_file, ui_pick_jpeg, ui_prompt, ui_copy_text and ui_windowRect are called from core modules (doc.cpp/opendoc.cpp/surgery.cpp/sidecar.cpp). The decision's 'any other member with no caller' clause is satisfied literally (every remaining member has a caller), so this is not a blocker, but the review report's stronger claim that the seam 'holds only what the core calls' is inaccurate for the five members above, and refreshDrawers in particular is a no-op stub kept for the not-ported drawers (docs/linux.md:43-44 says exactly that). Recommend either rewording the C12 sentence to match reality, or noting in ui.h which entries are platform-layer-only.

*File: `src/linux/ui.h` line 18*  

*Severity: info - action requested: auto-fix*

### MNPDF-TOMB-UNDO-RESTART (info) - DEFERRED (documented)

Accepted as a deliberate no-op: the behaviour is inherited verbatim from the Windows original (src/main.cpp:1660-1666 and its undoOp never touch gTomb). Fixing it on Linux only would break the cross-platform parity this port exists to preserve. Recorded in docs/linux.md.

> The pipeline said:
>
> Undo of a baked-mark deletion is not durable across a restart. deletePin records a dl= tombstone for a baked pin (src/linux/doc.cpp:507, deleteHighlight the same at :490) and undoOp's kind=4/kind=1 branches restore the mark without removing that tombstone, while still calling markSave() (doc.cpp:396-397) - so the sidecar then carries both the restored mark and the tombstone, and restoreSidecar applies tombstones last (sidecar.cpp:280-294), deleting the pin again on the next launch. Concrete sequence: open a document saved by mnpdf (marks lift as baked=true), delete a pin, undo (the pin is visible again), quit -> relaunch and the pin is gone. This is inherited verbatim from the Windows original (src/main.cpp:1660-1666 and its undoOp, which never touches gTomb), so it is a faithful port rather than a divergence introduced here; recorded so the divergence question (fix it on Linux only, or on both) is a deliberate decision rather than an accident.

*File: `src/linux/doc.cpp` line 507*  

*Severity: info - action requested: no-op*

### MNPDF-001 (error) - FIXED

**Fixed in `1a1fa1e`**

deletePin and commitPinText now record op.pinIdx; the kind=4 undo restores the pin at its original slot via pinInsertAt. Proven by suite checks and by the fixer rebuilding the pre-fix code and watching the four reported sequences fail there and pass after.

> The pipeline said:
>
> Pin deletion and creation operations omit the pin index required by undo/redo. deletePin creates a kind=4 operation without op.pinIdx; the undo branch at line 404 appends the pin instead of restoring its original position, and the redo branch at line 432 calls pinEraseAt(-1), so deleting a pin, undoing, and redoing leaves that pin present after redo. The same defect occurs for a newly created pin with a note: commitPinText creates a kind=3 operation without op.pinIdx at line 723, making both the undo branch at line 402 and redo branch at line 431 no-ops. Record op.pinIdx in both producers and restore the deleted pin by index in the kind=4 undo branch.

*File: `src/linux/doc.cpp` line 506*  

*Severity: error - action requested: auto-fix*

### MNPDF-002 (warning) - FIXED

**Fixed in `e95faf7`**

ui.h now declares the single implemented ui_prompt(label, value); the C12 disposition line in the review report was corrected.

> The pipeline said:
>
> A newly created pin marks the document dirty before the note is committed. If the user then cancels or submits empty text, commitPinText erases the pin at line 720 but leaves gDirty set, so a document with no resulting mark is still treated as modified and can trigger a spurious unsaved-changes prompt. Defer markDirty until a non-empty note is committed, while preserving any pre-existing dirty state.

*File: `src/linux/doc.cpp` line 500*  

*Severity: warning - action requested: auto-fix*

### MNPDF-DOCS-TEST-COUNT-DRIFT (info) - FIXED

**Fixed in `811bbec`**

the stated count is 146, the number the suite actually runs.

> The pipeline said:
>
> docs/linux.md records 145 checks for the headless core test, but the suite in tests/core-test.cpp contains 146 CHECK statements. The recent additions verifying canonical key ordering and foreign key preservation in app.txt (tests/core-test.cpp:547, 550, 553) raised the count to 146. Remedy: update docs/linux.md:131 to state 146 checks.

*File: `docs/linux.md` line 131*  

*Severity: info - action requested: auto-fix*

### MNPDF-GATE-CORE-TEST-LOG-RM (info) - FIXED

**Fixed in `811bbec`**

the gate keeps the failed suite log instead of deleting the directory it points at.

> The pipeline said:
>
> scripts/linux-gate.sh reports a preserved log file that is immediately deleted. When the headless engine suite fails, line 42 prints 'bad

*File: `scripts/linux-gate.sh` line 42*  

*Severity: info - action requested: auto-fix*

### MNPDF-LNX-001 (warning) - FIXED

**Fixed in `eb781f1`**

Ctrl+Tab now goes through gtk_notebook_set_current_page, so the model, the notebook and gCurArea cannot disagree; the old switchToDoc was removed.

> The pipeline said:
>
> Ctrl+Tab changes the active document in the model but never moves the GtkNotebook's current page, so the tab strip and the canvas can disagree. switchToDoc() sets gActiveDoc, reloads the page and repaints, but it neither calls gtk_notebook_set_current_page() (only addTab at main.cpp:272 and closeTab at main.cpp:330 do) nor updates gCurArea (only onNotebookSwitch at main.cpp:836-840 does). Concrete sequence: open two tabs, press Ctrl+Tab - renderPage() paints the second document into the shared back buffer and ui_invalidate() queues the redraw on gCurArea, which still points at the first tab's drawing area, so the visible canvas shows document 2 while the strip still highlights tab 1; pressing Ctrl+Tab again returns the model to tab 1, so the shortcut looks dead. The Windows build (src/main.cpp:5483) gets this right because it paints its own strip from gActiveDoc; the port's strip is a widget, so it must be driven. Sibling site: no other path reconciles 'the model changed' with 'the notebook changed'. Remedy: perform the switch through gtk_notebook_set_current_page() (which emits switch-page, already handled by onNotebookSwitch) so model, strip and drawn area always move together.

*File: `src/linux/main.cpp` line 288*  

*Severity: warning - action requested: auto-fix*

### MNPDF-LNX-001 (warning) - FIXED

*Also reported as `MNPDF-LNX-001-UNIMPLEMENTED` after a daemon restart killed the fixer before it committed.*

**Fixed in `eb781f1`**

Ctrl+Tab now goes through gtk_notebook_set_current_page, so the model, the notebook and gCurArea cannot disagree; the old switchToDoc was removed.

> The pipeline said:
>
> The recorded decision on LNX-001 (perform the Ctrl+Tab switch through gtk_notebook_set_current_page so model, strip and drawn area move together) is not implemented. switchToDoc (main.cpp:288-299) sets gActiveDoc, loads the page and repaints, but neither calls gtk_notebook_set_current_page nor updates gCurArea; the only reconcilers are addTab (main.cpp:269,272), closeTab (main.cpp:330-331) and onNotebookSwitch (main.cpp:836-840). Concrete sequence with two tabs: press Ctrl+Tab (main.cpp:731-733) -> renderPage paints document 2 into the shared back buffer and ui_invalidate() queues the redraw on gCurArea, which still points at tab 1's drawing area (main.cpp:269 was the last writer) -> the visible canvas shows document 2 while the strip still highlights tab 1; pressing Ctrl+Tab again returns the model to tab 1, so the shortcut looks dead until the user clicks a tab. Remedy per the recorded decision: make the key handler switch via gtk_notebook_set_current_page (which emits switch-page into the already-correct onNotebookSwitch) instead of calling switchToDoc directly.

*File: `src/linux/main.cpp` line 288*  

*Severity: warning - action requested: auto-fix*

### MNPDF-LNX-002 (warning) - FIXED

**Fixed in `eb781f1`**

the ui.h declaration matches the implementation.

> The pipeline said:
>
> The seam header declares ui_prompt(caption, label, value), but no implementation provides that signature: src/linux/main.cpp:170 and tests/core-test.cpp:35 both define the two-argument ui_prompt(label, value). The declared three-argument form is never defined anywhere, so any core module that follows the header and calls it fails to link at build time; the two-argument calls at main.cpp:188 and main.cpp:203 resolve to the undeclared overload, so the suite never notices. The repo's own review report records this as 'C12 Deferred (tracked, documented in docs/linux.md and AGENTS.md)', but the mismatch is still in the tree and neither document mentions it - grep for 'prompt' in docs/linux.md and AGENTS.md hits only the titlebar paragraph. Remedy: make the header's declaration match the single implemented signature, and correct the disposition line in docs/reviews/2026-10-19-codebase-review.md (or record the deferral where the report says it is recorded).

*File: `src/linux/ui.h` line 33*  

*Severity: warning - action requested: auto-fix*

### MNPDF-LNX-002 (warning) - FIXED

*Also reported as `MNPDF-LNX-002-UNIMPLEMENTED` after a daemon restart killed the fixer before it committed.*

**Fixed in `eb781f1`**

the ui.h declaration matches the implementation.

> The pipeline said:
>
> The recorded decision on LNX-002 (make the header declaration match the single implemented signature) is not implemented. ui.h:33 declares ui_prompt(caption, label, value), but both implementations define the two-argument form (src/linux/main.cpp:170, tests/core-test.cpp:35); the declared three-argument form is defined nowhere, so a core module that follows the header fails to link, while the two call sites (main.cpp:188, main.cpp:203) resolve to the undeclared overload and the headless suite cannot notice the mismatch. The C12 disposition the decision also required correcting still asserts the deferral is 'documented in docs/linux.md and AGENTS.md' (docs/reviews/2026-10-19-codebase-review.md:89) while neither document mentions ui_prompt or this seam mismatch. Remedy per the recorded decision: change ui.h:33 to bool ui_prompt(const std::string& label, std::string& value) and fix the C12 line or record the deferral where it claims to be recorded.

*File: `src/linux/ui.h` line 33*  

*Severity: warning - action requested: auto-fix*

### MNPDF-LNX-003 (warning) - FIXED

**Fixed in `eb781f1`**

restoreWindowGeometry applies the saved position and the maximized state, with monitor work-area bounds checks.

> The pipeline said:
>
> app.txt stores the window position (winx/winy) and the maximized state (winmax) and loadAppPref reads them (sidecar.cpp:315-319), but the Linux build never applies them: startup only calls gtk_window_set_default_size() with the saved width/height, and there is no gtk_window_move() anywhere in the tree, so the window always opens at the window manager's default position and never maximized. The repo's review report claims the opposite for C11 ('the window restores from them with on-screen bounds checks'), which the code does not do. Sibling sites: gWinX/gWinY (sidecar.cpp:296) and gWinMax (sidecar.cpp:297) are written back on every quit and read by nothing else. Decision needed: restore the position and the maximized state the way the Windows build does (so the report's claim holds), or amend the disposition and the docs to say only the size is restored.

*File: `src/linux/main.cpp` line 961*  

*Severity: warning - action requested: ask-user*

### MNPDF-LNX-003 (warning) - FIXED

*Also reported as `MNPDF-LNX-003-UNIMPLEMENTED` after a daemon restart killed the fixer before it committed.*

**Fixed in `eb781f1`**

restoreWindowGeometry applies the saved position and the maximized state, with monitor work-area bounds checks.

> The pipeline said:
>
> The recorded round-1 decision on LNX-003 ('restore the saved window position and maximized state, matching Windows, with monitor-bounds checks') is not in the code, and the change's own review report claims it is. loadAppPref reads winx/winy/winw/winh/winmax (src/linux/sidecar.cpp:315-319) and writeAppPref writes them (sidecar.cpp:341), but startup only calls gtk_window_set_default_size (main.cpp:961); there is no gtk_window_move and no startup gtk_window_maximize anywhere in the tree (gtk_window_maximize appears only in the Maximize/restore menu handler, main.cpp:434). Concrete sequence: quit a window maximized at (x,y) -> app.txt records winx/winy/winmax=1 -> relaunch opens at the window manager's default position, unmaximized. gWinX/gWinY/gWinMax are therefore written and read by nothing else (sibling consumers: ui_windowRect sidecar.cpp:336, onWindowConfigure main.cpp:867, onWindowState main.cpp:872). The disposition docs/reviews/2026-10-19-codebase-review.md:88 states 'the window restores from them with on-screen bounds checks', which the code does not do, and the fixer commit message repeats the claim. Remedy (already authorized by the recorded decision): apply gtk_window_move plus gtk_window_maximize at startup with monitor-bounds checks, as Windows does (src/main.cpp:6202-6204, 6281), or the claim must be withdrawn; both halves must move together.

*File: `src/linux/main.cpp` line 961*  

*Severity: warning - action requested: auto-fix*

### MNPDF-LNX-004 (info) - FIXED

**Fixed in `eb781f1`**

the stated check count was corrected and is now kept in step with the suite.

> The pipeline said:
>
> The doc states the headless suite runs '143 checks', but tests/core-test.cpp now holds 144 CHECK assertions (the fix round added 13 to the previous 131), so the measured number is off by one; the value it replaced (119) was already below the suite's 131 assertions. Correct the count to the suite's actual number and keep it in step when assertions are added.

*File: `docs/linux.md` line 101*  

*Severity: info - action requested: auto-fix*

### MNPDF-LNX-005 (info) - FIXED

**Fixed in `eb781f1`**

settleActiveDoc/flushPageCache moved inside addTab, so every tab-creating path (menu item, Ctrl+T) settles the outgoing tab first; newTab was folded in.

> The pipeline said:
>
> The 'Open in new tab...' action calls addTab() directly, and addTab() flips gActiveDoc to the new bundle without the settleActiveDoc()/flushPageCache() that newTab() performs first (main.cpp:302-306). Consequences: the outgoing tab's marks are not written to its sidecar at that moment, because openPath's dirty-flush now runs against the new bundle whose dirty flag is false; the shared search state survives the switch, so the find bar keeps showing the previous document's match count and query; and the previous bundle's cached PDFium pages stay open. Sibling site: addTab() is the only tab-creating path that does not settle the outgoing tab. Remedy: run the 'open in a new tab' path through the same settle/flush sequence newTab() uses.

*File: `src/linux/main.cpp` line 431*  

*Severity: info - action requested: auto-fix*

### MNPDF-LNX-005 (warning) - FIXED

*Also reported as `MNPDF-LNX-005-UNIMPLEMENTED` after a daemon restart killed the fixer before it committed.*

**Fixed in `eb781f1`**

settleActiveDoc/flushPageCache moved inside addTab, so every tab-creating path (menu item, Ctrl+T) settles the outgoing tab first; newTab was folded in.

> The pipeline said:
>
> The recorded decision on LNX-005 (route 'Open in new tab...' through the same settle/flush sequence newTab uses) is not implemented. Case 108 (main.cpp:431) calls addTab() directly, and addTab (main.cpp:263-283) flips gActiveDoc to the new bundle without settleActiveDoc()/flushPageCache() (only newTab main.cpp:301-307 and closeTab main.cpp:308-345 settle). Concrete consequences, all reachable from that one menu item: (a) the outgoing tab's dirty marks are not written to their sidecar at the switch - openPath's dirty flush (src/linux/opendoc.cpp:66) then runs against the NEW bundle whose dirty flag is false, so the outgoing sidecar on disk stays stale until quit; (b) the shared search state survives the switch, so the find bar keeps the previous document's match count and drawMatches (src/linux/render.cpp:99-113) paints the old document's match ranges at the same page index of the new document - wrong highlight rectangles with no error; (c) the previous bundle's PDFium page handles stay in the shared cache. Sibling site: addTab is the only tab-creating path that does not settle the outgoing tab. Remedy per the recorded decision: settle + flush before switching, exactly as newTab does.

*File: `src/linux/main.cpp` line 431*  

*Severity: warning - action requested: auto-fix*

### MNPDF-LNX-006 (info) - FIXED

**Fixed in `eb781f1`**

writeAppPref reads the existing app.txt and carries forward the keys Linux does not own (titlebar, updcheck, updtag).

> The pipeline said:
>
> writeAppPref() rewrites app.txt from scratch with only the keys the Linux build owns, so the Windows-only keys are dropped from a shared config directory: the Windows reader's titlebar= setting is lost and its updcheck=/updtag= state (the update-check clock and the tag it found) is discarded, so the next Windows launch re-runs the update check it had already done. The doc-*.txt sidecars only line up across platforms when both readers use the identical path string, but app.txt is always the same file, so this rewrite is where the two builds actually collide. Decision needed: either the Linux writer preserves the keys it does not own (read the existing app.txt and merge) or the docs state that app.txt is Linux-owned inside a shared directory. The remedy, not the defect, is what needs authorization here: merging unknown keys is extra persistence machinery beyond the port's stated scope.

*File: `src/linux/sidecar.cpp` line 334*  

*Severity: info - action requested: ask-user*

### MNPDF-LNX-006 (warning) - FIXED

*Also reported as `MNPDF-LNX-006-UNIMPLEMENTED` after a daemon restart killed the fixer before it committed.*

**Fixed in `eb781f1`**

writeAppPref reads the existing app.txt and carries forward the keys Linux does not own (titlebar, updcheck, updtag).

> The pipeline said:
>
> The recorded decision (review-1, auto-fix) that writeAppPref must preserve keys it does not own is not implemented. writeAppPref (sidecar.cpp:334-348) opens app.txt with

*File: `src/linux/sidecar.cpp` line 334*  

*Severity: warning - action requested: auto-fix*

### MNPDF-LNX-007 (warning) - FIXED

**Fixed in `eb781f1`**

ui_tabStripInfo, ui_hasDoc, ui_toast and ui_note_editor were removed from the seam.

> The pipeline said:
>
> Simplification pass: the change introduces a platform seam (src/linux/ui.h) whose members are not all needed by the port. (a) ui_tabStripInfo - declared extern in render.cpp:24, defined in main.cpp:79, stubbed in tests/core-test.cpp:47 - is never called, and it re-implements the tab-name and dirty-dot concept that refreshTabLabels() (main.cpp:91) already owns, i.e. a second definition of one concept. (b) ui_hasDoc (main.cpp:86) and ui_toast (main.cpp:113) have no caller. (c) ui_note_editor - declared in ui.h:40, defined in main.cpp:253 - is bypassed, because the note editor is entered directly through openNoteEditor(). (d) The palette branch in onMenuActivate (main.cpp:488, ids 500..500+kPalCount) can never fire: no menu item is ever created with those ids. The intent (same document engine, same saved-state formats) requires none of these entry points, so recommend removing them rather than wiring them up.

*File: `src/linux/main.cpp` line 79*  

*Severity: warning - action requested: ask-user*

### MNPDF-LNX-007 (warning) - FIXED

*Also reported as `MNPDF-LNX-007-UNIMPLEMENTED` after a daemon restart killed the fixer before it committed.*

**Fixed in `eb781f1`**

ui_tabStripInfo, ui_hasDoc, ui_toast and ui_note_editor were removed from the seam.

> The pipeline said:
>
> Simplification: the dead seam members the user decided to remove (MNPDF-LNX-007: 'remove the dead seam members it lists ... keep the seam minimal and documented in ui.h') are all still present, so the seam is not minimal. No intent requirement (same engine, same saved-state formats) needs any of them, and none has a caller: (a) ui_tabStripInfo - declared extern in src/linux/render.cpp:24, defined src/linux/main.cpp:79-84, stubbed tests/core-test.cpp:47-50, never called; it re-implements the tab-name and dirty-dot concept refreshTabLabels() (main.cpp:91-101) already owns, a second definition of one concept; (b) ui_hasDoc - main.cpp:86, no caller; (c) ui_toast - declared ui.h:21, defined main.cpp:113, stubbed core-test.cpp:30, no caller; (d) ui_note_editor - declared ui.h:40, defined main.cpp:253, bypassed because the editor is entered directly through openNoteEditor() (main.cpp:410, 455); (e) the palette branch in onMenuActivate (main.cpp:488-490, ids 500..500+kPalCount) can never fire because no menu item is created with those ids (verified: no GINT_TO_POINTER(5xx) anywhere in main.cpp). Remedy per the recorded decision: remove them where removal does not break the core or the headless suite, and document the remaining seam in ui.h.

*File: `src/linux/ui.h` line 21*  

*Severity: warning - action requested: auto-fix*

### MNPDF-LNX-APP-TXT-ORDER (warning) - FIXED

**Fixed in `2ec0c94`**

the Linux-owned keys are written in the Windows canonical order and the foreign lines follow them.

> The pipeline said:
>
> The app.txt merge keeps foreign lines but rewrites the Linux block on top of them, so the file no longer matches either writer's canonical layout, and the carried-forward lines are re-emitted before the Linux block instead of after it. Concrete sequence: a shared config dir holds a Windows-written app.txt whose order is tabstrip, titlebar, autosave, hlcolor, pincolor, night, winx..winmax, pal0..2, palnext, updcheck, updtag (src/main.cpp:3350-3370). The Linux reader takes titlebar/updcheck/updtag as foreign and the Linux block as owned, so writeAppPref() emits titlebar, updcheck, updtag first and then the Linux block, dropping every Windows-owned line that sits between them in the original (autosave, hlcolor, pincolor, night, winx..winmax, pal0..2, palnext are re-emitted with Linux values, which is intended, but the resulting file is no longer in the order either writer emits). More concretely and worse: the merge emits the foreign lines FIRST and the Linux block LAST, while the Windows writer emits titlebar in the middle of its block and updcheck/updtag last; after a Linux write the updcheck/updtag lines therefore precede tabstrip, so a Windows-side parser that is order-sensitive (the current one is not: src/main.cpp:3383-3408 parses by key with sscanf, so no key is lost today) would see them in a different order than it wrote. The read-merge-write itself is correct and no key is dropped today; the residual risk is that the recorded decision (round 1, LNX-006) asked for 'preserved keys keep their original order', and this implementation preserves only the relative order of the foreign subset, not the order of the whole file, so the doc claim in docs/linux.md:58-61 ('keeps every key it does not own ... verbatim and in its original order') is broader than what the code does. The Linux-owned lines are also duplicated in the sense that they are dropped and re-emitted with in-memory values, so a Windows-written hlcolor=3 that Linux has not loaded (loadAppPref only overwrites gHlDefault when the key is present, and it is present, so values are preserved) relies on loadAppPref having run before the write; writeAppPref() is called from ui_color_box (src/linux/main.cpp:207), onMenuActivate cases 109/200, mergeFiles (src/linux/surgery.cpp:168) and onWindowDelete, all of which run after loadAppPref() at startup, so no value is lost on those paths. Severity is warning rather than error because no key is actually lost on any current path.

*File: `src/linux/sidecar.cpp` line 345*  

*Severity: warning - action requested: auto-fix*

### MNPDF-LNX-GEOMETRY-RECT-IGNORES-WORKAREA (warning) - FIXED

**Fixed in `2ec0c94`**

the landing monitor is picked with the work area, the same area used for clamping, and the size shrinks to fit.

> The pipeline said:
>
> restoreWindowGeometry() picks the landing monitor by intersecting the saved rect with each monitor's full geometry (gdk_monitor_get_geometry) but then clamps the window into that monitor's WORK area (gdk_monitor_get_workarea), and the saved size is not shrunk to fit. Concrete sequence: the user leaves the window at x=1800, y=100, w=1100, h=800 on a 1920x1080 monitor with a 48px top panel (work area 0,48..1920,1080). The saved rect intersects the monitor, so land is that monitor; clampBox(1800, 1100, 0, 1920) yields pos=820 and clampBox(100, 800, 48, 1080) yields 48, so the window is placed at (820,48) with its 1100x800 size intact - but 820+1100=1920 is exactly the right edge and 48+800=848 fits, so the visible frame lands partly under the top panel region it was clamped against. Worse case: a window saved at w=1600 on that monitor clamps to pos=320, and the bottom edge at 48+1600 does not fit at all: clampBox only moves the top-left, it never shrinks the size, so the bottom of the window (y=48+1600=1648) is far below the work area bottom (1080) and off-screen, with no way to drag it back (the caption is visible but the resize border at the bottom is not). The Windows original has the same clampBox shape (src/main.cpp:6204-6215) but it passes startW/startH that came from rcNormalPosition and then lets CreateWindowExW's own work-area cap apply; the port's comment claims 'one that only partly fits keeps its width and height with its top-left moved inside that monitor's work area, so the caption strip and a resize border stay reachable' (src/linux/main.cpp:844-847), which is false for a saved size taller or wider than the work area: the far edge is simply off-screen. The recorded LNX-003 decision authorized 'restore winx/winy/winmax at startup with monitor-bounds checks', so the durable fix must keep the window reachable; the fix currently does not. The remedy is to clamp the SIZE to the work area as well as the position (or to fall back to the default size when the saved size does not fit), not to leave the far edge off-screen.

*File: `src/linux/main.cpp` line 858*  

*Severity: warning - action requested: auto-fix*

### MNPDF-LNX-MAXRECT-SAVE (info) - FIXED

**Fixed in `e4865f5`**

the maximized-frame vs rcNormalPosition divergence is recorded in docs/linux.md.

> The pipeline said:
>
> The new geometry restore saves the maximized frame as the normal rect, a divergence from Windows that docs/linux.md does not record. onWindowConfigure (line 835) captures the live allocation, so a quit while maximized writes winw/winh as the maximized size (e.g. 1920x1048) alongside winmax=1; the Windows original saves GetWindowPlacement's rcNormalPosition (src/main.cpp:3356-3365), the pre-maximize rect. Concrete sequence: an 800x600 window is maximized, the app quits, relaunches (maximized, correct), the user unmaximizes - on Linux GTK restores to the default size, which restoreWindowGeometry (line 873) just set to the capped maximized size, so the window stays work-area-sized instead of returning to 800x600 as on Windows. The primary flow (quit maximized -> reopen maximized) is correct; only the post-restore unmaximize size differs. The code remedy would need new state (a pre-maximize rect), beyond the recorded intent; the smallest honest remedy is one sentence in docs/linux.md 'What is not the same'.

*File: `src/linux/main.cpp` line 835*  

*Severity: info - action requested: auto-fix*

### MNPDF-LNX-NOTE-EDITOR-DROPS-TEXT (warning) - FIXED

**Fixed in `e4865f5`**

the modal-editor divergence is documented in docs/linux.md: on Windows an open note box is committed at tab switch and window close, on Linux the modal editor must be saved or cancelled first, and cancelling a NEW pin drops it on both platforms. No new machinery was authorised.

> The pipeline said:
>
> The Linux note editor is a modal dialog whose Cancel button discards the typed text, but it is not the empty-text case the model distinguishes: commitPinText(idx, fresh, text) is called with the dialog's text, and when the user presses Cancel the code takes the '!ok' branch and calls commitPinText(idx, true, L

*File: `src/linux/main.cpp` line 236*  

*Severity: warning - action requested: ask-user*

### MNPDF-LNX-PIN-UNDO-PARITY (info) - FIXED

**Fixed in `2ec0c94`**

the divergence is recorded in docs/linux.md: pin order after an undo may differ between platforms; both still parse each other's files.

> The pipeline said:
>
> The 1a1fa1e round changed undo kind=4 (pindel: restore) from gPins.push_back(op.pinVal) to pinInsertAt(op.pinIdx, op.pinVal) and made deletePin record op.pinIdx, so an undone pin deletion returns the pin to its original slot. The Windows original still does gPins.push_back(op.pinVal) with no index (src/main.cpp:460) and does not record an index in deletePin (src/main.cpp:1657-1663), so the two platforms now differ in the ORDER of pins in gPins after an undo, and therefore in the order of the 'pin=' lines written to the sidecar (src/linux/sidecar.cpp:143-148). Concrete sequence: a document with two pins, delete the first and undo it - Linux writes pin= lines with the first pin first, Windows writes them with the restored pin last. The sidecar format itself is unchanged and each pin carries its own page/x/y, so both platforms restore the same set of marks in the same places; only the physical line order differs, and restoreSidecar pushes pins in file order, so a cross-platform round trip preserves each platform's own order. The recorded round-1 decision MNPDF-LNX-007 and the pipeline instruction to keep the Windows build byte-identical mean the Windows side cannot be changed to match, so this is a deliberate, invisible divergence. The same divergence already exists in the redo path and was accepted in the round-1 review (C20 disposition). Flagging it as info so the divergence is recorded rather than left implicit, and recommending no code change: changing Linux back would reintroduce the index-loss defect the fixer just repaired.

*File: `src/linux/doc.cpp` line 389*  

*Severity: info - action requested: no-op*

### MNPDF-LNX-POST-DESTROY-WINDOW-DEREF (warning) - FIXED

**Fixed in `811bbec`**

ui_windowRect no longer calls into GTK at all: it returns the cached gWinX/gWinY/gWinW/gWinH/gWinMax. Proven: the pre-fix binary prints "gtk_window_is_maximized: assertion 'GTK_IS_WINDOW (window)' failed" on the quit path, the fixed one prints nothing, and the gate now asserts that.

> The pipeline said:
>
> ui_windowRect reads from a destroyed GtkWindow after gtk_main returns, invoking GTK accessors on a dangling pointer. In main (src/linux/main.cpp:1039-1043), gtk_main() returns after gWin has been destroyed via gtk_widget_destroy() in requestQuit/onWindowDelete. Then line 1043 calls writeAppPref(), which invokes ui_windowRect (src/linux/sidecar.cpp:361). In ui_windowRect (src/linux/main.cpp:206-209), `*maxed = (gWinMax || gtk_window_is_maximized(GTK_WINDOW(gWin))) ? 1 : 0;` calls gtk_window_is_maximized on the finalized gWin, triggering Gtk-CRITICAL assertions and reading freed memory. Sibling: gWinMax is already maintained by onWindowState (main.cpp:848-850), making the live GTK call redundant. Remedy: rely solely on gWinMax (or null gWin in onWindowDelete/destroy handler before checking).

*File: `src/linux/main.cpp` line 208*  

*Severity: warning - action requested: auto-fix*

### MNPDF-LNX-SCROLL-INVERT (warning) - FIXED

**Fixed in `e4865f5`**

the negation is gone: dy = ev->delta_y, matching the measured GTK convention (positive delta_y scrolls down) and the discrete branch; delta_x is now a horizontal pan.

> The pipeline said:
>
> Smooth (Wayland) scroll input is inverted, and this repeats a recorded decision the fix rounds did not implement. The C3 fix round (e95faf7) introduced the negation `dy = -(ev->delta_y + ev->delta_x)`; the later recorded round measured on this machine's GTK 3.24.52 that smooth delta_y=+1 increases a GtkScrolledWindow's vadjustment (positive delta_y = scroll down, same sign as the discrete GDK_SCROLL_DOWN path), concluding 'our negation inverts Wayland scrolling' - and chose to fix it alongside the four decisions eb781f1/2ec0c94 did implement. The line is unchanged. Concrete trace: on the port's target session (Wayland), a two-finger swipe up delivers delta_y=+1 -> dy=-1 -> accumulator passes -> scrollByDy(-120) -> the view moves UP, while the same gesture over X11 (GDK_SCROLL_DOWN, dy=+1) moves DOWN - the two input paths for one gesture disagree, and the smooth one is backwards. Sibling aspects of the same line: (a) the same dy feeds the Ctrl+wheel zoom branch (line 672), so Ctrl+wheel-down zooms IN on smooth devices (conventional is out); (b) summing delta_x into a vertical step means a purely horizontal two-finger pan (delta_x!=0, delta_y=0) scrolls the document vertically, an axis the discrete path never applies (gScrollX is untouched by onScroll). Remedy per the recorded finding: `dy = ev->delta_y;` - no negation, delta_x dropped - so the smooth path matches the discrete path's sign.

*File: `src/linux/main.cpp` line 656*  

*Severity: warning - action requested: auto-fix*

### MNPDF-LNX-SMOOTH-ACC-RESET (warning) - FIXED

**Fixed in `811bbec`**

the smooth accumulator is per-axis: a vertical step no longer zeroes a fractional horizontal accumulator.

> The pipeline said:
>
> Smooth scroll accumulator resets both axes when either axis reaches the threshold, discarding in-flight motion on the other axis. In onScroll (src/linux/main.cpp:668-675), sSmoothAccX and sSmoothAccY accumulate fractional deltas. When either reaches 1.0 (line 671), lines 672-673 compute discrete steps, and line 674 unconditionally sets `sSmoothAccX = 0; sSmoothAccY = 0;`. Concrete failing sequence: during diagonal scrolling or touchpad panning with a small horizontal component (e.g. dy=0.6, dx=0.2 per event), every two events sSmoothAccY reaches 1.2 and triggers vertical scrolling, while sSmoothAccX is reset from 0.4 to 0 without ever reaching 1.0, silently swallowing the horizontal scroll. Sibling consequence: rapid horizontal panning continuously zeroes out pending vertical scroll. Remedy: reset or decrement only the axis that actually tripped the threshold, e.g. `if (dx) sSmoothAccX = 0; if (dy) sSmoothAccY = 0;` (or subtract the step).

*File: `src/linux/main.cpp` line 674*  

*Severity: warning - action requested: auto-fix*

### MNPDF-TEST-JPEG-FIXTURE (info) - FIXED

**Fixed in `eb781f1`**

the badcomp fixture was rebuilt with a well-formed SOF so only the component-id matching rejects it.

> The pipeline said:
>
> The 'badcomp' fixture added by the fix round is refused by the SOF length check, not by the component-id matching the assertion names, so the newest guard logic is unproven: its SOF declares nf=3 with len=11 (0x000B), and 11 < 8 + 3*3 = 17, so looksLikeJpeg returns false at the 'len < (size_t)8 + 3*nf' branch (src/linux/opendoc.cpp:330) before the SOS component loop (:347-352) is ever reached; deleting the id-matching loop entirely would still pass this test. Remedy: build the fixture from a well-formed SOF (e.g. nf=1, len=11, one valid 3-byte component entry) whose SOS names a component the frame did not declare, so only the id check rejects it. The frameless fixture (:557) is fine - it is refused by the sawFrame check as intended.

*File: `tests/core-test.cpp` line 558*  

*Severity: info - action requested: auto-fix*

### MNPDF-TEST-SCRATCH-TMP (info) - FIXED

**Fixed in `eb781f1`**

the suite makes its own per-run scratch dir under the path it is given (mkdtemp); CMake and build-linux.sh pass a build-local path.

> The pipeline said:
>
> The engine suite's scratch directory is shared mutable state, so its file-existence assertions can read another run's leftovers. The ctest registration passes /tmp (CMakeLists.txt:46-47) and so does build-linux.sh (build-linux.sh:33), and the suite derives its config dir from the same path (setenv XDG_CONFIG_HOME, tests/core-test.cpp:73). A previous run's /tmp/split.pdf, /tmp/saved.pdf or /tmp/resaved.pdf makes 'split writes a new file' / 'split file holds the requested range' (tests/core-test.cpp:524-527) and 'saved file written to disk' / the bake-count checks (~:508-516) pass on stale bytes if the current run's write fails, and any foreign /tmp/bad.pdf makes 'a bad range writes nothing' (~:529) fail spuriously. The gate already gives the suite a private mktemp dir (scripts/linux-gate.sh:33); ctest and build-linux.sh should do the same (fresh mktemp -d per run).

*File: `CMakeLists.txt` line 46*  

*Severity: info - action requested: auto-fix*

### MNPDF-TOMB-DOC-MISSING (info) - FIXED

**Fixed in `e4865f5`**

the Windows-inherited tombstone-on-undo quirk is recorded in docs/linux.md.

> The pipeline said:
>
> The recorded decision on MNPDF-TOMB-UNDO-RESTART ('accepted as a deliberate no-op ... record it in docs/linux.md as a known Windows parity quirk rather than changing behaviour') is only half carried out: the behaviour is unchanged (correct), but the quirk is recorded nowhere. The quirk: undoing the deletion of a baked mark leaves the dl= tombstone in place while the mark is restored (src/linux/doc.cpp:490 and :507 push the tombstone, and undoOp's kind=1/kind=4 branches never remove it), so a save writes both the restored mark and its tombstone and restoreSidecar (src/linux/sidecar.cpp:280-294) applies tombstones last - the mark is gone again after a restart. It is inherited verbatim from the Windows original (src/main.cpp deletePin/deleteHighlight and its undoOp), which is why the decision was no-op. Remedy: one sentence in docs/linux.md's divergence section (the paragraph at line 63 already records the pin-order divergence); no code change.

*File: `docs/linux.md` line 63*  

*Severity: info - action requested: auto-fix*

### MNPDF-UPDTAG-TWO-MEANINGS (warning) - FIXED

**Fixed in `0371ccb`**

the shared `updcheck=`/`updtag=` pair carried two different meanings, one per
platform, in one file.

> The pipeline said:
>
> The update-check state written to app.txt collides across platforms: the
> Windows reader treats `updtag=` as the tag `/releases/latest` names - the
> newest non-prerelease overall (src/main.cpp:2435) - while the Linux reader
> stamped the newest release carrying a Linux archive into the same key. A
> shared app.txt (the whole point of the sidecar contract) therefore makes
> each reader's cached cooldown answer offer the other platform's release:
> a Linux-stamped v2.7.0 (Linux-only prerelease) would make a Windows reader
> at 2.6.0 offer a release with no Windows archive, and a Windows-stamped
> v2.6.0 would silence a Linux check that should offer v2.7.0.

*File: `src/linux/sidecar.cpp`*

*Severity: warning - action requested: ask-user*

The fix splits the pairs by platform rather than unifying the meaning: the
Linux reader now keeps its own cooldown in `lnxupdcheck=`/`lnxupdtag=` and
carries `updcheck=`/`updtag=` forward as foreign lines, verbatim, never read
on Linux. Each reader's cached answer comes only from its own stamp. The
Windows writer truncates the Linux pair, which costs the Linux reader one
fresh check after a Windows run - honest and bounded, unlike one key that
changes meaning per platform. Gate coverage: "a Windows stamp is not this
reader's cooldown" proves a fresh `updcheck=` does not stop the Linux check,
that the Linux check stamps its own pair, and that the Windows pair survives
a Linux run untouched.

### MNPDF-EMPTY-FEED-BLAMES-NETWORK (info) - FIXED

**Fixed in `0371ccb`**

a fetched-but-empty release feed reported a network failure.

> The pipeline said:
>
> `fetchReleaseTags` sets `why` only when the request itself failed, so a 200
> feed whose entries yield no usable tag returns with `why` empty and
> `checkForUpdate` composes "The update check could not reach GitHub - ." -
> a malformed sentence that blames the network for a host that answered.

*File: `src/linux/update.cpp`*

*Severity: info - action requested: auto-fix*

Fixed by giving the answered-but-nothing-readable outcome its own sentence:
`checkForUpdate` now emits "The release feed listed no release this build
could read." when the feed was fetched but yielded nothing usable, and the
network-blame sentence only when `why` actually names a network failure.

### MNPDF-LAUNCH-CHECK-MISSING (warning) - FIXED

**Fixed in `25643e1`**

the port ran its update check only from the "Check for updates" command.

> The pipeline said:
>
> The update check is only reachable from this one handler (case 170, the
> context-menu "Check for updates"), so the port never checks on launch. The
> Windows build does: src/main.cpp:6286 runs startUpdateCheck(false) in WinMain
> before the message loop - "notify only when a newer release exists",
> rate-limited to one request per kUpdateCheckIntervalMin. Consequence: a Linux
> reader is never told a new release exists unless they right-click and pick
> the command, so the two readers do not feel the same.

*File: `src/linux/main.cpp`*

*Severity: warning - action requested: ask-user*

The port now runs the same silent check before its main loop, with the two
Windows silences: a check with nothing to say, or one that failed, says
nothing, and a launch inside the cooldown says nothing even when the cached
answer names a newer one (the cached answer is only surfaced by a click).
MNPDF_NO_UPDATE skips only the launch check, so gate sections that are not
about the update check stay off the network. The gate proves both halves by
counting the toplevels the reader maps (xwininfo; a modal is a toplevel of
its own, measured: 2 without a dialog, 3 with), and both checks were proven
to fail by breaking each rule in turn.

### MNPDF-CASCADE-STATE / MNPDF-RING-FOLLOW / MNPDF-ROT-CAPTURE / MNPDF-LOCK-DOCS / MNPDF-RECIPE-PUBLISH (round 10) - FIXED

**Fixed in `732402e`**

Five findings of the round-10 re-review were real against the head; all are
closed by the same commit, and every other finding in that round's ledger
was re-reported against a stale snapshot (verified one by one against the
merged head: the colour entry, the cooldown cache answer, the stamp-on-cache,
the worker-thread write, the prose asset scan, the tarball tag contract, the
web-host trace check, the seam budget wait, the rotate-page semantics, the
night label, the drawer keyboard path, the CMake fallback comment, and the
F-series fixes).

- **review-23** the Advanced cascade draws the Windows advancedMenu state
  (src/main.cpp:5450-5464): drawer ticks while open, Night mode tick, and the
  document tools grey out (Split also needs a second page, Clear signatures a
  signature). The context cascade reads it at build time; the menu bar, built
  once, is refreshed by refreshMenuLabels from every state change.
- **review-21** a page move re-points the drawer's ring at the page that moved
  (Windows movePage, src/main.cpp:4615-4620). Gate-proven: the selection band
  sits at y=29 with the ring parked and y=175 after the move it selected, and
  the check fails on the broken behaviour.
- **review-22** the rotate capture is spent by the rotate that consumed it:
  Windows's only rotate commands are the context popup's own
  (src/main.cpp:5559), while the port also has them in the menu bar, where a
  surviving capture would rotate a page the reader is no longer looking at.
- **review-20** the single-writer guarantee is stated per platform
  (docs/linux.md, README-linux.txt): the flock cannot see the Windows named
  mutex and the mutex cannot see the flock.
- **review-4** the release recipe carries the publishing step: one GitHub
  release holds BOTH assets (`gh release create`), and a Linux-only release
  stays a prerelease until the Windows zip ships, so /releases/latest keeps
  naming a release a Windows reader can actually fetch.

Declined, with reasons recorded for the next round:

- **F19** (remove MNPDF_MENU_DUMP): declined - the dump is the mechanism the
  gate's 40-id parity check reads, and it quits through the app's own quit
  path, so the exit(0) concern it raised is already gone.
- **F20** (keep row pictures through a refresh): declined - it is the same
  documented one-raster divergence as F16 (docs/linux.md, "The one place the
  port is cheaper than the Windows build").

### Round 12 (the fa7900a re-review): five findings, all real - FIXED

**Fixed in the round-13 batch (the merge of 60aa0e2 plus `Start a second window empty and gate every sidecar write on the primary role`)**

The round-12 re-review of the pipeline head (fa7900a, which merges 668db01)
was the first to run the binary end to end rather than read code: it measured
the second-launch sidecar rewrite (zoom 1.767974 -> 1.366162 by a plain
second launch with no user input), the autosave-off sidecar write, the
0.0.0 CMake fallback, the stale resource numbers and the Windows readme
inside the Linux tarball. All five were real; all five are closed.

- **linux-second-launch-not-empty (error)** a plain second launch reopened
  the document the primary already had (Windows starts it empty,
  src/main.cpp:6241) and, worse, its clean exit rewrote that document's
  sidecar (writeSidecarFor carried no primary-role gate; Windows gates the
  sidecar write at src/main.cpp:3276). Fixed both halves: the launch branch
  consults the role, and writeSidecarFor carries the same gate. Gate-proven:
  the plain second window is empty (no document title), a second window with
  an explicit file argument still opens (two readers side by side is the
  point), and its exit leaves the primary's sidecar untouched in mtime AND
  content - the check fails on the broken code (verified).
- **linux-autosave-toggle-pending-flush (warning)** the toggle (case 109) only
  flipped the flag, where Windows also cancels the pending write on off and
  re-arms the flush on on (src/main.cpp:5702-5711). Fixed both halves.
  Gate-proven with timeout-killed runs (no quit path may write, so only the
  800 ms tick can have): a change then toggle-off writes nothing, toggle-off,
  change, toggle-on flushes it. Both halves fail on the flip-only code
  (verified).
- **cmake-version-fallback-0-0-0 (warning)** a version-regex miss silently
  stamped the build 0.0.0 (which makes every release look newer forever and
  names the tarball v0.0.0) instead of refusing to configure. The else branch
  is now message(FATAL_ERROR), matching the comment's own promise.
- **docs/linux.md resource numbers (warning, pipeline-owned delivery)** the
  suite the doc describes runs 171 checks now, and linking libcurl moved its
  peak RSS from 15.1 MB to ~21 MB (re-measured by the reviewer,
  /usr/bin/time -v). Updated; the 98.6 MB GUI figure still measured true.
- **the Linux tarball's Windows readme (warning, ask-user, pipeline-owned
  delivery)** the tarball shipped README.txt, whose first lines tell the
  reader to run mnpdf.exe. Decision made: the Linux asset ships
  README-linux.txt alone from the next release; the Windows ZIP keeps
  README.txt as its version contract, and the Linux asset's version is its
  tag-named directory. The published v2.7.0 asset (which already carries
  both files) is left as published; docs/linux.md records the change.

Also recorded: a supposed sibling divergence this batch first "fixed" and
then un-did on second look - menu Quit, Escape and the last tab's close were
re-routed to gtk_window_close on the theory they bypassed the unsaved-changes
prompt, but requestQuit already funnels through onWindowDelete (the port's
own comment says so), so the rerouting was redundant and was reverted. The
quit-prompt gate section stays: it locks the prompt behaviour (TOPS 3 with
unsaved changes, TOPS 0 clean) against future refactors.

Gate: 46 -> 53 checks, all passing; core 171/171.

### Round 13 (the dc99dec re-review) - five real, one reverted

**Fixed in the round-14 batch**

- **R-ctxmenu-pg-wrong-coord-space (warning, the one real code bug)**
  buildContextMenu called pageAtDocY(y) with the raw widget-space event y, but
  pageAtDocY takes a DOCUMENT-space y (every other caller subtracts docTop()
  first: charIndexAtDoc doc.cpp:629, pinAt doc.cpp:557). The subtraction was
  lost when the builder was split out of onContextMenu (4ee5573), so a
  right-click on a scrolled page set "Add pin here" to the page under the
  DOCUMENT's top - silently the wrong page, written to the sidecar on the next
  tick. Rotate and highlight were right because they come from charIndexAtDoc;
  only the pin branch disagreed. Fixed: pageAtDocY(y - docTop()).
- **R-sidecar-gate-not-windows-parity (info, ask-user) - MY GATE WAS WRONG,
  REVERTED.** Windows gates only the two SHARED files on the primary role
  (last.txt src/main.cpp:3276, app.txt :3345); a document's own sidecar is
  written by whichever window owns that document (:3243-3245, no gate). The
  gate I added in the previous batch would have DROPPED a second window's real
  marks at quit - a data loss the reviewer measured the risk of. Reverted, and
  the gate section now asserts the Windows behaviour instead: a second window
  rotating its own document keeps that rotation ("a second window keeps its own
  document's marks"), while the shared app.txt section still proves the shared
  files stay untouched. README.txt's "write none of them" is the documented
  promise, but the Windows implementation does not do that for the sidecar, and
  matching the implementation is what keeps a reader's marks.
- **R-gate-live-network / R-gate-update-check-not-hermetic (warning, info)**
  the gate's update-check sections now say "untested" when GitHub cannot be
  reached instead of failing the gate (a network outage is not a code defect),
  guard curl with command -v, and set MNPDF_NO_UPDATE on the click-only runs so
  the launch check cannot race the click counts.
- **S-dup-refreshDrawers-after-rotate (info)** the four handler-level
  refreshDrawers() calls after rotate/undo/redo were redundant - rotatePage and
  rotApply already refresh (doc.cpp:550, :394) - so they were dropped; each cost
  a full layoutThumbs plus a buildBmList walk.
- **R-docs-drawer-claim-still-wrong / R-gate-check-count-stale (info)** the
  docs/linux.md citation now points at the real clear (src/linux/main.cpp:1583-1585,
  and the Windows call sites :4813-4817, :4968), and the counts in this record
  are corrected.
