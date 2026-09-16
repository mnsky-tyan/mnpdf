// Pure geometry for drag-selection hit-testing/clamping (unit-testable, no DOM).

// A press only starts a selection when it lands on (or very near) a text
// glyph box; slop keeps line-margin presses working without letting true
// blank space (page gutters, margins below the last line) start one.
export const START_SLOP_X = 12;
export const START_SLOP_Y = 6;

// Resolve the span under a document-space y coordinate. pdf.js can split one
// visual line into several spans with the same vertical box, so use pointer x
// to select the actual span instead of returning the first vertical match.
export function lineAt(lines, docY, x) {
  if (!lines.length) return null;
  let best = null;
  let bestY = Infinity;
  let bestX = Infinity;
  for (const line of lines) {
    const centerY = (line.docTop + line.docBottom) / 2;
    const yDistance = docY >= line.docTop - 2 && docY <= line.docBottom + 2
      ? 0
      : Math.abs(docY - centerY);
    const vis = line.vis;
    const xDistance = x == null || !vis
      ? 0
      : x < vis.left ? vis.left - x : x > vis.right ? x - vis.right : 0;
    if (yDistance < bestY || (yDistance === bestY && xDistance < bestX)) {
      best = line;
      bestY = yDistance;
      bestX = xDistance;
    }
  }
  return best;
}

// Character offsets painted for one line between an anchor and focus. A
// same-line drag has two boundaries, regardless of drag direction.
export function selectionOffsetsForLine(i, iA, offA, iF, offF, lineStart, lineEnd) {
  if (iA === iF) {
    return { start: Math.min(offA, offF), end: Math.max(offA, offF) };
  }
  const first = Math.min(iA, iF);
  const last = Math.max(iA, iF);
  if (i === first) {
    return iA < iF
      ? { start: offA, end: lineEnd }
      : { start: lineStart, end: offF };
  }
  if (i === last) {
    return iA < iF
      ? { start: lineStart, end: offF }
      : { start: offA, end: lineEnd };
  }
  return { start: lineStart, end: lineEnd };
}

// Word-boundary snapping over the trimmed span slice text, whose characters
// live at full-span offsets [start, start + text.length). A focus on
// inter-word whitespace snaps so the selection never collapses to a gap: an
// end boundary keeps the trailing gap (a rightward drag grows through it), a
// start boundary skips forward to the next word (a leftward drag never
// selects whitespace alone).
export function snapWordEnd(text, start, off) {
  let i = Math.max(0, Math.min(text.length, off - start));
  while (i > 0 && !/\s/.test(text[i - 1])) i--;
  while (i < text.length && !/\s/.test(text[i])) i++;
  return start + i;
}

export function snapWordStart(text, start, off) {
  let i = Math.max(0, Math.min(text.length, off - start));
  while (i < text.length && /\s/.test(text[i])) i++;
  while (i > 0 && !/\s/.test(text[i - 1])) i--;
  return start + i;
}

// Word-mode (double-click-drag) boundaries between the anchor word
// [aStart, aEnd) on line iA and the word under the focus offset on line iF
// (text/start describe the focus span's trimmed slice). Returns the
// document-ordered [startLine, startOff, endLine, endOff] span for
// selectionOffsetsForLine: whole words throughout, so each boundary line
// keeps its word and every line in between is painted in full.
export function wordModeSpan(iA, aStart, aEnd, iF, text, start, off) {
  if (iA === iF) {
    if (off < aStart) return [iA, snapWordStart(text, start, off), iA, aEnd];
    return [iA, aStart, iA, snapWordEnd(text, start, off)];
  }
  if (iF > iA) return [iA, aStart, iF, snapWordEnd(text, start, off)];
  return [iF, snapWordStart(text, start, off), iA, aEnd];
}

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
