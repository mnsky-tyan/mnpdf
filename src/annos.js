// Highlights + hover-only text pins.
// Selecting text shows nothing by itself; right-clicking a selection offers
// highlight colors + copy. Pins are tiny dots placed via right-click; their
// text shows on hover and is stored in the per-document sidecar (never baked
// into the PDF), so nothing is printed on the page.
import { S, pushOp, onDocChange, newHighlight, newPin } from './state.js';
import { viewportFor, positionOverlays, setOverlayRenderer } from './viewer.js';
import { pointNearRect, bandClipRect } from './selection.js';
import { el } from './util.js';

export const HL_COLORS = ['#ffd400', '#7ded72', '#6ec1ff', '#ff9db1', '#ffb257'];

// ---- overlay rendering (registered into viewer) ----

let editingId = null; // pin ann.id currently being edited
const popEl = document.getElementById('pinpop');

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
    } else if (a.type === 'pin') {
      // dot stays visible even while its text is being edited
      const d = el('div', 'pin');
      d.dataset.id = a.id;
      const [x, y] = vp.convertToViewportPoint(a.x, a.y);
      d.style.left = x + 'px';
      d.style.top = y + 'px';
      if (!a.text) d.classList.add('empty');
      nFrag.appendChild(d);
    }
  }
  w.hl.replaceChildren(hlFrag);
  w.notes.replaceChildren(nFrag);
}

// ---- selection -> highlight rects ----

// The browser's drag-selection overshoots badly on text layers (a 2-line drag
// can select to the end of the page when an endpoint lands in a gap). So for
// drags we derive highlight rects from the POINTER BAND ∩ text spans instead
// of trusting the selection. Double-click/keyboard selections keep the
// selection-based path.
let dragBand = null;    // {wrapEl, x0, y0, x1, y1} viewport coords
let bandSel = null;     // last band-derived selection info
let bandSelAt = 0;

function releaseSelecting() {
  document.querySelectorAll('.textLayer.selecting').forEach((l) => l.classList.remove('selecting'));
}

function initDragBandTracking() {
  document.addEventListener(
    'mousedown',
    (e) => {
      if (e.button !== 0) return;
      const tlEl = e.target.closest?.('.textLayer');
      if (!tlEl) return;
      // drag-from-blank: a press that misses every glyph box must not start a
      // selection at all (preventing mousedown's default blocks selection).
      let onText = false;
      for (const span of tlEl.querySelectorAll('span')) {
        if (pointNearRect(e.clientX, e.clientY, span.getBoundingClientRect())) {
          onText = true;
          break;
        }
      }
      if (!onText) {
        e.preventDefault();
        window.getSelection()?.removeAllRanges();
        return;
      }
      // drag-to-blank clamp: endOfContent covers the layer while selecting so
      // the browser keeps the endpoint on real text (see .textLayer.selecting)
      tlEl.classList.add('selecting');
      const wrapEl = tlEl.closest('.pagewrap');
      if (wrapEl) dragBand = { wrapEl, x0: e.clientX, y0: e.clientY, x1: e.clientX, y1: e.clientY };
    },
    true,
  );
  document.addEventListener(
    'pointermove',
    (e) => {
      if (dragBand && e.buttons & 1) {
        dragBand.x1 = e.clientX;
        dragBand.y1 = e.clientY;
      }
    },
    true,
  );
  document.addEventListener(
    'pointerup',
    () => {
      releaseSelecting();
      if (dragBand) {
        const derived = deriveBandSelection(dragBand);
        if (derived) {
          bandSel = derived;
          bandSelAt = Date.now();
          lastSel = derived;
        }
        dragBand = null;
      }
    },
    true,
  );
  document.addEventListener('pointercancel', () => {
    releaseSelecting();
    dragBand = null;
  });
  window.addEventListener('blur', releaseSelecting);
}

// rects covered by the pointer band: every text span the band crosses,
// horizontally clipped to the band
function deriveBandSelection(band) {
  const i = +band.wrapEl.dataset.i;
  if (!S.pageList[i]) return null;
  const vp = viewportFor(i);
  if (!vp) return null;
  const wr = band.wrapEl.getBoundingClientRect();
  const bx0 = Math.min(band.x0, band.x1), bx1 = Math.max(band.x0, band.x1);
  const by0 = Math.min(band.y0, band.y1), by1 = Math.max(band.y0, band.y1);
  const rects = [];
  for (const span of band.wrapEl.querySelectorAll('.textLayer span')) {
    const r = span.getBoundingClientRect();
    const clip = bandClipRect(r, by0, by1, bx0, bx1);
    if (!clip) continue;
    const p1 = vp.convertToPdfPoint(clip.sx - wr.left, r.top - wr.top);
    const p2 = vp.convertToPdfPoint(clip.ex - wr.left, r.bottom - wr.top);
    rects.push([
      Math.min(p1[0], p2[0]), Math.min(p1[1], p2[1]),
      Math.max(p1[0], p2[0]), Math.max(p1[1], p2[1]),
    ]);
  }
  if (!rects.length) return null;
  return {
    srcIdx: S.pageList[i].src, rects,
    text: String(window.getSelection() || ''),
    band: { x0: bx0, y0: by0, x1: bx1, y1: by1 },
  };
}

// what context menus should treat as "the selection": a recent pointer-band
// drag wins over the (overshooting) browser selection — but only when the
// right-click actually happened on the dragged region
export function preferredSelection(x, y) {
  if (
    bandSel && Date.now() - bandSelAt < 2500 &&
    x >= bandSel.band.x0 - 40 && x <= bandSel.band.x1 + 40 &&
    y >= bandSel.band.y0 - 40 && y <= bandSel.band.y1 + 40
  ) {
    return bandSel;
  }
  return selectionInfo() || lastSel;
}

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
  let clientRects = clientRectsFor(range, wrapEl.querySelector('.textLayer'), wr);
  if (!clientRects.length) return null;
  // sanity: drop any rect far taller than the typical selected line — a stale
  // text layer can leave overlapping spans that produce runaway rects
  const hs = clientRects.map((r) => r.height).sort((a, b) => a - b);
  const med = hs[Math.floor(hs.length / 2)] || 0;
  clientRects = clientRects.filter((r) => r.height <= med * 3 + 1);
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

// two pdf-space rects intersect (0.5pt tolerance so line-adjacent rects don't count)
function rectsOverlap(a, b) {
  return a[0] < b[2] - 0.5 && a[2] > b[0] + 0.5 && a[1] < b[3] - 0.5 && a[3] > b[1] + 0.5;
}

// union overlapping/touching line rects into bigger ones — stacked translucent
// rects render as darker "clogged" patches on screen and in the saved PDF
function mergeRects(rects) {
  const rs = rects.map((r) => [...r]);
  let merged = true;
  while (merged) {
    merged = false;
    outer: for (let i = 0; i < rs.length; i++) {
      for (let j = i + 1; j < rs.length; j++) {
        if (rectsOverlap(rs[i], rs[j])) {
          rs[i] = [
            Math.min(rs[i][0], rs[j][0]), Math.min(rs[i][1], rs[j][1]),
            Math.max(rs[i][2], rs[j][2]), Math.max(rs[i][3], rs[j][3]),
          ];
          rs.splice(j, 1);
          merged = true;
          break outer;
        }
      }
    }
  }
  return rs;
}

export function addHighlight(info, color) {
  S.hlColor = color;
  const ann = newHighlight(info.srcIdx, mergeRects(info.rects), color);
  const replaced = S.anns.filter(
    (a) =>
      a.type === 'hl' && a.page === info.srcIdx &&
      a.rects.some((r) => ann.rects.some((n) => rectsOverlap(n, r))),
  );
  if (replaced.length) pushOp({ kind: 'hlreplace', ann, replaced });
  else pushOp({ kind: 'add', ann });
  window.getSelection()?.removeAllRanges();
}

// Windows Chromium clears the text selection on right mousedown, before the
// contextmenu event — remember the last live selection so the context menu can
// still offer Highlight.
let lastSel = null;
let selTimer = null;

export function lastSelection() {
  return lastSel;
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

export function deleteAnn(id) {
  const ann = S.anns.find((a) => a.id === id);
  if (!ann) return;
  pushOp({ kind: 'del', ann });
  if (S.selAnnId === id) S.selAnnId = null;
}

// ---- copy helper (context menu) ----

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

// ---- pins: tiny dots, hover popup, click to edit ----

function hidePop() {
  popEl.classList.add('hidden');
}

function showPop(pinEl) {
  const ann = S.anns.find((a) => a.id === pinEl.dataset.id);
  if (!ann || ann.id === editingId) return;
  popEl.textContent = ann.text || 'Empty pin — click to add text';
  popEl.classList.toggle('empty', !ann.text);
  popEl.classList.remove('hidden');
  const r = pinEl.getBoundingClientRect();
  const pr = popEl.getBoundingClientRect();
  let x = r.left + r.width / 2 - pr.width / 2;
  let y = r.top - pr.height - 10;
  x = Math.max(8, Math.min(x, window.innerWidth - pr.width - 8));
  if (y < 8) y = r.bottom + 10;
  popEl.style.left = x + 'px';
  popEl.style.top = y + 'px';
}

export function startNewPin(displayIdx, vx, vy) {
  const vp = viewportFor(displayIdx);
  if (!vp) return;
  const [x, y] = vp.convertToPdfPoint(vx, vy);
  startPinEdit(newPin(S.pageList[displayIdx].src, x, y), true);
}

export function startPinEdit(ann, isNew = false) {
  const i = S.pageList.findIndex((p) => p.src === ann.page);
  if (i < 0) return;
  const vp = viewportFor(i);
  const w = document.querySelector(`.pagewrap[data-i="${i}"]`);
  if (!vp || !w) return;
  editingId = ann.id;
  hidePop();
  const [x, y] = vp.convertToViewportPoint(ann.x, ann.y);
  const layer = w.querySelector('.note-layer');
  // show the dot immediately so you can see where the pin landed
  let preview = null;
  if (isNew) {
    preview = el('div', 'pin preview');
    preview.style.left = x + 'px';
    preview.style.top = y + 'px';
    layer.appendChild(preview);
  }
  const ta = el('textarea', 'pin-ta');
  ta.style.left = x + 9 + 'px';
  ta.style.top = y + 9 + 'px';
  ta.value = ann.text || '';
  ta.placeholder = 'Type, then Ctrl+Enter';
  ta.spellcheck = false;
  layer.appendChild(ta);
  requestAnimationFrame(() => ta.focus());

  let closed = false;
  const close = (commit) => {
    if (closed) return;
    closed = true;
    const v = ta.value.replace(/\s+$/, '');
    ta.remove();
    preview?.remove();
    preview = null;
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

// ---- events ----

export function init() {
  setOverlayRenderer(renderWrapOverlays);
  initDragBandTracking();
  document.addEventListener('selectionchange', () => {
    clearTimeout(selTimer);
    selTimer = setTimeout(() => { lastSel = selectionInfo(); }, 120);
  });
  document.addEventListener('mnpdf:scroll', () => { lastSel = null; });
  document.addEventListener('mnpdf:zoom', () => { lastSel = null; });
  onDocChange((what) => {
    positionOverlays(true);
    hidePop();
    if (what?.kind === 'pages') window.getSelection()?.removeAllRanges();
  });

  // pin hover popup
  document.addEventListener('mouseover', (e) => {
    const pin = e.target.closest?.('.pin');
    if (pin) showPop(pin);
    else if (!e.target.closest?.('#pinpop')) hidePop();
  });
  document.addEventListener('mnpdf:zoom', hidePop);
  document.addEventListener('mnpdf:scroll', hidePop);

  // click a pin to edit it
  document.addEventListener('click', (e) => {
    const pin = e.target.closest?.('.pin');
    if (!pin || e.button !== 0) return;
    const ann = S.anns.find((a) => a.id === pin.dataset.id);
    if (ann) startPinEdit(ann);
  });
}
