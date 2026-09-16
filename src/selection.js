// Pure geometry for drag-selection hit-testing/clamping (unit-testable, no DOM).

// A press only starts a selection when it lands on (or very near) a text
// glyph box; slop keeps line-margin presses working without letting true
// blank space (page gutters, margins below the last line) start one.
export const START_SLOP_X = 12;
export const START_SLOP_Y = 6;

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

// Word-boundary snapping over text[start, end). A focus on inter-word
// whitespace snaps so the selection never collapses to a gap: an end boundary
// keeps the trailing gap (a rightward drag grows through it), a start
// boundary skips forward to the next word (a leftward drag never selects
// whitespace alone).
export function snapWordEnd(text, start, end, off) {
  let i = Math.max(start, Math.min(end, off));
  while (i > start && !/\s/.test(text[i - 1])) i--;
  while (i < end && !/\s/.test(text[i])) i++;
  return i;
}

export function snapWordStart(text, start, end, off) {
  let i = Math.max(start, Math.min(end, off));
  while (i < end && /\s/.test(text[i])) i++;
  while (i > start && !/\s/.test(text[i - 1])) i--;
  return i;
}

// Word-mode (double-click-drag) offsets for one apply: the anchor word spans
// [aStart, aEnd) and rel is the focus line relative to the anchor line
// (-1 above, 0 same line, +1 below). The focus snaps to a word boundary on
// the far side of the drag so whole words are selected in either direction;
// on the same line the direction comes from the raw focus offset against the
// anchor word start, and a leftward drag ends at the stored anchor word end.
// Returns the [anchorOff, focusOff] pair for selectionOffsetsForLine.
export function wordModeOffsets(rel, aStart, aEnd, text, start, end, off) {
  if (rel === 0 && off < aStart) return [snapWordStart(text, start, end, off), aEnd];
  return [aStart, rel < 0 ? snapWordStart(text, start, end, off) : snapWordEnd(text, start, end, off)];
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
