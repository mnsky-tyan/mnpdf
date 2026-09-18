// Central document state + undo/redo + a tiny change bus.
import { uid } from './util.js';

export const S = {
  path: null,          // file path (string) — null when nothing is open
  name: null,          // display name
  pdf: null,           // pdf.js document
  bytes: null,         // pristine bytes of the file on disk
  nPages: 0,
  view: [],            // per original page: page.view (cropbox)
  unit: [],            // per original page: userUnit
  baseRot: [],         // per original page: page.rotate
  pageList: [],        // [{src, rot}] display order + extra rotation
  anns: [],            // annotations: {id, type:'hl'|'note', page:srcIdx, ...}
  undo: [],
  redo: [],
  dirty: false,
  zoom: 1,
  selAnnId: null,
  hlColor: '#ffe680',
  titlebar: false,      // optional draggable title bar (right-click toggle)
};

const listeners = new Set();
export function onDocChange(fn) {
  listeners.add(fn);
}
export function emitChange(what = {}) {
  for (const fn of listeners) {
    try { fn(what); } catch (e) { console.error(e); }
  }
}

export function markDirty() {
  if (!S.dirty) {
    S.dirty = true;
    emitChange({ title: true });
  }
}

export function pushOp(op) {
  applyOp(op, true);
  S.undo.push(op);
  S.redo.length = 0;
  markDirty();
  emitChange(op);
}

export function undo() {
  const op = S.undo.pop();
  if (!op) return;
  applyOp(op, false);
  S.redo.push(op);
  markDirty();
  emitChange({ undid: op });
}

export function redo() {
  const op = S.redo.pop();
  if (!op) return;
  applyOp(op, true);
  S.undo.push(op);
  markDirty();
  emitChange({ redid: op });
}

function applyOp(op, fwd) {
  switch (op.kind) {
    case 'add':
      if (fwd) S.anns.push(op.ann);
      else S.anns = S.anns.filter((a) => a.id !== op.ann.id);
      break;
    case 'hlreplace':
      // new highlight replaces every highlight it overlaps (one undo step)
      if (fwd) {
        const gone = new Set(op.replaced.map((a) => a.id));
        S.anns = S.anns.filter((a) => !gone.has(a.id));
        S.anns.push(op.ann);
      } else {
        S.anns = S.anns.filter((a) => a.id !== op.ann.id);
        S.anns.push(...op.replaced);
      }
      break;
    case 'del':
      if (fwd) S.anns = S.anns.filter((a) => a.id !== op.ann.id);
      else S.anns.push(op.ann);
      break;
    case 'move': {
      const a = S.anns.find((x) => x.id === op.id);
      if (a) { a.x = fwd ? op.to.x : op.from.x; a.y = fwd ? op.to.y : op.from.y; }
      break;
    }
    case 'text': {
      const a = S.anns.find((x) => x.id === op.id);
      if (a) a.text = fwd ? op.to : op.from;
      break;
    }
    case 'addmany':
      if (fwd) S.anns.push(...op.anns);
      else S.anns = S.anns.filter((a) => !op.anns.some((x) => x.id === a.id));
      break;
    case 'hlreplacemany': {
      // multi-page drag highlight: new highlights replace everything they
      // overlap, as one undo step
      if (fwd) {
        const gone = new Set();
        op.replaced.forEach((group) => group.forEach((a) => gone.add(a.id)));
        S.anns = S.anns.filter((a) => !gone.has(a.id));
        S.anns.push(...op.anns);
      } else {
        const ids = new Set(op.anns.map((a) => a.id));
        S.anns = S.anns.filter((a) => !ids.has(a.id));
        op.replaced.forEach((group) => S.anns.push(...group));
      }
      break;
    }
    case 'recolor': {
      const a = S.anns.find((x) => x.id === op.id);
      if (a) a.color = fwd ? op.to : op.from;
      break;
    }
    case 'pages':
      S.pageList = (fwd ? op.after : op.before).map((p) => ({ ...p }));
      break;
  }
}

export function newHighlight(srcIdx, rects, color, text = '') {
  return { id: uid(), type: 'hl', page: srcIdx, rects, color, text };
}

export function newPin(srcIdx, x, y) {
  return { id: uid(), type: 'pin', page: srcIdx, x, y, text: '' };
}
