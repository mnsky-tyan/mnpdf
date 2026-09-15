// mnpdf — minimal local PDF reader/annotator. Wiring only; logic lives in modules.
import './style.css';
import platform from './platform.js';
import { S, onDocChange, undo as undoOp, redo as redoOp } from './state.js';
import * as viewer from './viewer.js';
import * as annos from './annos.js';
import * as search from './search.js';
import * as thumbs from './thumbs.js';
import { showMenu, closeMenu, init as initMenu } from './menu.js';
import * as cmd from './commands.js';
import { toast } from './util.js';

// ---------- context menus ----------

function windowItems() {
  if (platform.kind !== 'tauri') return [];
  return [
    'sep',
    { label: 'Minimize', fn: () => platform.minimize() },
    { label: 'Maximize / Restore', fn: () => platform.toggleMaximize() },
    { label: 'Quit', danger: true, fn: () => platform.closeWindow() },
  ];
}

function menuGeneral(e) {
  showMenu(e.clientX, e.clientY, [
    { label: 'Open…', hint: 'Ctrl+O', fn: cmd.openCmd },
    ...(S.pdf
      ? [
          { label: 'Save', hint: 'Ctrl+S', fn: cmd.doSave },
          { label: 'Save As…', hint: 'Ctrl+Shift+S', fn: cmd.doSaveAs },
          'sep',
          { label: 'Undo', hint: 'Ctrl+Z', fn: cmd.undoCmd },
          { label: 'Redo', hint: 'Ctrl+Y', fn: cmd.redoCmd },
          'sep',
          { label: 'Go to page…', hint: 'Ctrl+G', fn: cmd.gotoPrompt },
          { label: 'Find…', hint: 'Ctrl+F', fn: () => search.open() },
          { label: 'Thumbnails', hint: 'F9', fn: () => thumbs.toggle() },
          'sep',
          { label: 'Zoom in', hint: '+', fn: cmd.zoomIn },
          { label: 'Zoom out', hint: '−', fn: cmd.zoomOut },
          { label: 'Zoom to…', fn: () => viewer.pillGotoMode('zoom') },
          { label: 'Fit width', hint: 'Ctrl+0', fn: cmd.zoomFit },
        ]
      : []),
    ...windowItems(),
  ]);
}

function menuForSelection(e, info) {
  showMenu(e.clientX, e.clientY, [
    { label: 'Highlight', swatches: annos.HL_COLORS, fn: (c) => annos.addHighlight(info, c) },
    { label: 'Copy text', hint: 'Ctrl+C', fn: () => { annos.copyText(info.text); toast('Copied'); } },
  ]);
}

function menuForPage(e, wrapEl) {
  const i = +wrapEl.dataset.i;
  const wr = wrapEl.getBoundingClientRect();
  const vx = e.clientX - wr.left;
  const vy = e.clientY - wr.top;
  const hl = annos.highlightAt(i, vx, vy);
  const items = [];
  if (hl) items.push({ label: 'Delete highlight', danger: true, fn: () => annos.deleteAnn(hl.id) });
  items.push(
    { label: 'Add pin here', fn: () => annos.startNewPin(i, vx, vy) },
    'sep',
    { label: 'Undo', hint: 'Ctrl+Z', fn: cmd.undoCmd },
    { label: 'Redo', hint: 'Ctrl+Y', fn: cmd.redoCmd },
    'sep',
    { label: 'Rotate clockwise', fn: () => cmd.rotatePage(i, 90) },
    { label: 'Rotate counter-clockwise', fn: () => cmd.rotatePage(i, -90) },
    { label: 'Delete page', danger: true, fn: () => cmd.deletePage(i) },
    'sep',
    { label: 'Zoom in', fn: cmd.zoomIn },
    { label: 'Zoom out', fn: cmd.zoomOut },
    { label: 'Zoom to…', fn: () => viewer.pillGotoMode('zoom') },
    { label: 'Fit width', fn: cmd.zoomFit },
    'sep',
    { label: 'Find…', hint: 'Ctrl+F', fn: () => search.open() },
    { label: 'Thumbnails', hint: 'F9', fn: () => thumbs.toggle() },
    ...windowItems(),
  );
  showMenu(e.clientX, e.clientY, items);
}

function menuForPin(e, pinEl) {
  const a = S.anns.find((x) => x.id === pinEl.dataset.id);
  if (!a) return;
  showMenu(e.clientX, e.clientY, [
    { label: 'Edit text', fn: () => annos.startPinEdit(a) },
    { label: 'Delete pin', danger: true, fn: () => annos.deleteAnn(a.id) },
  ]);
}

function initContextMenu() {
  window.addEventListener('contextmenu', (e) => {
    e.preventDefault();
    const pinEl = e.target.closest?.('.pin');
    if (pinEl) return menuForPin(e, pinEl);
    const thumbEl = e.target.closest?.('.thumb');
    if (thumbEl) {
      return showMenu(e.clientX, e.clientY, thumbs.menuItems(thumbEl, {
        rotate: cmd.rotatePage,
        del: cmd.deletePage,
      }));
    }
    const sel = annos.preferredSelection();
    if (sel) return menuForSelection(e, sel);
    const wrapEl = e.target.closest?.('.pagewrap');
    if (wrapEl && S.pdf) return menuForPage(e, wrapEl);
    menuGeneral(e);
  });
}

// ---------- keyboard ----------

function isTyping(e) {
  return !!e.target.closest?.('input, textarea, [contenteditable="true"]');
}

function initKeyboard() {
  window.addEventListener('keydown', (e) => {
    if (isTyping(e)) return;
    const c = e.ctrlKey || e.metaKey;
    const k = e.key;
    if (c && k === 'o') { e.preventDefault(); cmd.openCmd(); }
    else if (c && k.toLowerCase() === 's') { e.preventDefault(); e.shiftKey ? cmd.doSaveAs() : cmd.doSave(); }
    else if (c && k.toLowerCase() === 'z') { e.preventDefault(); e.shiftKey ? cmd.redoCmd() : cmd.undoCmd(); }
    else if (c && k.toLowerCase() === 'y') { e.preventDefault(); cmd.redoCmd(); }
    else if (c && k.toLowerCase() === 'f') { e.preventDefault(); search.open(); }
    else if (c && k.toLowerCase() === 'g') { e.preventDefault(); cmd.gotoPrompt(); }
    else if (c && k === '0') { e.preventDefault(); cmd.zoomFit(); }
    else if (c && (k === '=' || k === '+')) { e.preventDefault(); cmd.zoomIn(); }
    else if (c && k === '-') { e.preventDefault(); cmd.zoomOut(); }
    else if (k === 'F3') { e.preventDefault(); if (search.isOpen()) search.nav(e.shiftKey ? -1 : 1); else search.open(); }
    else if (k === 'F9') { e.preventDefault(); thumbs.toggle(); }
    else if (k === '/') { e.preventDefault(); search.open(); }
    else if (k === '+' || k === '=') { e.preventDefault(); cmd.zoomIn(); }
    else if (k === '-') { e.preventDefault(); cmd.zoomOut(); }
    else if (k === '0') { e.preventDefault(); cmd.zoomReset(); }
    else if (k === 'Escape') {
      if (search.isOpen()) search.close();
      else if (thumbs.visible()) thumbs.toggle(false);
      else window.getSelection()?.removeAllRanges();
    }
  });
}

// ---------- doc changes ----------

function watchDocChanges() {
  onDocChange((what) => {
    if (what?.kind === 'pages' || what?.undid?.kind === 'pages' || what?.redid?.kind === 'pages') {
      const cur = viewer.currentPageIdx();
      viewer.buildPages();
      viewer.goToDisplayPage(Math.min(cur, viewer.wrapCount() - 1));
    }
    viewer.updatePill(true);
    cmd.updateTitle();
    cmd.persistDocSoon();
  });
}

// ---------- lifecycle ----------

async function initCloseGuard() {
  platform.onCloseGuard(async (e) => {
    if (!S.dirty) {
      await cmd.persistDocNow();
      return; // allow close
    }
    e.preventDefault();
    const ok = await platform.ask('Save changes?');
    if (ok) {
      const saved = await cmd.doSave();
      if (!saved) return; // save failed; stay open (toast explains)
    } else if (S.path) {
      // No = discard unsaved highlights/page edits; pins stay (sidecar-only)
      await platform.kvSet('doc:' + S.path, JSON.stringify({
        zoom: S.zoom,
        top: viewer.currentPageIdx() + 1,
        hl: S.hlColor,
        anns: S.anns.filter((a) => a.type === 'pin'),
      })).catch(() => {});
    }
    if (platform.kind === 'tauri') {
      const { getCurrentWindow } = await import('@tauri-apps/api/window');
      await getCurrentWindow().destroy();
    }
  });
  if (platform.kind === 'web') {
    const flush = (e) => {
      cmd.persistDocNow();
      if (S.dirty) e.preventDefault();
    };
    window.addEventListener('beforeunload', flush);
    window.addEventListener('pagehide', () => cmd.persistDocNow());
  }
}

async function boot() {
  viewer.init();
  initMenu();
  watchDocChanges(); // registered before annos so page rebuilds run first
  annos.init();
  search.init();
  thumbs.init({ rotate: cmd.rotatePage, del: cmd.deletePage });
  initContextMenu();
  initKeyboard();
  platform.onDrop(async (f) => {
    try {
      if (platform.kind === 'web' && f instanceof File) await cmd.openFile(f);
      else await cmd.openPath(f);
    } catch {}
  });
  await initCloseGuard();
  cmd.updateTitle();

  const initial = await platform.initialPath();
  if (initial) {
    try { await cmd.openPath(initial); } catch {}
  } else {
    // plain launch: reopen the last document
    const last = await platform.kvGet('lastPath');
    if (last) {
      try { await cmd.openPath(last); } catch {}
    }
  }
}

boot();

// debug/testing hook (also handy from devtools)
window.mnpdf = { S, viewer, cmd, annos, search, thumbs, bake: cmd.bake, state: () => ({ ...S, pdf: null }) };
