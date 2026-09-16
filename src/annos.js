// Highlights + hover-only text pins.
// Selecting text shows nothing by itself; right-clicking a selection offers
// highlight colors + copy. Pins are tiny dots placed via right-click; their
// text shows on hover and is stored in the per-document sidecar (never baked
// into the PDF), so nothing is printed on the page.
import { S, pushOp, onDocChange, newHighlight, newPin } from './state.js';
import { viewportFor, positionOverlays, setSelectionDrag, setOverlayRenderer } from './viewer.js';
import { el } from './util.js';
import { combineSpans, groupLineSpans, lineAt, offsetAtX, selectionOffsetsForLine, wordModeSpan, xOfOffset } from './selection.js';

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
  if (end <= lead) return null;
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

// every rendered text line of the document, in reading order.
// docTop/docBottom are scroller-document y (stable while the view scrolls).
// pdf.js can split one visual line into several spans (whitespace runs, font
// changes); spans of one visual line are merged into one logical line
// (groupLineSpans) so a drag between them never reads as a cross-line drag.
function allLines() {
  const sc = document.getElementById('scroller');
  const st = sc.scrollTop;
  const out = [];
  document.querySelectorAll('.pagewrap').forEach((wrap) => {
    const layer = wrap.querySelector('.textLayer');
    if (!layer) return;
    const pageIdx = +wrap.dataset.i;
    for (const group of groupLineSpans(layerSpans(layer))) {
      const top = Math.min(...group.map((s) => s.top));
      const bottom = Math.max(...group.map((s) => s.bottom));
      const vis = combineSpans(group);
      for (const seg of vis.segs) { seg.top += st; seg.bottom += st; }
      out.push({ wrapEl: wrap, pageIdx, vis,
                 docTop: top + st, docBottom: bottom + st,
                 lh: bottom - top });
    }
  });
  return out;
}

function anchorAt(lines, x, docY) {
  const line = lineAt(lines, docY, x);
  if (!line) return null;
  const vis = line.vis;
  let off;
  if (x <= vis.left + 2) off = vis.start;
  else if (x >= vis.right - 2) off = vis.end;
  else off = offsetAtX(vis, x);
  return { node: vis.node, off, line };
}

function offAtLine(l, x) {
  const vis = l.vis;
  if (x <= vis.left + 2) return vis.start;
  if (x >= vis.right - 2) return vis.end;
  return offsetAtX(vis, x);
}
// per-line segments between two anchors (ordered): boundary lines are trimmed
// to the pointer character, middle lines are full glyphs, and each selected
// line extends down to the next selected line's top so the block is
// continuous — but the bridge is capped at ~1.5 line heights, so figures,
// margins and page gaps stay unpainted.
function buildSegments(lines, iA, offA, iF, offF) {
  const fi = Math.min(iA, iF), li = Math.max(iA, iF);
  const segs = [];
  for (let i = fi; i <= li; i++) {
    const line = lines[i];
    const vis = line.vis;
    const offsets = selectionOffsetsForLine(i, iA, offA, iF, offF, vis.start, vis.end);
    // a merged line paints one rect per span: the trimmed inter-span gaps
    // stay unpainted, exactly like the glyph boxes they cover
    const sOff = Math.min(offsets.start, offsets.end);
    const eOff = Math.max(offsets.start, offsets.end);
    const rects = [];
    for (const seg of vis.segs) {
      const s = Math.max(sOff, seg.start), e = Math.min(eOff, seg.end);
      if (e <= s) continue;
      const sx = xOfOffset(vis, s), ex = xOfOffset(vis, e);
      if (ex - sx < 0.5) continue;
      rects.push([Math.min(sx, ex), Math.max(sx, ex), seg.top, seg.bottom]);
    }
    if (!rects.length) continue;
    segs.push({ line, rects, sOff, eOff });
  }
  for (let i = 1; i < segs.length; i++) {
    const prev = segs[i - 1], cur = segs[i];
    if (prev.line.wrapEl !== cur.line.wrapEl) continue;
    const pr = prev.rects[prev.rects.length - 1], cr = cur.rects[0];
    const gap = cr[2] - pr[3];
    const bridge = (pr[3] - pr[2]) * 1.5;
    if (gap > 0 && gap < bridge) cr[2] = pr[3];
  }
  return segs;
}

function drawSelection(segs) {
  const sc = document.getElementById('scroller');
  const st = sc.scrollTop;
  const byPage = new Map();
  const parts = [];
  for (const seg of segs) {
    const vp = viewportFor(seg.line.pageIdx);
    if (!vp) continue;
    const wr = seg.line.wrapEl.getBoundingClientRect();
    for (const [sx, ex, top, bottom] of seg.rects) {
      const p1 = vp.convertToPdfPoint(sx - wr.left, top - st - wr.top);
      const p2 = vp.convertToPdfPoint(ex - wr.left, bottom - st - wr.top);
      const rect = [
        Math.min(p1[0], p2[0]), Math.min(p1[1], p2[1]),
        Math.max(p1[0], p2[0]), Math.max(p1[1], p2[1]),
      ];
      if (!byPage.has(seg.line.pageIdx)) byPage.set(seg.line.pageIdx, { srcIdx: seg.line.pageIdx, rects: [] });
      byPage.get(seg.line.pageIdx).rects.push(rect);
    }
    const lineParts = [];
    for (const spanSeg of seg.line.vis.segs) {
      const s = Math.max(seg.sOff, spanSeg.start), e = Math.min(seg.eOff, spanSeg.end);
      if (e > s) lineParts.push(spanSeg.text.slice(s - spanSeg.start, e - spanSeg.start));
    }
    parts.push(lineParts.join(' '));
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

let selLastLine = null;

function applySelectionAt(x, docY) {
  const lines = selAnchor.lines;
  // re-derive the anchor line from its stored doc position — stable across
  // scrolling and text-layer re-renders
  const anchorLine = lineAt(lines, selAnchor.docY, selAnchor.x);
  const focus = lineAt(lines, docY, x);
  if (!anchorLine || !focus) return;
  const focusOff = offAtLine(focus, x);
  let iA = lines.indexOf(anchorLine);
  let iF = lines.indexOf(focus);
  let offA = selAnchor.off, offF = focusOff;
  if (selAnchor.wordMode) {
    [iA, offA, iF, offF] = wordModeSpan(iA, selAnchor.off, selAnchor.offEnd, iF, focus.vis.text, focus.vis.start, focusOff);
  }
  selState = drawSelection(buildSegments(lines, iA, offA, iF, offF));
  renderSelection();
}

function lines_key(l) { return selAnchor.lines.indexOf(l); }

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
      const line = lineAt(lines, docY, e.clientX);
      if (!line) return;
      const vis = line.vis;
      let off;
      if (e.clientX <= vis.left + 2) off = vis.start;
      else if (e.clientX >= vis.right - 2) off = vis.end;
      else off = offsetAtX(vis, e.clientX);
      const wordMode = e.detail >= 2;
      let offEnd = off;
      if (wordMode) {
        // double click: the word under the press; dragging extends by words
        let sOff = off, eOff = off;
        const t = vis.text;
        while (sOff > vis.start && !/\s/.test(t[sOff - 1 - vis.start] || ' ')) sOff--;
        while (eOff < vis.end && !/\s/.test(t[eOff - vis.start] || ' ')) eOff++;
        selState = drawSelection(buildSegments([line], 0, sOff, 0, eOff));
        renderSelection();
        off = sOff;
        offEnd = eOff;
      }
      selAnchor = { node: vis.node, off, offEnd, line, lines, wordMode, x: e.clientX, docY };
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
  document.addEventListener('mouseup', (e) => {
    if (e.button !== 0) return;
    // A mouseup can arrive before the animation frame queued by the final
    // mousemove. Apply the release coordinates synchronously before clearing
    // the drag, otherwise the painted focus remains one event behind.
    if (selAnchor) {
      if (selApplyRaf) { cancelAnimationFrame(selApplyRaf); selApplyRaf = 0; }
      const sc = document.getElementById('scroller');
      applySelectionAt(e.clientX, e.clientY + sc.scrollTop);
    }
    selAnchor = null;
    stopSelAutoScroll();
  }, true);
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
  setSelectionDrag(true);
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
  setSelectionDrag(false);
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
