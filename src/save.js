// Saving: bakes highlights + page ops into the PDF with pdf-lib.
// Edits are applied in place on the original document (preserves bookmarks);
// if low-level page-tree surgery fails, falls back to copyPages rebuild.
import { PDFDocument, degrees, rgb, PDFName } from 'pdf-lib';
import { S } from './state.js';
import { hexRgb } from './util.js';

export async function bake() {
  let doc;
  try {
    doc = await PDFDocument.load(S.bytes, { ignoreEncryption: true, throwOnInvalidObject: false });
  } catch (e) {
    throw new Error('cannot parse PDF for saving: ' + (e?.message || e));
  }
  const pages = doc.getPages();
  const refOf = new Map(pages.map((p, i) => [i, p.ref]));

  // 1. bake highlights only — pins are an app-layer sidecar feature (their text
  // is hover-only, so it must never be printed onto the page)
  for (const a of S.anns) {
    if (a.type !== 'hl') continue;
    const page = pages[a.page];
    if (!page) continue;
    const [r, g, b] = hexRgb(a.color);
    for (const [x1, y1, x2, y2] of a.rects) {
      page.drawRectangle({
        x: Math.min(x1, x2),
        y: Math.min(y1, y2),
        width: Math.abs(x2 - x1) || 1,
        height: Math.abs(y2 - y1) || 1,
        color: rgb(r / 255, g / 255, b / 255),
        opacity: 0.4,
      });
    }
  }

  // 2. page rotations
  for (const p of S.pageList) {
    const page = pages[p.src];
    if (!page) continue;
    const total = (((page.getRotation().angle ?? 0) + p.rot) % 360 + 360) % 360;
    page.setRotation(degrees(total));
  }


  // 3. delete + reorder pages, in place to keep the outline intact
  const wanted = S.pageList.map((p) => p.src);
  const keep = new Set(wanted);
  try {
    for (let i = pages.length - 1; i >= 0; i--) {
      if (!keep.has(i)) doc.removePage(i);
    }
    const pageTree = doc.catalog.Pages();
    const kids = wanted.map((s) => refOf.get(s));
    pageTree.set(PDFName.of('Kids'), doc.context.obj(kids));
    pageTree.set(PDFName.of('Count'), doc.context.obj(kids.length));
  } catch (e) {
    console.warn('in-place page ops failed, rebuilding via copyPages', e);
    const sorted = [...wanted].sort((a, b) => a - b);
    const idxOf = new Map(sorted.map((s, k) => [s, k]));
    const out = await PDFDocument.create();
    const copied = await out.copyPages(doc, wanted.map((s) => idxOf.get(s)));
    copied.forEach((p) => out.addPage(p));
    doc = out;
  }

  return doc.save();
}
