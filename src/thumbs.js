// Thumbnail sidebar: lazy mini-renders, click to jump, drag to reorder.
import { S, pushOp, onDocChange } from './state.js';
import { goToDisplayPage, currentPageIdx, ensurePage } from './viewer.js';
import { el } from './util.js';

const panel = document.getElementById('thumbs');
let io = null;
let dragI = null;

export function visible() {
  return !panel.classList.contains('hidden');
}

export function toggle(force) {
  const show = force ?? !visible();
  panel.classList.toggle('hidden', !show);
  if (show) refresh(true);
}

async function renderThumb(item, i) {
  if (!S.pdf || !S.pageList[i]) return;
  const { src, rot } = S.pageList[i];
  const key = `${src}|${rot}`;
  if (item.dataset.key === key) return;
  item.dataset.key = key;
  const canvas = item.querySelector('canvas');
  try {
    const page = await ensurePage(src);
    if (item.dataset.key !== key) return;
    const total = (((page.rotate + rot) % 360) + 360) % 360;
    const v1 = page.getViewport({ scale: 1, rotation: total });
    const scale = 118 / v1.width;
    const vp = page.getViewport({ scale, rotation: total });
    canvas.width = Math.floor(vp.width);
    canvas.height = Math.floor(vp.height);
    await page.render({ canvasContext: canvas.getContext('2d'), viewport: vp }).promise;
  } catch (e) {
    if (!/cancel/i.test(String(e?.name || e))) console.warn('thumb', e);
    item.dataset.key = '';
  }
}

export function setActive(i) {
  [...panel.children].forEach((c, k) => c.classList.toggle('active', k === i));
}

export function refresh(force = false) {
  if (!S.pdf) { panel.replaceChildren(); return; }
  if (!force && panel.childElementCount === S.pageList.length) {
    // page list unchanged in length: just re-render keys (rotations may differ)
    [...panel.children].forEach((item, i) => renderThumb(item, i));
    setActive(currentPageIdx());
    return;
  }
  if (io) io.disconnect();
  panel.replaceChildren();
  io = new IntersectionObserver(
    (entries) => {
      for (const en of entries) {
        if (en.isIntersecting) renderThumb(en.target, +en.target.dataset.i);
      }
    },
    { root: panel, rootMargin: '260px' },
  );
  S.pageList.forEach((_, i) => {
    const item = el('div', 'thumb');
    item.dataset.i = i;
    item.draggable = true;
    el('canvas', '', item);
    const lab = el('span', 'tlabel', item);
    lab.textContent = i + 1;
    panel.appendChild(item);
    io.observe(item);
  });
  setActive(currentPageIdx());
}

export function menuItems(itemEl, ctx) {
  const i = +itemEl.dataset.i;
  const entry = S.pageList[i];
  if (!entry) return [];
  return [
    { label: 'Rotate clockwise', fn: () => ctx.rotate(i, 90) },
    { label: 'Rotate counter-clockwise', fn: () => ctx.rotate(i, -90) },
    'sep',
    { label: 'Delete page', danger: true, fn: () => ctx.del(i) },
  ];
}

function initDrag() {
  let marker = null;
  const clearMarker = () => { marker?.remove(); marker = null; };
  panel.addEventListener('dragstart', (e) => {
    const it = e.target.closest('.thumb');
    if (!it) return;
    dragI = +it.dataset.i;
    e.dataTransfer.effectAllowed = 'move';
    try { e.dataTransfer.setData('text/plain', String(dragI)); } catch {}
  });
  panel.addEventListener('dragover', (e) => {
    const it = e.target.closest('.thumb');
    if (!it || dragI == null) return;
    e.preventDefault();
    e.dataTransfer.dropEffect = 'move';
    if (!marker) { marker = el('div', 'thumb-marker'); panel.appendChild(marker); }
    const ref = +it.dataset.i;
    const rect = it.getBoundingClientRect();
    const before = e.clientY < rect.top + rect.height / 2;
    const target = before ? ref : ref + 1;
    marker.dataset.at = target;
    const refEl = target >= panel.childElementCount ? null : panel.children[target];
    panel.insertBefore(marker, refEl);
  });
  panel.addEventListener('drop', (e) => {
    e.preventDefault();
    const it = e.target.closest('.thumb');
    const at = marker ? +marker.dataset.at : null;
    clearMarker();
    if (dragI == null || at == null) return;
    let to = at;
    if (to > dragI) to--;
    if (to === dragI) return;
    const before = S.pageList.map((p) => ({ ...p }));
    const [moved] = before.splice(dragI, 1);
    before.splice(to, 0, moved);
    pushOp({ kind: 'pages', before, after: before });
  });
  panel.addEventListener('dragend', () => { dragI = null; clearMarker(); });
  panel.addEventListener('dragleave', (e) => { if (e.target === panel) clearMarker(); });
}

export function init(ctx) {
  initDrag();
  panel.addEventListener('click', (e) => {
    const it = e.target.closest('.thumb');
    if (!it) return;
    goToDisplayPage(+it.dataset.i);
  });
  document.addEventListener('mnpdf:scroll', () => {
    if (visible()) setActive(currentPageIdx());
  });
  onDocChange((what) => {
    if (visible()) refresh(what?.kind === 'pages');
  });
}
