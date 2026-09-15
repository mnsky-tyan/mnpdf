// Highlights + hover-only text pins.
// Selecting text shows nothing by itself; right-clicking a selection offers
// highlight colors + copy. Pins are tiny dots placed via right-click; their
// text shows on hover and is stored in the per-document sidecar (never baked
// into the PDF), so nothing is printed on the page.
import { S, pushOp, onDocChange, newHighlight, newPin } from './state.js';
import { viewportFor, positionOverlays, setOverlayRenderer } from './viewer.js';
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

// ---- text selection (mnpdf-drawn, glyph-accurate, original blue) ----

// Chromium's native selection paints line boxes including stretched/trailing
// space and overshoots on blank-space drags — it can never look right on a
// PDF. So mnpdf prevents native selection entirely and draws its own
// selection rectangles from the trimmed glyph boxes, in the classic blue.
// What you see selected is byte-for-byte what Highlight/Copy act on.

let selAnchor = null;   // {node, off, layer, boxes} while dragging
let selState = null;    // {pageIdx, pdfRects, text} — persists after release
let selLastApplied = null;

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

function offsetAtX(vis, x) {
  if (!vis.node || vis.right <= vis.left) return vis.start;
  const frac = (x - vis.left) / (vis.right - vis.left);
  return Math.max(vis.start, Math.min(vis.end, vis.start + Math.round(frac * (vis.end - vis.start))));
}

function layerSpans(layer) {
  const out = [];
  for (const span of layer.querySelectorAll('span')) {
    const vis = spanVisibleBox(span);
    if (vis && vis.bottom - vis.top >= 2 && vis.right - vis.left >= 1) out.push(vis);
  }
  return out;
}

function lineAt(boxes, x, y) {
  let best = null, bestD = Infinity;
  for (const b of boxes) {
    if (y >= b.top - 3 && y <= b.bottom + 3) return b;
    const d = y < b.top ? b.top - y : y - b.bottom;
    if (d < bestD) { bestD = d; best = b; }
  }
  const lh = best ? best.bottom - best.top : 0;
  return best && bestD <= lh * 0.75 ? best : null;
}

// anchor/focus on a line: blank space beside the text means that line's
// first/last glyph; inside the text it means the character under the pointer
function anchorAt(boxes, x, y) {
  const vis = lineAt(boxes, x, y);
  if (!vis) return null;
  let off;
  if (x <= vis.left + 2) off = vis.start;
  else if (x >= vis.right - 2) off = vis.end;
  else off = offsetAtX(vis, x);
  return { node: vis.node, off, vis, boxes };
}

// the line segments between two anchors (ordered), each trimmed to glyphs and
// clipped to the pointer on the anchor lines
function segmentsBetween(a, b) {
  const bBoxes = b.boxes || a.boxes;
  const iA = a.boxes.indexOf(a.vis);
  const iB = bBoxes.indexOf(b.vis);
  const [first, last] = iA <= iB ? [a, b] : [b, a];
  const fi = Math.min(iA, iB), li = Math.max(iA, iB);
  const segs = [];
  for (let i = fi; i <= li; i++) {
    const vis = a.boxes[i];
    let sx = vis.left, sOff = vis.start, ex = vis.right, eOff = vis.end;
    if (i === fi && first.vis === vis && first.off > vis.start + 2) {
      sx = Math.min(first.off === vis.start ? vis.left : xOfOffset(vis, first.off), vis.right);
      sOff = first.off;
    }
    if (i === li && last.vis === vis && last.off < vis.end - 2) {
      ex = Math.max(last.off === vis.end ? vis.right : xOfOffset(vis, last.off), vis.left);
      eOff = last.off;
    }
    if (ex - sx < 0.5) continue;
    segs.push({ vis, sx, ex, sOff, eOff });
  }
  return segs;
}

function xOfOffset(vis, off) {
  if (!vis.node || vis.end <= vis.start) return vis.left;
  const frac = (off - vis.start) / (vis.end - vis.start);
  return vis.left + frac * (vis.right - vis.left);
}

function drawSelection(segs, wrapEl, vp, wr) {
  const rects = [];
  const parts = [];
  for (const seg of segs) {
    const p1 = vp.convertToPdfPoint(seg.sx - wr.left, seg.vis.top - wr.top);
    const p2 = vp.convertToPdfPoint(seg.ex - wr.left, seg.vis.bottom - wr.top);
    rects.push([
      Math.min(p1[0], p2[0]), Math.min(p1[1], p2[1]),
      Math.max(p1[0], p2[0]), Math.max(p1[1], p2[1]),
    ]);
    parts.push(seg.vis.text.slice(seg.sOff - seg.vis.start, seg.eOff - seg.vis.start));
  }
  return { pageIdx: +wrapEl.dataset.i, pdfRects: rects, text: parts.join(' ').replace(/\s+/g, ' ').trim() };
}

function renderSelection() {
  document.querySelectorAll('.sel-layer').forEach((l) => l.replaceChildren());
  if (!selState) return;
  const i = S.pageList.findIndex((p) => p.src === selState.pageIdx);
  if (i < 0) return;
  const vp = viewportFor(i);
  if (!vp) return;
  const wrap = document.querySelector(`.pagewrap[data-i="${i}"] .sel-layer`);
  if (!wrap) return;
  for (const r of selState.pdfRects) {
    const d = el('div', 'selrect');
    const v = vp.convertToViewportRectangle(r);
    d.style.left = Math.min(v[0], v[2]) + 'px';
    d.style.top = Math.min(v[1], v[3]) + 'px';
    d.style.width = Math.abs(v[2] - v[0]) + 'px';
    d.style.height = Math.abs(v[3] - v[1]) + 'px';
    wrap.appendChild(d);
  }
}

function clearSelection() {
  selState = null;
  renderSelection();
}

// what context menus act on
export function currentSelection() {
  if (!selState || !selState.pdfRects.length) return null;
  return { srcIdx: selState.pageIdx, rects: selState.pdfRects, text: selState.text };
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
      const boxes = layerSpans(tlEl);
      const a = anchorAt(boxes, e.clientX, e.clientY);
      if (!a) return;
      if (e.detail >= 2) {
        // double/triple click: word / whole line
        const vis = a.vis;
        let sOff = vis.start, eOff = vis.end;
        if (e.detail === 2) {
          let off = offsetAtX(vis, e.clientX);
          const t = vis.text;
          while (sOff < off && /\s/.test(t[sOff - vis.start] || '')) sOff++;
          while (eOff > off && /\s/.test(t[eOff - 1 - vis.start] || '')) eOff--;
          while (sOff > vis.start && !/\s/.test(t[sOff - 1 - vis.start] || '')) sOff--;
          while (eOff < vis.end && !/\s/.test(t[eOff - vis.start] || '')) eOff++;
        }
        selAnchor = { node: vis.node, off: sOff, layer: tlEl, boxes };
        const anchor = { node: vis.node, off: sOff, vis, boxes };
        const focus = { node: vis.node, off: eOff, vis, boxes };
        const segs = segmentsBetween(anchor, focus);
        selState = drawSelection(segs, tlEl.closest('.pagewrap'), viewportFor(+tlEl.closest('.pagewrap').dataset.i), tlEl.closest('.pagewrap').getBoundingClientRect());
        renderSelection();
        selAnchor = null;
        return;
      }
      selAnchor = { node: a.node, off: a.off, vis: a.vis, layer: tlEl, boxes };
    },
    true,
  );
  document.addEventListener(
    'mousemove',
    (e) => {
      if (!selAnchor || !(e.buttons & 1)) return;
      const focus = anchorAt(selAnchor.boxes, e.clientX, e.clientY);
      if (!focus) return;
      const last = selLastApplied;
      if (last && last.node === focus.node && last.off === focus.off) return;
      selLastApplied = focus;
      const anchor = { node: selAnchor.node, off: selAnchor.off, vis: selAnchor.vis, boxes: selAnchor.boxes };
      const segs = segmentsBetween(anchor, focus);
      selState = drawSelection(segs, selAnchor.layer.closest('.pagewrap'), viewportFor(+selAnchor.layer.closest('.pagewrap').dataset.i), selAnchor.layer.closest('.pagewrap').getBoundingClientRect());
      renderSelection();
    },
    true,
  );
  document.addEventListener('mouseup', () => { selAnchor = null; }, true);
  document.addEventListener('pointercancel', () => { selAnchor = null; });
  window.addEventListener('blur', () => { selAnchor = null; });
  document.addEventListener('keydown', (e) => {
    if (e.key === 'Escape' && selState) clearSelection();
  });
  document.addEventListener('mnpdf:scroll', renderSelection);
  document.addEventListener('mnpdf:zoom', renderSelection);
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
