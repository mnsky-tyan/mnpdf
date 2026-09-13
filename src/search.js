// Text search: per-page text with char->item mapping, approximate match rects.
import { S, onDocChange } from './state.js';
import { goToSrcPage, viewportFor } from './viewer.js';
import { toast } from './util.js';

const box = document.getElementById('search');
const input = document.getElementById('search-input');
const count = document.getElementById('search-count');

let matches = [];   // {src, rects:[[x1,y1,x2,y2],..]}
let cur = -1;
let active = false;
let running = 0;
const cache = new Map(); // src -> {text, chars:[{b:{x,y,w,h,L},k}]}

async function pageText(src) {
  if (cache.has(src)) return cache.get(src);
  const page = await S.pdf.getPage(src + 1);
  const tc = await page.getTextContent();
  let text = '';
  const chars = [];
  let prev = null;
  for (const it of tc.items) {
    if (!it.str || !it.transform) continue;
    const base = {
      x: it.transform[4],
      y: it.transform[5],
      w: it.width || 0,
      h: it.height || 10,
      L: it.str.length,
    };
    if (prev && Math.abs(prev.y - base.y) < Math.max(prev.h, base.h) * 0.6) {
      const gap = base.x - (prev.x + prev.w);
      if (gap > Math.max(prev.h, base.h) * 0.2 && !/\s$/.test(text) && !/^\s/.test(it.str)) {
        text += ' ';
        chars.push(null);
      }
    } else if (text && !/\s$/.test(text)) {
      text += ' ';
      chars.push(null);
    }
    for (let k = 0; k < it.str.length; k++) chars.push({ b: base, k });
    text += it.str;
    prev = base;
  }
  const rec = { text: text.toLowerCase(), chars };
  cache.set(src, rec);
  return rec;
}

function rectsFor(rec, s, e) {
  const rects = [];
  let runB = null, k0 = 0, k1 = 0;
  const flush = () => {
    if (!runB) return;
    const a = runB.x + runB.w * (k0 / runB.L);
    const b = runB.x + runB.w * ((k1 + 1) / runB.L);
    rects.push([a, runB.y - runB.h * 0.22, b, runB.y + runB.h * 0.88]);
  };
  for (let i = s; i < e; i++) {
    const c = rec.chars[i];
    if (!c) { flush(); runB = null; continue; }
    if (c.b !== runB) { flush(); runB = c.b; k0 = c.k; }
    k1 = c.k;
  }
  flush();
  return rects;
}

async function run() {
  const q = input.value.trim().toLowerCase();
  matches = [];
  cur = -1;
  if (!q || !S.pdf) { draw(); return; }
  const token = ++running;
  count.textContent = '…';
  for (let src = 0; src < S.nPages; src++) {
    let rec;
    try { rec = await pageText(src); } catch { continue; }
    if (token !== running) return;
    let from = 0;
    let idx;
    while ((idx = rec.text.indexOf(q, from)) !== -1) {
      matches.push({ src, rects: rectsFor(rec, idx, idx + q.length) });
      from = idx + q.length;
    }
  }
  if (token !== running) return;
  cur = matches.length ? 0 : -1;
  draw();
  if (!matches.length) toast('No matches');
}

function draw() {
  document.querySelectorAll('.find-layer').forEach((f) => f.replaceChildren());
  if (cur < 0 || !matches[cur]) {
    count.textContent = matches.length ? `0/${matches.length}` : '0';
    return;
  }
  const m = matches[cur];
  const dispIdx = S.pageList.findIndex((p) => p.src === m.src);
  if (dispIdx < 0) { count.textContent = '0'; return; }
  count.textContent = `${cur + 1}/${matches.length}`;
  goToSrcPage(m.src);
  const wrap = document.querySelector(`.pagewrap[data-i="${dispIdx}"]`);
  const layer = wrap?.querySelector('.find-layer');
  if (!layer) return;
  const vp = viewportFor(dispIdx);
  if (!vp) return;
  const frag = document.createDocumentFragment();
  for (const r of m.rects) {
    const d = document.createElement('div');
    d.className = 'srch';
    const v = vp.convertToViewportRectangle(r);
    d.style.left = Math.min(v[0], v[2]) + 'px';
    d.style.top = Math.min(v[1], v[3]) + 'px';
    d.style.width = Math.abs(v[2] - v[0]) + 'px';
    d.style.height = Math.abs(v[3] - v[1]) + 'px';
    frag.appendChild(d);
  }
  layer.replaceChildren(frag);
}

function next(dir = 1) {
  if (!matches.length) return;
  cur = (cur + dir + matches.length) % matches.length;
  draw();
}

export function nav(dir = 1) {
  if (matches.length) next(dir);
  else run();
}

export function open(prefill = '') {
  active = true;
  box.classList.remove('hidden');
  input.value = prefill;
  input.focus();
  input.select();
}

export function close() {
  active = false;
  running++;
  box.classList.add('hidden');
  document.querySelectorAll('.find-layer').forEach((f) => f.replaceChildren());
}

export function isOpen() {
  return active;
}

export function init() {
  input.addEventListener('keydown', (e) => {
    e.stopPropagation();
    if (e.key === 'Enter') { e.preventDefault(); matches.length ? next(e.shiftKey ? -1 : 1) : run(); }
    else if (e.key === 'F3') { e.preventDefault(); matches.length ? next(e.shiftKey ? -1 : 1) : run(); }
    else if (e.key === 'Escape') close();
  });
  input.addEventListener('input', () => { clearTimeout(input._t); input._t = setTimeout(run, 350); });
  document.getElementById('search-next').addEventListener('click', () => (matches.length ? next(1) : run()));
  document.getElementById('search-prev').addEventListener('click', () => next(-1));
  document.getElementById('search-close').addEventListener('click', close);
  onDocChange(() => {
    cache.clear();
    close();
  });
  document.addEventListener('mnpdf:zoom', () => { if (active && matches[cur]) draw(); });
}
