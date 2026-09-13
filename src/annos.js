// Highlights + text notes: capture from selection, overlay rendering,
// note editing/dragging, and the minimal floating swatch bar.
import { S, pushOp, onDocChange, newHighlight, newNote } from './state.js';
import { viewportFor, positionOverlays, setOverlayRenderer } from './viewer.js';
import { el } from './util.js';

export const HL_COLORS = ['#ffd400', '#7ded72', '#6ec1ff', '#ff9db1', '#ffb257'];

// ---- overlay rendering (registered into viewer) ----

let editingId = null; // note ann.id currently being edited

function renderWrapOverlays(i, w, vp) {
  const src = S.pageList[i].src;
  const hlFrag = document.createDocumentFragment();
  const nFrag = document.createDocumentFragment();
  for (const a of S.anns) {
    if (a.page !== src) continue;
    if (a.type === 'hl') {
      for (const r of a.rects) {
        const d = el('div', 'hl');
        d.style.background = a.color;
        const v = vp.convertToViewportRectangle(r);
        d.style.left = Math.min(v[0], v[2]) + 'px';
        d.style.top = Math.min(v[1], v[3]) + 'px';
        d.style.width = Math.abs(v[2] - v[0]) + 'px';
        d.style.height = Math.abs(v[3] - v[1]) + 'px';
        hlFrag.appendChild(d);
      }
    } else if (a.type === 'note' && a.text && a.id !== editingId) {
      const d = el('div', 'note');
      d.dataset.id = a.id;
      if (S.selAnnId === a.id) d.classList.add('sel');
      const [x, y] = vp.convertToViewportPoint(a.x, a.y);
      d.style.left = x + 'px';
      d.style.top = y + 'px';
      d.style.fontSize = a.size * vp.scale + 'px';
      d.style.color = a.color;
      d.textContent = a.text;
      nFrag.appendChild(d);
    }
  }
  w.hl.replaceChildren(hlFrag);
  w.notes.replaceChildren(nFrag);
}

// ---- selection -> highlight rects ----

// Clean line rects for the selected characters: walk text nodes of the page's
// text layer and take a sub-range per node (raw range.getClientRects() mixes in
// container-element junk when the range ends between spans).
function clientRectsFor(range, textLayerEl, wr) {
  const out = [];
  const walker = document.createTreeWalker(textLayerEl, NodeFilter.SHOW_TEXT);
  while (walker.nextNode()) {
    const node = walker.currentNode;
    if (!range.intersectsNode(node)) continue;
    const r = document.createRange();
    r.selectNodeContents(node);
    if (node === range.startContainer) r.setStart(node, range.startOffset);
    if (node === range.endContainer) r.setEnd(node, range.endOffset);
    if (r.collapsed) continue;
    for (const cr of r.getClientRects()) {
      if (cr.width < 1 || cr.height < 1) continue;
      if (cr.right < wr.left || cr.left > wr.right || cr.bottom < wr.top || cr.top > wr.bottom) continue;
      out.push(cr);
    }
  }
  return out;
}

// Returns {srcIdx, rects(pdf pts), at} if the current selection lives inside a
// rendered text layer, else null.
export function selectionInfo() {
  const sel = window.getSelection();
  if (!sel || sel.isCollapsed || sel.rangeCount === 0) return null;
  const range = sel.getRangeAt(0);
  const startEl = range.startContainer.nodeType === 1
    ? range.startContainer
    : range.startContainer.parentElement;
  const wrapEl = startEl?.closest?.('.pagewrap');
  if (!wrapEl) return null;
  const i = +wrapEl.dataset.i;
  if (!S.pageList[i]) return null;
  const vp = viewportFor(i);
  if (!vp) return null;
  const wr = wrapEl.getBoundingClientRect();
  const clientRects = clientRectsFor(range, wrapEl.querySelector('.textLayer'), wr);
  if (!clientRects.length) return null;
  const rects = clientRects.map((cr) => {
    const p1 = vp.convertToPdfPoint(cr.left - wr.left, cr.top - wr.top);
    const p2 = vp.convertToPdfPoint(cr.right - wr.left, cr.bottom - wr.top);
    return [
      Math.min(p1[0], p2[0]), Math.min(p1[1], p2[1]),
      Math.max(p1[0], p2[0]), Math.max(p1[1], p2[1]),
    ];
  });
  const last = clientRects.slice().sort((a, b) => a.top - b.top || a.right - b.right).pop();
  return {
    srcIdx: S.pageList[i].src,
    rects,
    at: last ? { x: last.right, y: last.top } : null,
    text: String(sel),
  };
}

export function addHighlight(info, color) {
  S.hlColor = color;
  pushOp({ kind: 'add', ann: newHighlight(info.srcIdx, info.rects, color) });
  window.getSelection()?.removeAllRanges();
  hideSwatchBar();
}

// Highlight under a viewport point (for right-click delete), topmost first.
export function highlightAt(displayIdx, vx, vy) {
  const vp = viewportFor(displayIdx);
  if (!vp) return null;
  const [px, py] = vp.convertToPdfPoint(vx, vy);
  for (let k = S.anns.length - 1; k >= 0; k--) {
    const a = S.anns[k];
    if (a.type !== 'hl' || a.page !== S.pageList[displayIdx].src) continue;
    if (a.rects.some((r) => px >= r[0] && px <= r[2] && py >= r[1] && py <= r[3])) return a;
  }
  return null;
}

// ---- floating swatch bar over fresh selections ----

let bar = null;
let barVisible = false;

export function hideSwatchBar() {
  bar?.remove();
  bar = null;
  barVisible = false;
}

function showSwatchBar(info) {
  hideSwatchBar();
  bar = el('div', 'selbar');
  for (const c of HL_COLORS) {
    const b = el('button', 'selbar-sw');
    b.style.background = c;
    b.title = 'Highlight';
    b.addEventListener('pointerdown', (e) => {
      e.preventDefault();
      e.stopPropagation();
      addHighlight(info, c);
    });
    bar.appendChild(b);
  }
  const cp = el('button', 'selbar-cp', bar);
  cp.textContent = '⧉';
  cp.title = 'Copy text';
  cp.addEventListener('pointerdown', (e) => {
    e.preventDefault();
    e.stopPropagation();
    copyText(info.text);
    hideSwatchBar();
  });
  document.body.appendChild(bar);
  const r = bar.getBoundingClientRect();
  let x = info.at ? info.at.x : window.innerWidth / 2;
  let y = info.at ? info.at.y - r.height - 10 : 80;
  x = Math.max(8, Math.min(x - r.width / 2, window.innerWidth - r.width - 8));
  y = Math.max(8, Math.min(y, window.innerHeight - r.height - 8));
  bar.style.left = x + 'px';
  bar.style.top = y + 'px';
  barVisible = true;
}

export function copyText(t) {
  if (!t) return;
  const ta = el('textarea');
  ta.value = t;
  ta.style.position = 'fixed';
  ta.style.opacity = '0';
  document.body.appendChild(ta);
  ta.select();
  try { document.execCommand('copy'); } catch {}
  ta.remove();
}

// ---- notes ----

function displayIndexOfSrc(src) {
  return S.pageList.findIndex((p) => p.src === src);
}

export function startNewNote(displayIdx, vx, vy) {
  const vp = viewportFor(displayIdx);
  if (!vp) return;
  const [x, y] = vp.convertToPdfPoint(vx, vy);
  const ann = newNote(S.pageList[displayIdx].src, x, y);
  startEdit(ann, true);
}

export function startEdit(ann, isNew = false) {
  const i = displayIndexOfSrc(ann.page);
  if (i < 0) return;
  const vp = viewportFor(i);
  const w = document.querySelector(`.pagewrap[data-i="${i}"]`);
  if (!vp || !w) return;
  editingId = ann.id;
  const [x, y] = vp.convertToViewportPoint(ann.x, ann.y);
  const ta = el('textarea', 'note-ta');
  ta.style.left = x + 'px';
  ta.style.top = y + 'px';
  ta.style.fontSize = ann.size * vp.scale + 'px';
  ta.style.color = ann.color;
  ta.value = ann.text || '';
  ta.spellcheck = false;
  w.querySelector('.note-layer').appendChild(ta);
  const size = () => {
    ta.style.height = 'auto';
    ta.style.height = ta.scrollHeight + 'px';
    ta.style.width = 'auto';
    const cols = Math.max(6, ...ta.value.split('\n').map((l) => l.length + 1));
    ta.style.width = Math.min(46, cols) + 'ch';
    if (!ta.value) ta.style.width = '10ch';
  };
  ta.addEventListener('input', size);
  size();
  requestAnimationFrame(() => { size(); ta.focus(); });

  let closed = false;
  const close = (commit) => {
    if (closed) return;
    closed = true;
    const v = ta.value.replace(/\s+$/, '');
    ta.remove();
    editingId = null;
    if (commit) {
      if (isNew) {
        if (v) pushOp({ kind: 'add', ann: { ...ann, text: v } });
      } else if (v && v !== ann.text) {
        pushOp({ kind: 'text', id: ann.id, from: ann.text, to: v });
      }
    }
    positionOverlays(true);
  };
  ta.addEventListener('blur', () => close(true));
  ta.addEventListener('keydown', (e) => {
    e.stopPropagation();
    if (e.key === 'Enter' && (e.ctrlKey || e.metaKey)) close(true);
    else if (e.key === 'Escape') close(false);
  });
}

export function deleteAnn(id) {
  const ann = S.anns.find((a) => a.id === id);
  if (!ann) return;
  pushOp({ kind: 'del', ann });
  if (S.selAnnId === id) S.selAnnId = null;
}

function initNoteInteractions() {
  document.addEventListener('pointerdown', (e) => {
    const noteEl = e.target.closest?.('.note');
    if (!noteEl) return;
    const id = noteEl.dataset.id;
    const ann = S.anns.find((a) => a.id === id);
    if (!ann) return;
    const wrapEl = noteEl.closest('.pagewrap');
    const i = +wrapEl.dataset.i;
    const vp = viewportFor(i);
    if (!vp) return;
    if (e.button !== 0) { S.selAnnId = id; positionOverlays(true); return; }
    e.preventDefault();
    const start = { px: e.clientX, py: e.clientY, ax: ann.x, ay: ann.y };
    const p0 = vp.convertToPdfPoint(0, 0);
    let moved = false;
    noteEl.setPointerCapture(e.pointerId);
    const move = (ev) => {
      if (Math.abs(ev.clientX - start.px) + Math.abs(ev.clientY - start.py) > 3) moved = true;
      if (!moved) return;
      const p1 = vp.convertToPdfPoint(ev.clientX - start.px, ev.clientY - start.py);
      ann.x = start.ax + (p1[0] - p0[0]);
      ann.y = start.ay + (p1[1] - p0[1]);
      const [x, y] = vp.convertToViewportPoint(ann.x, ann.y);
      noteEl.style.left = x + 'px';
      noteEl.style.top = y + 'px';
    };
    const up = () => {
      noteEl.removeEventListener('pointermove', move);
      noteEl.removeEventListener('pointerup', up);
      if (moved) {
        pushOp({
          kind: 'move', id,
          from: { x: start.ax, y: start.ay },
          to: { x: ann.x, y: ann.y },
        });
      } else {
        S.selAnnId = S.selAnnId === id ? null : id;
        positionOverlays(true);
      }
    };
    noteEl.addEventListener('pointermove', move);
    noteEl.addEventListener('pointerup', up);
  });

  document.addEventListener('dblclick', (e) => {
    const noteEl = e.target.closest?.('.note');
    if (!noteEl) return;
    const ann = S.anns.find((a) => a.id === noteEl.dataset.id);
    if (ann) startEdit(ann);
  });
}

// ---- selection watcher ----

let selTimer = null;
function watchSelection() {
  document.addEventListener('selectionchange', () => {
    clearTimeout(selTimer);
    selTimer = setTimeout(() => {
      const sel = window.getSelection();
      if (!sel || sel.isCollapsed) { hideSwatchBar(); return; }
      const info = selectionInfo();
      if (info) showSwatchBar(info);
      else hideSwatchBar();
    }, 220);
  });
  document.addEventListener('pointerdown', (e) => {
    if (bar && !bar.contains(e.target)) hideSwatchBar();
  }, true);
}

export function init() {
  setOverlayRenderer(renderWrapOverlays);
  onDocChange((what) => {
    positionOverlays(true);
    if (what?.kind === 'pages') hideSwatchBar();
  });
  document.addEventListener('mnpdf:zoom', hideSwatchBar);
  document.addEventListener('mnpdf:scroll', hideSwatchBar);
  initNoteInteractions();
  watchSelection();
}
