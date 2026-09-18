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
    if (i === fi) { sx = line.xOf(topOff); sOff = topOff; }
    if (i === li) { ex = line.xOf(botOff); eOff = botOff; }
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
