// Pure geometry for drag-selection hit-testing/clamping (unit-testable, no DOM).

// A press only starts a selection when it lands on (or very near) a text
// glyph box; slop keeps line-margin presses working without letting true
// blank space (page gutters, margins below the last line) start one.
export const START_SLOP_X = 12;
export const START_SLOP_Y = 6;

export function pointNearRect(x, y, r, slopX = START_SLOP_X, slopY = START_SLOP_Y) {
  if (!r || r.width < 1 || r.height < 2) return false;
  return (
    x >= r.left - slopX && x <= r.right + slopX &&
    y >= r.top - slopY && y <= r.bottom + slopY
  );
}

// The part of a span rect covered by the pointer band, horizontally clipped
// to the band. Returns {sx, ex} or null when the band misses the span — the
// clamp: only text the band actually crosses is selected, never blank regions.
export function bandClipRect(r, by0, by1, bx0, bx1, slop = 6) {
  if (!r || r.width < 1 || r.height < 2) return null;
  if (r.bottom < by0 - slop || r.top > by1 + slop) return null;
  const sx = Math.max(bx0, r.left);
  const ex = Math.min(bx1, r.right);
  if (ex - sx < 1) return null;
  return { sx, ex };
}

// typical top-to-top distance of the given lines — the line pitch of the text.
// The median keeps one odd gap (a heading, a page break) from skewing it.
export function linePitch(lines) {
  const d = [];
  for (let i = 1; i < lines.length; i++) {
    const v = lines[i].top - lines[i - 1].top;
    if (v > 0) d.push(v);
  }
  if (!d.length) return 0;
  d.sort((a, b) => a - b);
  return d[d.length >> 1];
}

// ---- rows ----
// pdf.js splits one rendered text row into several spans (kerning, justified
// space gaps, bold lead-ins). Per-span selection entries break trims: the row
// trim clamps to the first span, drops as zero-width, and the remaining spans
// paint full-width — the highlight looks stuck no matter where the pointer
// goes, and pieces of the row fall out of the selection range. So spans whose
// boxes overlap vertically are merged into one row entry with a single shared
// offset space; trims, bridges and text slices then work per row.

export function sameRow(a, b) {
  const overlap = Math.min(a.bottom, b.bottom) - Math.max(a.top, b.top);
  const minH = Math.min(a.bottom - a.top, b.bottom - b.top);
  return overlap > minH * 0.45;
}

// ents: {left, right, top, bottom, start, end, text} in reading order, one per
// pdf.js span, in one shared vertical space. Returns rows: same box shape plus
// {parts: ents, offs: [{ent, cum}], text} where cum is each span's start index
// in the concatenated row text. All other properties of ent pass through
// untouched (annos.js keeps DOM references on them).
export function mergeRows(ents) {
  const rows = [];
  for (const ent of ents) {
    const last = rows[rows.length - 1];
    if (last && sameRow(last, ent)) {
      last.parts.push(ent);
      last.left = Math.min(last.left, ent.left);
      last.right = Math.max(last.right, ent.right);
      last.top = Math.min(last.top, ent.top);
      last.bottom = Math.max(last.bottom, ent.bottom);
      continue;
    }
    rows.push({ parts: [ent], left: ent.left, right: ent.right, top: ent.top, bottom: ent.bottom });
  }
  for (const r of rows) {
    let text = '', prev = null;
    r.offs = [];
    for (const p of r.parts) {
      // a wide horizontal gap between spans is a space the per-span texts
      // lost (spanVisibleBox trims their trailing whitespace)
      if (prev && p.left - prev.right > (r.bottom - r.top) * 0.25) text += ' ';
      r.offs.push({ ent: p, cum: text.length });
      text += p.text;
      prev = p;
    }
    r.start = 0;
    r.end = text.length;
    r.text = text;
  }
  return rows;
}

// pointer x -> offset inside one span's own text (proportional glyph
// fraction; the 2px edge slop keeps margin presses at the span boundaries)
export function spanOffsetAtX(b, x) {
  if (b.right <= b.left) return b.start;
  if (x <= b.left + 2) return b.start;
  if (x >= b.right - 2) return b.end;
  const frac = (x - b.left) / (b.right - b.left);
  return Math.max(b.start, Math.min(b.end, b.start + Math.round(frac * (b.end - b.start))));
}

// offset inside one span's own text -> x
export function spanXOf(b, off) {
  if (b.end <= b.start) return b.left;
  const frac = (off - b.start) / (b.end - b.start);
  return b.left + frac * (b.right - b.left);
}

// pointer x -> offset in the merged row text: the span under the pointer (or
// the nearest one, in inter-span gaps and at row edges) contributes its local
// offset. localOff(box, x) maps within one span's own text space.
export function rowOffsetAtX(row, x, localOff) {
  if (x <= row.left + 2) return 0;
  if (x >= row.right - 2) return row.text.length;
  let best = null, bestD = Infinity;
  for (const o of row.offs) {
    const b = o.ent;
    if (x >= b.left && x <= b.right) { best = o; break; }
    const d = x < b.left ? b.left - x : x - b.right;
    if (d < bestD) { bestD = d; best = o; }
  }
  const b = best.ent;
  const lx = Math.max(b.left, Math.min(b.right, x));
  return best.cum + (localOff(b, lx) - b.start);
}

// offset in the merged row text -> x. localX(box, off) maps one span's own
// text offset to x. `side` resolves a boundary offset that sits exactly at a
// span's end: 'start' (a selection's left edge) takes the NEXT span's first
// glyph box, 'end' (a selection's right edge) takes the previous glyph's
// right edge — pdf.js splits leave inter-span gaps, so the two differ.
export function rowXOf(row, off, localX, side = 'end') {
  const first = row.offs[0], last = row.offs[row.offs.length - 1];
  if (off <= 0) return localX(first.ent, first.ent.start); // first glyph's box
  if (off >= row.text.length) return localX(last.ent, last.ent.end);
  let best = last;
  for (const o of row.offs) {
    const oEnd = o.cum + o.ent.text.length;
    if (side === 'start' ? off < oEnd : off <= oEnd) { best = o; break; }
  }
  const local = Math.max(0, Math.min(off - best.cum, best.ent.text.length));
  return localX(best.ent, best.ent.start + local);
}

// How much inter-line whitespace a selected block fills so it reads as one
// continuous highlight: line leading plus normal paragraph breaks (a blank
// line, paragraph spacing) are bridged; anything several line pitches tall
// (figures, margins, page gaps) stays unpainted.
export const BRIDGE_PITCHES = 4;

// Per-line segments between two anchors, ordered top to bottom: boundary
// lines are trimmed to the pointer character, middle lines are full glyphs.
// Which offset trims the first line depends on the drag direction — an upward
// drag has the focus above the anchor, so the focus offset belongs to the top
// line and the anchor offset to the bottom line.
//
// Line entries need {left, right, top, bottom, start, end, xOf(off)} in one
// shared vertical space (mnpdf uses scroller-document y); `pageIdx` is the
// group segments may bridge within. Returns {line, sx, ex, sOff, eOff,
// docTop, docBottom} with sOff <= eOff, docTop/docBottom already bridged.
export function buildSegments(lines, iA, offA, iF, offF) {
  const fi = Math.min(iA, iF), li = Math.max(iA, iF);
  const downward = iA <= iF;
  const topOff = downward ? offA : offF;
  const botOff = downward ? offF : offA;
  const pitch = linePitch(lines);
  const segs = [];
  for (let i = fi; i <= li; i++) {
    const line = lines[i];
    let sx = line.left, sOff = line.start, ex = line.right, eOff = line.end;
    if (i === fi) { sx = line.xOf(topOff, 'start'); sOff = topOff; }
    if (i === li) { ex = line.xOf(botOff, 'end'); eOff = botOff; }
    if (sx > ex) { [sx, ex] = [ex, sx]; [sOff, eOff] = [eOff, sOff]; }
    if (ex - sx < 0.5) continue;
    segs.push({ line, sx, ex, sOff, eOff, docTop: line.top, docBottom: line.bottom });
  }
  // each segment extends up to the previous one so the block is continuous —
  // capped at a few line pitches so figures, margins and page gaps stay
  // unpainted
  for (let i = 1; i < segs.length; i++) {
    const prev = segs[i - 1], cur = segs[i];
    if (prev.line.pageIdx !== cur.line.pageIdx) continue;
    const gap = cur.docTop - prev.docBottom;
    const bridge = pitch > 0 ? pitch * BRIDGE_PITCHES : (prev.docBottom - prev.docTop) * 1.5;
    if (gap > 0 && gap < bridge) cur.docTop = prev.docBottom;
  }
  return segs;
}
