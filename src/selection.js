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
