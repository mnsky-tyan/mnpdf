// Minimal custom context menu. Items:
//   {label, hint, danger, fn} | {swatches: ['#..'], fn(color)} | 'sep'
import { el } from './util.js';

let root = null;

export function closeMenu() {
  if (root) { root.remove(); root = null; }
}

export function menuOpen() {
  return !!root;
}

export function showMenu(x, y, items) {
  closeMenu();
  root = el('div', 'menu');
  for (const it of items) {
    if (it === 'sep' || it?.sep) { el('div', 'menu-sep', root); continue; }
    if (it.swatches) {
      const row = el('div', 'menu-swatches', root);
      for (const c of it.swatches) {
        const b = el('button', 'sw');
        b.style.background = c;
        b.title = c;
        b.addEventListener('click', (ev) => { ev.stopPropagation(); const f = it.fn; closeMenu(); f(c); });
        row.appendChild(b);
      }
      continue;
    }
    const d = el('div', 'menu-item');
    const lab = el('span', 'menu-label'); lab.textContent = it.label; d.appendChild(lab);
    if (it.hint) { const h = el('span', 'menu-hint'); h.textContent = it.hint; d.appendChild(h); }
    if (it.danger) d.classList.add('danger');
    d.addEventListener('click', () => { const f = it.fn; closeMenu(); f(); });
    root.appendChild(d);
  }
  document.body.appendChild(root);
  const r = root.getBoundingClientRect();
  root.style.left = Math.max(6, Math.min(x, window.innerWidth - r.width - 8)) + 'px';
  root.style.top = Math.max(6, Math.min(y, window.innerHeight - r.height - 8)) + 'px';
}

export function init() {
  window.addEventListener('pointerdown', (e) => {
    if (root && !root.contains(e.target)) closeMenu();
  }, true);
  window.addEventListener('keydown', (e) => {
    if (e.key === 'Escape') closeMenu();
  }, true);
  window.addEventListener('blur', closeMenu);
  document.getElementById('scroller').addEventListener('scroll', closeMenu, { passive: true });
}
