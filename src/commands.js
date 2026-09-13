// All user actions (open/save/zoom/page ops), shared by menus + keyboard.
import platform from './platform.js';
import { S, markDirty, pushOp, undo, redo, onDocChange } from './state.js';
import * as viewer from './viewer.js';
import * as thumbs from './thumbs.js';
import * as search from './search.js';
import { bake } from './save.js';
import { toast, debounce, clamp } from './util.js';

const emptyEl = document.getElementById('empty');

async function kvGetJson(k) {
  try {
    const raw = await platform.kvGet(k);
    return raw ? JSON.parse(raw) : null;
  } catch {
    return null;
  }
}

export async function lastDir() {
  const v = await platform.kvGet('lastDir');
  return v || null;
}

// ---- per-document sidecar (zoom, page, unsaved annotations, pins) ----

export const persistDocSoon = debounce(persistDocNow, 700);

// Pins are always persisted (they never bake into the PDF); highlights/pages
// only while unsaved.
export function sidecarAnns() {
  return S.anns.filter((a) => a.type === 'pin' || S.dirty);
}

export async function persistDocNow() {
  if (!S.path || !S.pdf) return;
  const st = {
    zoom: S.zoom,
    top: viewer.currentPageIdx() + 1,
    hl: S.hlColor,
    anns: sidecarAnns(),
    pages: S.dirty ? S.pageList : undefined,
  };
  try {
    await platform.kvSet('doc:' + S.path, JSON.stringify(st));
  } catch (e) {
    console.warn('persist failed', e);
  }
}

async function loadDocState(path) {
  return kvGetJson('doc:' + path);
}

// ---- open / reopen ----

function validAnns(a) {
  return Array.isArray(a) && a.every(
    (x) =>
      x && typeof x.page === 'number' && x.page >= 0 && x.page < S.nPages &&
      ((x.type === 'hl' && Array.isArray(x.rects)) ||
        (typeof x.x === 'number' && typeof x.y === 'number' &&
          (x.type === 'pin' || x.type === 'note'))),
  );
}

function validPages(p) {
  return (
    Array.isArray(p) && p.length >= 1 && p.length <= S.nPages &&
    p.every((x) => x && Number.isInteger(x.src) && x.src >= 0 && x.src < S.nPages) &&
    new Set(p.map((x) => x.src)).size === p.length
  );
}

async function openBytes(bytes, path, name) {
  let pdf;
  try {
    pdf = await viewer.loadPdfDoc(bytes.slice());
  } catch (e) {
    const msg = e?.name === 'PasswordException'
      ? 'Password-protected PDFs are not supported'
      : 'Could not open PDF: ' + (e?.message || e);
    toast(msg, 3200);
    throw new Error(msg);
  }
  const keepZoom = S.zoom;
  Object.assign(S, {
    path, name, bytes, pdf,
    nPages: pdf.numPages,
    view: [], unit: [], baseRot: [],
    pageList: Array.from({ length: pdf.numPages }, (_, i) => ({ src: i, rot: 0 })),
    anns: [], undo: [], redo: [],
    dirty: false, selAnnId: null,
    zoom: 0.5,
  });
  viewer.clearDoc();
  emptyEl.classList.add('hidden');

  await viewer.prefetchSizes();

  let top = 0;
  const st = path ? await loadDocState(path) : null;
  if (st) {
    if (typeof st.zoom === 'number') S.zoom = clamp(st.zoom, 0.25, 6);
    if (typeof st.top === 'number') top = st.top - 1;
    if (st.hl) S.hlColor = st.hl;
    if (validAnns(st.anns)) {
      S.anns = st.anns.map((a) => (a.type === 'note' ? { ...a, type: 'pin' } : a));
    }
    if (validPages(st.pages)) S.pageList = st.pages;
    const bakedEdits = S.anns.filter((a) => a.type !== 'pin').length;
    if (bakedEdits || S.pageList.length !== S.nPages || S.pageList.some((p) => p.rot % 360 !== 0)) {
      markDirty(); // recovered unsaved highlight/page edits
    }
  }
  if (S.zoom === 0.5) S.zoom = viewer.fitZoom();
  if (S.zoom === 0.5) S.zoom = keepZoom > 0.25 && keepZoom < 6 ? keepZoom : 1;

  viewer.buildPages();
  thumbs.refresh(true);
  viewer.goToDisplayPage(top);
  viewer.updatePill(true);
  viewer.setTopChangeFn(() => persistDocSoon());
  updateTitle();
  toast(`${name} · ${pdf.numPages} page${pdf.numPages > 1 ? 's' : ''}`, 1200);
}

export function updateTitle() {
  platform.setTitle(`${S.dirty ? '• ' : ''}${S.name || 'mnpdf'}`);
}

export async function openPath(path) {
  toast('Opening…', 8000);
  const bytes = await platform.read(path);
  const name = String(path).split(/[\\/]/).pop();
  await openBytes(bytes, path, name);
  const dir = String(path).replace(/[\\/][^\\/]+$/, '');
  platform.kvSet('lastDir', dir).catch(() => {});
}

export async function openFile(file) {
  toast('Opening…', 8000);
  const bytes = new Uint8Array(await file.arrayBuffer());
  await openBytes(bytes, file.name, file.name);
}

export async function openCmd() {
  const r = await platform.openDialog((await lastDir()) || undefined);
  if (!r) return;
  if (platform.kind === 'web' && r instanceof File) openFile(r);
  else openPath(r);
}

// ---- save ----

async function reopenAfterSave(bytes) {
  // Write a clean sidecar first so openBytes doesn't restore the highlights we
  // just baked into the file. Pins stay: they never bake, so the sidecar is
  // their only home.
  await platform.kvSet('doc:' + S.path, JSON.stringify({
    zoom: S.zoom,
    top: viewer.currentPageIdx() + 1,
    hl: S.hlColor,
    anns: S.anns.filter((a) => a.type === 'pin'),
  }));
  await openBytes(bytes, S.path, S.name);
}

// returns true when the document ended up saved (or had nothing to save)
export async function doSave() {
  if (!S.pdf) return false;
  if (!S.dirty) { toast('No changes'); return true; }
  if (!S.path || platform.kind === 'web') return doSaveAs();
  try {
    toast('Saving…', 8000);
    const bytes = await bake();
    await platform.write(S.path, bytes);
    await reopenAfterSave(bytes);
    toast('Saved');
    return true;
  } catch (e) {
    console.error(e);
    toast('Save failed: ' + (e?.message || e), 3500);
    return false;
  }
}

export async function doSaveAs() {
  if (!S.pdf) return false;
  try {
    const target = await platform.saveDialog(S.name || 'document.pdf');
    if (!target) return false;
    toast('Saving…', 8000);
    const bytes = await bake();
    if (platform.kind === 'tauri') {
      await platform.write(target, bytes);
      S.path = target;
      S.name = String(target).split(/[\\/]/).pop();
      const dir = String(target).replace(/[\\/][^\\/]+$/, '');
      platform.kvSet('lastDir', dir).catch(() => {});
      await reopenAfterSave(bytes);
    } else {
      await platform.write(target, bytes);
      S.dirty = false;
      viewer.updatePill(true);
    }
    toast('Saved');
    return true;
  } catch (e) {
    console.error(e);
    toast('Save As failed: ' + (e?.message || e), 3500);
    return false;
  }
}

// ---- zoom / pages ----

export function zoomIn() { viewer.setZoom(S.zoom * 1.2, viewer.zoomAnchorCenter()); }
export function zoomOut() { viewer.setZoom(S.zoom / 1.2, viewer.zoomAnchorCenter()); }
export function zoomReset() { viewer.setZoom(1, viewer.zoomAnchorCenter()); }
export function zoomFit() { viewer.setZoom(viewer.fitZoom(), viewer.zoomAnchorCenter()); }

export function rotatePage(i, dir) {
  const before = S.pageList.map((p) => ({ ...p }));
  if (!before[i]) return;
  const after = before.map((p, k) => (k === i ? { ...p, rot: (((p.rot + dir) % 360) + 360) % 360 } : p));
  pushOp({ kind: 'pages', before, after });
}

export function deletePage(i) {
  if (S.pageList.length <= 1) { toast('Cannot delete the only page'); return; }
  const before = S.pageList.map((p) => ({ ...p }));
  const after = before.filter((_, k) => k !== i);
  pushOp({ kind: 'pages', before, after });
}

export function gotoPrompt() { viewer.pillGotoMode(); }
export function undoCmd() { if (S.undo.length) { undo(); toast('Undo'); } }
export function redoCmd() { if (S.redo.length) { redo(); toast('Redo'); } }

export { bake };
