// Highlights + hover-only text pins.
// Selecting text shows nothing by itself; right-clicking a selection offers
// highlight colors + copy. Pins are tiny dots placed via right-click; their
// text shows on hover and is stored in the per-document sidecar (never baked
// into the PDF), so nothing is printed on the page.
import { S, pushOp, onDocChange, newHighlight, newPin } from './state.js';
import { viewportFor, positionOverlays, setOverlayRenderer } from './viewer.js';
import { buildSegments, mergeRows, rowOffsetAtX, rowXOf } from './selection.js';
import { el } from './util.js';

// pastel set — vivid primaries fought with the text under them (v1.3.3)
export const HL_COLORS = ['#ffe680', '#c3f2bd', '#b8dcff', '#ffc9d4', '#ffd9ad'];
// docs saved with the old vivid palette read back paler too
export const HL_COLOR_MIGRATION = Object.fromEntries(
  ['#ffd400', '#7ded72', '#6ec1ff', '#ff9db1', '#ffb257'].map((c, i) => [c, HL_COLORS[i]]),
);

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

// ---- text selection (mnpdf-drawn, glyph-accurate, multi-page, original blue) ----
//
// Chromium's native selection paints stretched/trailing whitespace and
// overshoots on blank-space drags — unusable on a PDF. So mnpdf prevents it
// and draws its own selection rectangles from trimmed glyph boxes across all
// pages. What is painted blue is exactly what Highlight/Copy act on.
//
// All vertical geometry is kept in SCROLLER-DOCUMENT coordinates
// (viewport y + scrollTop) so the selection stays anchored while edge
// auto-scroll moves the view. Line boxes are computed once per drag.

let selAnchor = null;   // {node, off, lines, line, wordMode}
let selState = null;    // {segments: [{srcIdx, rects}], text}
let selLastKey = '';
let selPointer = null;  // {x, y} client coords
let selRaf = 0;
let selApplyRaf = 0;

function spanVisibleBox(span) {
  const text = span.textContent || '';
  const node = span.firstChild;
  if (!node || node.nodeType !== Node.TEXT_NODE) {
    if (!text.trim()) return null;
    const r = span.getBoundingClientRect();
    return { left: r.left, right: r.right, top: r.top, bottom: r.bottom,
             node: null, start: 0, end: text.length, text };
  }
  const lead = text.length - text.replace(/^\s+/, '').length;
  const end = lead + text.trim().length;
  if (end <= lead) {
    // whitespace-only span: no glyphs, but it still occupies real advance
    // width — keep it as a one-space contribution so the merged row text
    // keeps its word separation ("Retrodict reports", not "Retrodictreports")
    if (!text.length) return null;
    const r = span.getBoundingClientRect();
    return { left: r.left, right: r.right, top: r.top, bottom: r.bottom,
             node, start: 0, end: 1, text: ' ' };
  }
  const range = document.createRange();
  range.setStart(node, lead);
  range.setEnd(node, end);
  const rects = [...range.getClientRects()].filter((r) => r.width > 0.5 && r.height > 0.5);
  if (!rects.length) return null;
  return {
    left: Math.min(...rects.map((r) => r.left)),
    right: Math.max(...rects.map((r) => r.right)),
    top: Math.min(...rects.map((r) => r.top)),
    bottom: Math.max(...rects.map((r) => r.bottom)),
    node, start: lead, end,
    text: text.slice(lead, end),
  };
}

function layerSpans(layer) {
  const out = [];
  for (const span of layer.querySelectorAll('span')) {
    const vis = spanVisibleBox(span);
    if (vis && vis.bottom - vis.top >= 2 && vis.right - vis.left >= 1) out.push(vis);
  }
  return out;
}

// per-span mappers. Glyph-exact: the browser laid the text out, so DOM ranges
// over the span's text node give the true character boxes — proportional
// mapping (offset/chars × width) misses by up to a character on proportional
// fonts, which is visible on word selection (double-click cut the last glyph).
// The node guards cover spans without a text node, which cannot map into
// glyphs; those keep the span box as the approximation.
const charBoxes = new WeakMap();
function visChars(vis) {
  let boxes = charBoxes.get(vis.node);
  if (!boxes) {
    boxes = [];
    for (let i = vis.start; i < vis.end; i++) {
      const r = document.createRange();
      r.setStart(vis.node, i);
      r.setEnd(vis.node, i + 1);
      const b = r.getBoundingClientRect();
      boxes.push({ left: b.left, right: b.right });
    }
    charBoxes.set(vis.node, boxes);
  }
  return boxes;
}

function localOff(vis, x) {
  if (!vis.node || vis.right <= vis.left) return vis.start;
  const boxes = visChars(vis);
  for (let i = 0; i < boxes.length; i++) {
    if (x <= boxes[i].right) return vis.start + i + (x > (boxes[i].left + boxes[i].right) / 2 ? 1 : 0);
  }
  return vis.end;
}
function localX(vis, off) {
  if (!vis.node || vis.end <= vis.start) return vis.left;
  if (off <= vis.start) return vis.left;
  if (off >= vis.end) return vis.right;
  return visChars(vis)[off - vis.start].left;
}

function offsetAtX(vis, x) {
  return rowOffsetAtX(vis, x, localOff);
}

function xOfOffset(vis, off) {
  return rowXOf(vis, off, localX);
}

// one rendered text row: spans merged by vertical overlap (see selection.js
// mergeRows — per-span entries break row trims and drop mid-row pieces).
// Merged box + text live on the row; per-span vis boxes stay reachable as
// row.parts for the offset mappers.
function rowEntries(layer, st) {
  const rows = mergeRows(layerSpans(layer));
  return rows.map((row) => ({
    vis: row,
    docTop: row.top + st, docBottom: row.bottom + st,
    left: row.left, right: row.right,
    top: row.top + st, bottom: row.bottom + st,
    start: 0, end: row.text.length,
    xOf: (off) => rowXOf(row, off, localX),
  }));
}

// every rendered text line of the document, in reading order — one entry per
// visual row. docTop/docBottom are scroller-document y (stable while the view
// scrolls); x stays client-viewport (stable while scrolling).
function allLines() {
  const sc = document.getElementById('scroller');
  const st = sc.scrollTop;
  const out = [];
  document.querySelectorAll('.pagewrap').forEach((wrap) => {
    const layer = wrap.querySelector('.textLayer');
    if (!layer) return;
    const pageIdx = +wrap.dataset.i;
    for (const e of rowEntries(layer, st)) {
      e.wrapEl = wrap;
      e.pageIdx = pageIdx;
      out.push(e);
    }
  });
  return out;
}

function lineAt(lines, docY) {
  if (!lines.length) return null;
  for (const l of lines) {
    if (docY >= l.docTop - 2 && docY <= l.docBottom + 2) return l;
  }
  let best = null, bestD = Infinity;
  for (const l of lines) {
    const c = (l.docTop + l.docBottom) / 2;
    const d = Math.abs(docY - c);
    if (d < bestD) { bestD = d; best = l; }
  }
  return best;
}

function anchorAt(lines, x, docY) {
  const line = lineAt(lines, docY);
  if (!line) return null;
  return { off: offsetAtX(line.vis, x), line };
}

function offAtLine(l, x) {
  return offsetAtX(l.vis, x);
}

function snapWord(vis, off) {
  let i = Math.max(vis.start, Math.min(vis.end, off)) - vis.start;
  const t = vis.text;
  while (i > 0 && !/\s/.test(t[i - 1] || ' ')) i--;
  while (i < t.length && !/\s/.test(t[i] || ' ')) i++;
  return vis.start + i;
}

// boundary segments are trimmed and the block bridged by the pure
// geometry in selection.js (direction-aware, pitch-limited)

function drawSelection(segs) {
  const sc = document.getElementById('scroller');
  const st = sc.scrollTop;
  const byPage = new Map();
  const parts = [];
  for (const seg of segs) {
    const vp = viewportFor(seg.line.pageIdx);
    if (!vp) continue;
    const wr = seg.line.wrapEl.getBoundingClientRect();
    const topView = seg.docTop - st;
    const botView = seg.docBottom - st;
    const p1 = vp.convertToPdfPoint(seg.sx - wr.left, topView - wr.top);
    const p2 = vp.convertToPdfPoint(seg.ex - wr.left, botView - wr.top);
    const rect = [
      Math.min(p1[0], p2[0]), Math.min(p1[1], p2[1]),
      Math.max(p1[0], p2[0]), Math.max(p1[1], p2[1]),
    ];
    if (!byPage.has(seg.line.pageIdx)) byPage.set(seg.line.pageIdx, { srcIdx: seg.line.pageIdx, rects: [] });
    byPage.get(seg.line.pageIdx).rects.push(rect);
    parts.push(seg.line.vis.text.slice(seg.sOff - seg.line.vis.start, seg.eOff - seg.line.vis.start));
  }
  return { segments: [...byPage.values()], text: parts.join(' ').replace(/\s+/g, ' ').trim() };
}

function renderSelection() {
  document.querySelectorAll('.sel-layer').forEach((l) => l.replaceChildren());
  if (!selState) return;
  for (const seg of selState.segments) {
    const i = S.pageList.findIndex((p) => p.src === seg.srcIdx);
    if (i < 0) continue;
    const vp = viewportFor(i);
    if (!vp) continue;
    const layer = document.querySelector(`.pagewrap[data-i="${i}"] .sel-layer`);
    if (!layer) continue;
    for (const r of seg.rects) {
      const d = el('div', 'selrect');
      const v = vp.convertToViewportRectangle(r);
      d.style.left = Math.min(v[0], v[2]) + 'px';
      d.style.top = Math.min(v[1], v[3]) + 'px';
      d.style.width = Math.abs(v[2] - v[0]) + 'px';
      d.style.height = Math.abs(v[3] - v[1]) + 'px';
      layer.appendChild(d);
    }
  }
}

function clearSelection() {
  selState = null;
  renderSelection();
}

// what context menus act on: one segment per crossed page
export function currentSelection() {
  if (!selState || !selState.segments.length) return null;
  return { segments: selState.segments, text: selState.text };
}

let selLinesAt = 0; // when selAnchor.lines was built

// pages render lazily as you scroll. The row list captured at mousedown only
// knows the pages rendered back then, so a drag that walks into newly
// rendered pages would stall at the old edge while the view keeps scrolling
// (pointer maps to the nearest stale row forever). Rebuild cheaply: on span
// count change (a page's text layer got populated) and at least every 200ms.
function cachedLines() {
  const count = document.querySelectorAll('.pagewrap .textLayer span').length;
  const stale = performance.now() - selLinesAt > 200;
  if (count !== selAnchor.spanCount || stale) {
    selAnchor.lines = allLines();
    selAnchor.spanCount = count;
    selLinesAt = performance.now();
  }
  return selAnchor.lines;
}

function applySelectionAt(x, docY) {
  const lines = cachedLines();
  // re-derive the anchor line from its stored doc position — stable across
  // scrolling and text-layer re-renders
  const anchorLine = lineAt(lines, selAnchor.docY);
  const focus = lineAt(lines, docY);
  if (!anchorLine || !focus) return;
  let focusOff = offAtLine(focus, x);
  if (selAnchor.wordMode) focusOff = snapWord(focus.vis, focusOff);
  const iA = lines.indexOf(anchorLine);
  const iF = lines.indexOf(focus);
  selState = drawSelection(buildSegments(lines, iA, selAnchor.off, iF, focusOff));
  renderSelection();
}

function initManualSelection() {
  document.addEventListener(
    'mousedown',
    (e) => {
      if (e.button !== 0) return;
      const tlEl = e.target.closest?.('.textLayer');
      if (!tlEl) return;
      e.preventDefault(); // Chromium's selection paint is what goes wrong
      clearSelection();
      const sc = document.getElementById('scroller');
      const docY = e.clientY + sc.scrollTop;
      const lines = allLines();
      const line = lineAt(lines, docY);
      if (!line) return;
      selLinesAt = performance.now();
      const vis = line.vis;
      let off = offsetAtX(vis, e.clientX);
      const wordMode = e.detail >= 2;
      if (wordMode) {
        // double click: the word under the press; dragging extends by words
        let sOff = off, eOff = off;
        const t = vis.text;
        while (sOff > vis.start && !/\s/.test(t[sOff - 1 - vis.start] || ' ')) sOff--;
        while (eOff < vis.end && !/\s/.test(t[eOff - vis.start] || ' ')) eOff++;
        selState = drawSelection(buildSegments([line], 0, sOff, 0, eOff), [line], 0);
        renderSelection();
        off = sOff;
      }
      selAnchor = { off, line, lines, wordMode, docY,
                    spanCount: document.querySelectorAll('.pagewrap .textLayer span').length };
      selPointer = { x: e.clientX, y: e.clientY };
      selLastKey = '';
      startSelAutoScroll();
    },
    true,
  );
  document.addEventListener(
    'mousemove',
    (e) => {
      if (!selAnchor || !(e.buttons & 1)) return;
      selPointer = { x: e.clientX, y: e.clientY };
      scheduleApply();
    },
    true,
  );
  document.addEventListener('mouseup', () => { selAnchor = null; stopSelAutoScroll(); }, true);
  document.addEventListener('pointercancel', () => { selAnchor = null; stopSelAutoScroll(); });
  window.addEventListener('blur', () => { selAnchor = null; stopSelAutoScroll(); });
  document.addEventListener('keydown', (e) => {
    if (e.key === 'Escape' && selState) { clearSelection(); selAnchor = null; stopSelAutoScroll(); }
  });
  document.addEventListener('mnpdf:scroll', renderSelection);
  document.addEventListener('mnpdf:zoom', () => { selAnchor = null; renderSelection(); });
}

function scheduleApply() {
  if (selApplyRaf) return;
  selApplyRaf = requestAnimationFrame(() => {
    selApplyRaf = 0;
    if (!selAnchor) return;
    const sc = document.getElementById('scroller');
    applySelectionAt(selPointer.x, selPointer.y + sc.scrollTop);
  });
}

// edge auto-scroll while selecting: hold near the top/bottom and the document
// keeps scrolling; the selection extends over the newly revealed lines —
// across page boundaries.
function startSelAutoScroll() {
  if (selRaf) return;
  const scroller = document.getElementById('scroller');
  const tick = () => {
    if (!selAnchor || !selPointer) { selRaf = 0; return; }
    const sr = scroller.getBoundingClientRect();
    const M = 64;
    let v = 0;
    if (selPointer.y < sr.top + M) v = -Math.min(12, Math.ceil((sr.top + M - selPointer.y) * 0.18) + 2);
    else if (selPointer.y > sr.bottom - M) v = Math.min(12, Math.ceil((selPointer.y - (sr.bottom - M)) * 0.18) + 2);
    if (v) {
      scroller.scrollTop += v;
      scheduleApply();
    }
    selRaf = requestAnimationFrame(tick);
  };
  selRaf = requestAnimationFrame(tick);
}

function stopSelAutoScroll() {
  if (selRaf) { cancelAnimationFrame(selRaf); selRaf = 0; }
}

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
  const ann = newHighlight(info.srcIdx, mergeRects(info.rects), color, info.text || '');
  const replaced = S.anns.filter(
    (a) =>
      a.type === 'hl' && a.page === info.srcIdx &&
      a.rects.some((r) => ann.rects.some((n) => rectsOverlap(n, r))),
  );
  if (replaced.length) pushOp({ kind: 'hlreplace', ann, replaced });
  else pushOp({ kind: 'add', ann });
  clearSelection(); // the drawn selection has become the highlight
}

// highlight a (possibly multi-page) selection: one highlight per page, one
// undo step; overlapping old highlights are replaced
export function addHighlightMulti(sel, color) {
  S.hlColor = color;
  const anns = [];
  const replaced = [];
  sel.segments.forEach((seg) => {
    const rects = mergeRects(seg.rects);
    if (!rects.length) return;
    anns.push(newHighlight(seg.srcIdx, rects, color, seg === sel.segments[0] ? sel.text : ''));
    replaced.push(S.anns.filter(
      (a) => a.type === 'hl' && a.page === seg.srcIdx &&
        a.rects.some((r) => rects.some((n) => rectsOverlap(n, r))),
    ));
  });
  if (!anns.length) return;
  if (replaced.some((r) => r.length)) pushOp({ kind: 'hlreplacemany', anns, replaced });
  else pushOp({ kind: 'addmany', anns });
  clearSelection();
}

export function recolorHighlight(id, color) {
  const a = S.anns.find((x) => x.id === id);
  if (!a || a.type !== 'hl' || a.color === color) return;
  S.hlColor = color;
  pushOp({ kind: 'recolor', id, from: a.color, to: color });
}

// Windows Chromium clears the text selection on right mousedown, before the
// contextmenu event — remember the last live selection so the context menu can
// still offer Highlight.
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
  initManualSelection();
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
