// Page rendering, zoom, scroll, text layer, coordinate plumbing.
import * as pdfjsLib from 'pdfjs-dist';
import { S } from './state.js';
import { clamp, debounce } from './util.js';

pdfjsLib.GlobalWorkerOptions.workerSrc =
  (import.meta.env.BASE_URL || '/') + 'pdf.worker.min.mjs';

const scroller = document.getElementById('scroller');
const pagesEl = document.getElementById('pages');
const pill = document.getElementById('pill');

export const CSS_PER_PT = 96 / 72;

let wraps = [];          // per display index: {el, canvas, hl, find, tl, notes, key, task, ovStale}
let offsets = [];        // per display index: {top, h}
let dpr = window.devicePixelRatio;
let renderQueued = false;
let overlayRenderer = null; // set by annos.js
let onTopChange = null;     // set by main.js
let pillTimer = null;
let gotoEdit = null;

export function cssScale() {
  return S.zoom * CSS_PER_PT;
}

export function loadPdfDoc(bytesCopy) {
  return pdfjsLib.getDocument({
    data: bytesCopy,
    cMapUrl: (import.meta.env.BASE_URL || '/') + 'cmaps/',
    cMapPacked: true,
  }).promise;
}

export function setOverlayRenderer(fn) {
  overlayRenderer = fn;
}

export function setTopChangeFn(fn) {
  onTopChange = fn;
}

// Page proxies are cached so overlays/notes can compute viewports synchronously.
const pageCache = new Map();

export async function ensurePage(src) {
  let p = pageCache.get(src);
  if (p) return p;
  p = await S.pdf.getPage(src + 1);
  pageCache.set(src, p);
  if (!S.view[src]) {
    S.view[src] = p.view;
    S.unit[src] = p.userUnit ?? 1;
    S.baseRot[src] = p.rotate;
  }
  return p;
}

export function clearDoc() {
  pageCache.clear();
}

export async function prefetchSizes(all = false) {
  const n = S.nPages;
  const head = all || n <= 250 ? n : 8;
  await Promise.all(
    Array.from({ length: head }, (_, i) => ensurePage(i).catch(() => {})),
  );
  if (head < n) {
    (async () => {
      for (let i = head; i < n; i++) {
        try { await ensurePage(i); } catch {}
        if (i % 20 === 0) layoutSoon();
      }
      layoutSoon();
    })();
  }
}

// Viewport for a display page without needing a render (matches page.getViewport).
export function viewportFor(i) {
  const entry = S.pageList[i];
  if (!entry) return null;
  const page = pageCache.get(entry.src);
  if (!page) return null;
  const total = (((page.rotate + entry.rot) % 360) + 360) % 360;
  return page.getViewport({ scale: cssScale(), rotation: total });
}

export function buildPages() {
  wraps.forEach((w) => {
    try { w.task?.cancel(); } catch {}
    try { w.tlObj?.cancel(); } catch {}
  });
  pagesEl.replaceChildren();
  wraps = S.pageList.map((_, i) => {
    const wrapEl = document.createElement('div');
    wrapEl.className = 'pagewrap';
    wrapEl.dataset.i = i;
    wrapEl.innerHTML =
      '<canvas></canvas><div class="hl-layer"></div><div class="find-layer"></div>' +
      '<div class="textLayer"></div><div class="note-layer"></div>';
    pagesEl.appendChild(wrapEl);
    return {
      el: wrapEl,
      canvas: wrapEl.querySelector('canvas'),
      hl: wrapEl.querySelector('.hl-layer'),
      find: wrapEl.querySelector('.find-layer'),
      tl: wrapEl.querySelector('.textLayer'),
      tlObj: null,
      notes: wrapEl.querySelector('.note-layer'),
      key: null,
      task: null,
      ovStale: true,
    };
  });
  layoutAll();
}

function dispDims(i) {
  const { src, rot } = S.pageList[i];
  const v = S.view[src];
  const w = v ? v[2] - v[0] : 612;
  const h = v ? v[3] - v[1] : 792;
  const swap = (((S.baseRot[src] || 0) + rot) % 180) !== 0;
  const k = cssScale();
  return swap ? [h * k, w * k] : [w * k, h * k];
}

export function layoutAll() {
  const k = cssScale();
  wraps.forEach((w, i) => {
    const [W, H] = dispDims(i);
    w.el.style.width = Math.floor(W) + 'px';
    w.el.style.height = Math.floor(H) + 'px';
    w.el.style.setProperty('--scale-factor', k);
  });
  measure();
  positionOverlays(true);
  scheduleRender();
}

export const layoutSoon = debounce(() => layoutAll(), 150);

function measure() {
  offsets = wraps.map((w) => ({ top: w.el.offsetTop, h: w.el.offsetHeight }));
}

export function visibleRange() {
  const st = scroller.scrollTop;
  const vh = scroller.clientHeight;
  let lo = wraps.length, hi = -1;
  for (let i = 0; i < wraps.length; i++) {
    const o = offsets[i];
    if (!o) continue;
    if (o.top < st + vh + 400 && o.top + o.h > st - 400) {
      lo = Math.min(lo, i);
      hi = Math.max(hi, i);
    }
  }
  if (hi < 0) { lo = 0; hi = Math.min(1, wraps.length - 1); }
  return [Math.max(0, lo), hi];
}

export function currentPageIdx() {
  const st = scroller.scrollTop + 40;
  for (let i = 0; i < offsets.length; i++) {
    if (offsets[i] && offsets[i].top + offsets[i].h > st) return i;
  }
  return Math.max(0, offsets.length - 1);
}

export function goToDisplayPage(i, tryKeep = false) {
  if (!wraps.length) return;
  i = clamp(i, 0, wraps.length - 1);
  if (tryKeep) return;
  scroller.scrollTop = offsets[i]?.top ?? 0;
}

export function goToSrcPage(src) {
  const i = S.pageList.findIndex((p) => p.src === src);
  if (i >= 0) goToDisplayPage(i);
  return i >= 0;
}

// ---- rendering ----

export function scheduleRender() {
  if (renderQueued) return;
  renderQueued = true;
  requestAnimationFrame(() => {
    renderQueued = false;
    renderVisible();
  });
}

function renderVisible() {
  if (!S.pdf || !wraps.length) return;
  dpr = window.devicePixelRatio;
  const [lo, hi] = visibleRange();
  for (let i = lo; i <= hi; i++) renderPage(i);
  for (let i = 0; i < wraps.length; i++) {
    if (i < lo - 2 || i > hi + 2) maybeDestroy(i);
  }
}

const isCancel = (e) => /cancel/i.test(String(e?.name || e?.message || e));

async function renderPage(i) {
  const w = wraps[i];
  const { src, rot } = S.pageList[i];
  const key = `${src}|${rot}|${S.zoom.toFixed(4)}|${dpr.toFixed(2)}`;
  if (w.key === key) return;
  w.key = key;
  try { w.task?.cancel(); } catch {}
  let page;
  try {
    page = await ensurePage(src);
  } catch (e) {
    if (!isCancel(e)) console.warn('getPage', e);
    w.key = null;
    return;
  }
  if (w.key !== key) return;
  const total = (((page.rotate + rot) % 360) + 360) % 360;
  const vp = page.getViewport({ scale: cssScale(), rotation: total });
  const rvp = page.getViewport({ scale: cssScale() * dpr, rotation: total });
  const c = w.canvas;
  c.width = Math.max(1, Math.floor(rvp.width));
  c.height = Math.max(1, Math.floor(rvp.height));
  c.style.width = Math.floor(vp.width) + 'px';
  c.style.height = Math.floor(vp.height) + 'px';
  const task = page.render({ canvasContext: c.getContext('2d'), viewport: rvp });
  w.task = task;
  try {
    await task.promise;
  } catch (e) {
    if (!isCancel(e)) console.warn('render', e);
    if (w.key === key) w.key = null;
    return;
  }
  if (w.key !== key) return;
  // text layer (selection). Cancel any in-flight layer first — a stale layer
  // would keep appending spans into the fresh one and corrupt selection.
  try { w.tlObj?.cancel(); } catch {}
  w.tlObj = null;
  w.tl.replaceChildren();
  try {
    const tl = new pdfjsLib.TextLayer({
      textContentSource: page.streamTextContent(),
      container: w.tl,
      viewport: vp,
    });
    w.tlObj = tl;
    await tl.render();
    // pdf.js-style end-of-content marker (see .endOfContent in style.css):
    // while .selecting it covers the layer so the browser clamps drag
    // endpoints to real text instead of overshooting into blank space.
    const eoc = document.createElement('div');
    eoc.className = 'endOfContent';
    w.tl.appendChild(eoc);
  } catch (e) {
    if (!isCancel(e)) console.warn('textlayer', e);
  }
  if (w.key !== key) {
    try { w.tlObj?.cancel(); } catch {}
    w.tlObj = null;
    w.tl.replaceChildren();
  }
}

function maybeDestroy(i) {
  const w = wraps[i];
  if (!w || !w.key) return;
  try { w.task?.cancel(); } catch {}
  try { w.tlObj?.cancel(); } catch {}
  w.tlObj = null;
  w.canvas.width = 1;
  w.canvas.height = 1;
  w.tl.replaceChildren();
  w.key = null;
}

// ---- overlays (highlights, notes, search rects live in page wraps) ----

export function positionOverlays(force = false) {
  if (!wraps.length) return;
  const [lo, hi] = visibleRange();
  for (let i = 0; i < wraps.length; i++) {
    const w = wraps[i];
    const near = i >= lo - 2 && i <= hi + 2;
    if (!near) { w.ovStale = true; continue; }
    if (!force && !w.ovStale) continue;
    let vp = null;
    try { vp = viewportFor(i); } catch { vp = null; }
    if (!vp) continue;
    w.ovStale = false;
    overlayRenderer?.(i, w, vp);
  }
}

// ---- zoom ----

export function setZoom(nz, anchor) {
  nz = clamp(nz, 0.25, 6);
  if (Math.abs(nz - S.zoom) < 1e-4) return;
  const sr = scroller.getBoundingClientRect();
  let ax = 0, ay = 0, hasA = false, boxL = 0, boxT = 0;
  if (anchor) {
    hasA = true;
    const box = pagesEl.getBoundingClientRect();
    boxL = box.left - sr.left + scroller.scrollLeft;
    boxT = box.top - sr.top + scroller.scrollTop;
    ax = anchor.x - sr.left + scroller.scrollLeft - boxL;
    ay = anchor.y - sr.top + scroller.scrollTop - boxT;
  }
  const ratio = nz / S.zoom;
  S.zoom = nz;
  layoutAll();
  if (hasA) {
    const box2 = pagesEl.getBoundingClientRect();
    const nBoxL = box2.left - sr.left + scroller.scrollLeft;
    const nBoxT = box2.top - sr.top + scroller.scrollTop;
    scroller.scrollLeft = nBoxL + ax * ratio - (anchor.x - sr.left);
    scroller.scrollTop = nBoxT + ay * ratio - (anchor.y - sr.top);
  }
  document.dispatchEvent(new CustomEvent('mnpdf:zoom'));
  updatePill(true);
}

export function zoomAnchorCenter() {
  const sr = scroller.getBoundingClientRect();
  return { x: sr.left + scroller.clientWidth / 2, y: sr.top + scroller.clientHeight / 2 };
}

export function fitZoom() {
  if (!S.pageList.length) return 1;
  const i = currentPageIdx();
  const v = S.view[S.pageList[i]?.src] || S.view[0];
  const w = v ? v[2] - v[0] : 612;
  const h = v ? v[3] - v[1] : 792;
  const swap = ((S.baseRot[S.pageList[i]?.src] || 0) % 180) !== 0;
  const pageW = (swap ? h : w) * CSS_PER_PT;
  // exact: the white paper touches the window borders side-to-side
  return clamp(scroller.clientWidth / pageW, 0.25, 6);
}

// ---- pill / goto ----

export function updatePill(sticky = false) {
  if (gotoEdit) return; // never touch content/timers while the editor is open
  if (!wraps.length) { pill.classList.add('hidden'); return; }
  const i = currentPageIdx();
  const cur = i + 1;
  pill.textContent = `${cur} / ${wraps.length}  ·  ${Math.round(S.zoom * 100)}%`;
  pill.classList.remove('hidden');
  pill.classList.add('fading');
  clearTimeout(pillTimer);
  if (!sticky) pillTimer = setTimeout(() => pill.classList.add('hidden'), 1300);
  else pillTimer = setTimeout(() => pill.classList.add('hidden'), 1300);
  if (cur !== pill.dataset.cur) {
    pill.dataset.cur = cur;
    onTopChange?.(i);
  }
}

// mode 'page': type a page number. mode 'zoom': type a percentage.
export function pillGotoMode(mode = 'page') {
  if (gotoEdit) return;
  clearTimeout(pillTimer); // the auto-hide must not kill the editor mid-typing
  const saved = pill.textContent;
  pill.classList.remove('hidden', 'fading');
  pill.innerHTML = '';
  const inp = document.createElement('input');
  if (mode === 'zoom') {
    inp.value = String(Math.round(S.zoom * 100));
    inp.title = 'Zoom percent (25–600) — Enter to apply, Esc to cancel';
  } else {
    inp.value = pill.dataset.cur || '1';
    inp.title = 'Page number — Enter to go, Esc to cancel';
  }
  pill.appendChild(inp);
  inp.focus();
  inp.select();
  gotoEdit = { mode };
  const done = (ok) => {
    if (!gotoEdit) return;
    gotoEdit = null;
    const v = parseFloat(inp.value);
    pill.textContent = saved;
    updatePill(true); // restore the label and restart the fade cleanly
    if (!ok || !Number.isFinite(v)) return;
    if (mode === 'zoom') {
      if (v >= 25 && v <= 600) setZoom(v / 100, zoomAnchorCenter());
    } else {
      goToDisplayPage(v - 1);
    }
  };
  inp.addEventListener('keydown', (e) => {
    e.stopPropagation();
    if (e.key === 'Enter') done(true);
    else if (e.key === 'Escape') done(false);
  });
  inp.addEventListener('blur', () => gotoEdit && done(true));
}

// ---- events ----

export function init() {
  scroller.addEventListener(
    'wheel',
    (e) => {
      if (e.ctrlKey) {
        e.preventDefault();
        const d = e.deltaY * (e.deltaMode === 1 ? 33 : 1);
        const f = clamp(Math.exp(-d * 0.01), 0.78, 1.28);
        setZoom(S.zoom * f, { x: e.clientX, y: e.clientY });
      }
    },
    { passive: false },
  );

  let scrolling = false;
  scroller.addEventListener('scroll', () => {
    if (!scrolling) {
      scrolling = true;
      requestAnimationFrame(() => {
        scrolling = false;
        scheduleRender();
        updatePill();
        document.dispatchEvent(new CustomEvent('mnpdf:scroll'));
      });
    }
  });

  window.addEventListener('resize', debounce(() => {
    if (!wraps.length) return;
    measure();
    scheduleRender();
    positionOverlays();
  }, 120));

  // left half of the pill = go to page, right half = zoom percent
  pill.addEventListener('click', (e) => {
    const r = pill.getBoundingClientRect();
    pillGotoMode(e.clientX - r.left > r.width * 0.55 ? 'zoom' : 'page');
  });
  // don't fade out while the user is aiming at / hovering the pill
  pill.addEventListener('mouseenter', () => { if (!gotoEdit) clearTimeout(pillTimer); });
  pill.addEventListener('mouseleave', () => { if (!gotoEdit) updatePill(true); });

  // touchscreen pinch-zoom (trackpad pinch arrives as ctrl+wheel, above)
  let pinch = null;
  scroller.addEventListener(
    'touchstart',
    (e) => {
      if (e.touches.length !== 2) return;
      e.preventDefault();
      const [a, b] = e.touches;
      pinch = {
        d: Math.hypot(a.clientX - b.clientX, a.clientY - b.clientY),
        z: S.zoom,
        x: (a.clientX + b.clientX) / 2,
        y: (a.clientY + b.clientY) / 2,
      };
    },
    { passive: false },
  );
  scroller.addEventListener(
    'touchmove',
    (e) => {
      if (!pinch || e.touches.length !== 2) return;
      e.preventDefault();
      const [a, b] = e.touches;
      const d = Math.hypot(a.clientX - b.clientX, a.clientY - b.clientY);
      if (d > 10) setZoom(pinch.z * (d / pinch.d), { x: pinch.x, y: pinch.y });
    },
    { passive: false },
  );
  scroller.addEventListener('touchend', (e) => {
    if (e.touches.length < 2) pinch = null;
  });
}

export function wrapCount() {
  return wraps.length;
}
