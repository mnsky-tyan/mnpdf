// Thin platform bridge: Tauri desktop (real file IO) with a browser fallback
// (vite dev / automated testing). Everything downstream only sees this API.
import { b64ToU8, u8ToB64, el } from './util.js';

const isTauri = typeof window.__TAURI_INTERNALS__ !== 'undefined';

let impl;

if (isTauri) {
  const { invoke } = await import('@tauri-apps/api/core');
  const dlg = await import('@tauri-apps/plugin-dialog');
  const { getCurrentWebview } = await import('@tauri-apps/api/webview');
  const { getCurrentWindow } = await import('@tauri-apps/api/window');
  const win = getCurrentWindow();

  impl = {
    kind: 'tauri',
    async read(path) {
      return b64ToU8(await invoke('read_file_b64', { path }));
    },
    async write(path, u8) {
      await invoke('write_file_b64', { path, data: u8ToB64(u8) });
    },
    async openDialog(defaultPath) {
      return dlg.open({
        multiple: false,
        defaultPath: defaultPath || undefined,
        filters: [{ name: 'PDF', extensions: ['pdf'] }],
      });
    },
    async saveDialog(defaultPath) {
      return dlg.save({
        defaultPath: defaultPath || undefined,
        filters: [{ name: 'PDF', extensions: ['pdf'] }],
      });
    },
    ask: (msg) => dlg.ask(msg, { title: 'mnpdf' }),
    kvGet: (k) => invoke('kv_get', { key: k }),
    kvSet: (k, v) => invoke('kv_set', { key: k, value: v }),
    initialPath: () => invoke('initial_path'),
    onDrop(cb) {
      getCurrentWebview().onDragDropEvent((e) => {
        const p = e.payload;
        if (p.type === 'drop') {
          const pdf = (p.paths || []).find((f) => f.toLowerCase().endsWith('.pdf'));
          if (pdf) cb(pdf);
        }
      });
    },
    onCloseGuard(fn) {
      win.onCloseRequested(fn);
    },
    minimize: () => win.minimize(),
    toggleMaximize: () => win.toggleMaximize(),
    closeWindow: () => win.close(),
    setTitle(t) {
      document.title = t;
      win.setTitle(t).catch(() => {});
    },
  };
} else {
  // Browser fallback: localStorage store, <input type=file>, download-as-save.
  const fileInput = el('input');
  fileInput.type = 'file';
  fileInput.accept = 'application/pdf,.pdf';
  fileInput.style.display = 'none';
  document.documentElement.appendChild(fileInput);

  impl = {
    kind: 'web',
    async read(path) {
      // path may be a URL like "sample.pdf" or a blob: URL (used by tests)
      const r = await fetch(path);
      if (!r.ok) throw new Error(`fetch ${path}: ${r.status}`);
      return new Uint8Array(await r.arrayBuffer());
    },
    async write(name, u8) {
      const blob = new Blob([u8], { type: 'application/pdf' });
      const a = el('a');
      a.href = URL.createObjectURL(blob);
      a.download = (name || 'mnpdf.pdf').split(/[\\/]/).pop();
      a.click();
      setTimeout(() => URL.revokeObjectURL(a.href), 5000);
    },
    async openDialog() {
      return new Promise((resolve) => {
        fileInput.onchange = () => {
          const f = fileInput.files[0];
          fileInput.value = '';
          resolve(f || null);
        };
        fileInput.click();
      });
    },
    async saveDialog(hint) {
      const n = prompt('Save as file name:', hint || 'document.pdf');
      return n ? n.trim() : null;
    },
    ask: (msg) => Promise.resolve(confirm(msg)),
    kvGet: (k) => Promise.resolve(localStorage.getItem('k:' + k)),
    kvSet: (k, v) => {
      localStorage.setItem('k:' + k, v);
      return Promise.resolve();
    },
    async initialPath() {
      return new URLSearchParams(location.search).get('file');
    },
    onDrop(cb) {
      window.addEventListener('dragover', (e) => e.preventDefault());
      window.addEventListener('drop', (e) => {
        e.preventDefault();
        const f = [...(e.dataTransfer?.files || [])].find((f) =>
          f.name.toLowerCase().endsWith('.pdf'),
        );
        if (f) cb(f);
      });
    },
    onCloseGuard() {},
    minimize() {},
    toggleMaximize() {},
    closeWindow() {
      window.close();
    },
    setTitle(t) {
      document.title = t;
    },
  };
}

export default impl;
