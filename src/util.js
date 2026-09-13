// Small shared helpers. No framework, no build magic.

export function uid() {
  return crypto.randomUUID?.() || 'id-' + Math.random().toString(36).slice(2) + Date.now().toString(36);
}

export function clamp(v, a, b) {
  return Math.min(b, Math.max(a, v));
}

export function el(tag, cls, parent) {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (parent) parent.appendChild(e);
  return e;
}

export function debounce(fn, ms) {
  let t = null;
  return (...a) => {
    clearTimeout(t);
    t = setTimeout(() => fn(...a), ms);
  };
}

// base64 <-> Uint8Array (chunked; PDFs can be tens of MB)
export function u8ToB64(u8) {
  let s = '';
  const CH = 0x8000;
  for (let i = 0; i < u8.length; i += CH) {
    s += String.fromCharCode.apply(null, u8.subarray(i, i + CH));
  }
  return btoa(s);
}

export function b64ToU8(b64) {
  const s = atob(b64);
  const u = new Uint8Array(s.length);
  for (let i = 0; i < s.length; i++) u[i] = s.charCodeAt(i);
  return u;
}

export function hexRgb(hex) {
  const n = parseInt((hex || '#000').slice(1), 16);
  return [(n >> 16) & 255, (n >> 8) & 255, n & 255];
}

let toastTimer = null;
export function toast(msg, ms = 1600) {
  const t = document.getElementById('toast');
  t.textContent = msg;
  t.classList.remove('hidden');
  clearTimeout(toastTimer);
  if (ms > 0) toastTimer = setTimeout(() => t.classList.add('hidden'), ms);
}
