import { test } from 'node:test';
import assert from 'node:assert/strict';
import { pointNearRect, bandClipRect, buildSegments, linePitch } from '../src/selection.js';

const line = (left, top, right, bottom) => ({ left, top, right, bottom, width: right - left, height: bottom - top });

test('pointNearRect: press on a glyph box starts a selection', () => {
  assert.equal(pointNearRect(50, 10, line(0, 0, 100, 14)), true);
});

test('pointNearRect: press in true blank space (page margin) is ignored', () => {
  // below the last line of the page, far from any span
  assert.equal(pointNearRect(50, 500, line(0, 0, 100, 14)), false);
  // left gutter, far from the line start
  assert.equal(pointNearRect(-80, 7, line(0, 0, 100, 14)), false);
});

test('pointNearRect: small slop still allows line-margin presses', () => {
  assert.equal(pointNearRect(-8, 7, line(0, 0, 100, 14)), true); // within X slop
  assert.equal(pointNearRect(50, 18, line(0, 0, 100, 14)), true); // within Y slop
  assert.equal(pointNearRect(50, 24, line(0, 0, 100, 14)), false); // past Y slop
});

test('pointNearRect: degenerate rects (empty/marked-content spans) never hit', () => {
  assert.equal(pointNearRect(50, 10, line(50, 0, 50, 30)), false); // zero width
  assert.equal(pointNearRect(50, 10, line(0, 10, 100, 10.5)), false); // zero height
});

test('bandClipRect: band crossing a mid-span clips horizontally to the band', () => {
  const clip = bandClipRect(line(0, 0, 100, 14), -5, 20, 30, 70);
  assert.deepEqual(clip, { sx: 30, ex: 70 });
});

test('bandClipRect: band beyond the last line does not reach the span (clamp)', () => {
  // drag-to-blank: band ends far below the final text line -> no selection
  assert.equal(bandClipRect(line(0, 0, 100, 14), 200, 260, 0, 100), null);
});

test('bandClipRect: drag-from-blank with band entirely left of text selects nothing', () => {
  assert.equal(bandClipRect(line(50, 0, 150, 14), -5, 20, 0, 40), null);
});

test('bandClipRect: adjacent-line slop keeps an in-text drag inclusive', () => {
  // band bottom just above the next line (within slop) still includes it
  assert.ok(bandClipRect(line(0, 20, 100, 34), 0, 18, 0, 100));
  // one pixel beyond the slop it is dropped
  assert.equal(bandClipRect(line(0, 20, 100, 34), 0, 13, 0, 100), null);
});

test('bandClipRect: degenerate spans are skipped', () => {
  assert.equal(bandClipRect(line(50, 0, 50, 30), -5, 40, 0, 100), null);
  assert.equal(bandClipRect(line(0, 10, 100, 10.5), -5, 40, 0, 100), null);
});

// ---- buildSegments: drag direction, boundary trims, continuity bridge ----

// a row of text: glyph box [left,right]x[top,top+20], offsets 0..10 spread
// linearly across the glyphs, at scroller-document y `top`
const row = (top, left = 0, right = 100, pageIdx = 0) => ({
  left, right, top, bottom: top + 20, start: 0, end: 10, pageIdx,
  xOf: (off) => left + (off / 10) * (right - left),
});

// five rows at pitch 30 with one figure-sized hole before the last row
const rows = [row(0), row(30), row(60), row(200), row(230)];

const spans = (segs) => segs.map((s) => [s.sx, s.ex, s.sOff, s.eOff]);

test('linePitch: median of the top deltas ignores one big gap', () => {
  assert.equal(linePitch(rows), 30);
  assert.equal(linePitch([row(0)]), 0);
});

test('buildSegments: downward drag trims the top line at the anchor, bottom at the focus', () => {
  // press mid row1 (offset 5 -> x 50), drag to row3 (offset 3 -> x 30)
  const segs = buildSegments(rows, 1, 5, 3, 3);
  assert.deepEqual(spans(segs), [
    [50, 100, 5, 10], // top row: anchor .. end of line
    [0, 100, 0, 10],  // middle row: full glyphs
    [0, 30, 0, 3],    // bottom row: line start .. focus
  ]);
});

test('buildSegments: upward drag mirrors the trims (top row right of the focus)', () => {
  // same endpoints, dragged upward: anchor on row3, focus on row1
  const segs = buildSegments(rows, 3, 3, 1, 5);
  assert.deepEqual(spans(segs), [
    [50, 100, 5, 10], // top (focus) row: focus .. end of line
    [0, 100, 0, 10],
    [0, 30, 0, 3],    // bottom (anchor) row: line start .. anchor
  ]);
});

test('buildSegments: upward and downward drags over the same endpoints agree', () => {
  const down = spans(buildSegments(rows, 1, 5, 3, 3));
  const up = spans(buildSegments(rows, 3, 3, 1, 5));
  assert.deepEqual(up, down);
});

test('buildSegments: same-row drag is the piece between the two pointers, either way', () => {
  assert.deepEqual(spans(buildSegments([rows[1]], 0, 2, 0, 8)), [[20, 80, 2, 8]]);
  assert.deepEqual(spans(buildSegments([rows[1]], 0, 8, 0, 2)), [[20, 80, 2, 8]]);
});

test('buildSegments: word-mode drag on one row keeps the word span', () => {
  assert.deepEqual(spans(buildSegments([rows[1]], 0, 2, 0, 6)), [[20, 60, 2, 6]]);
});

test('buildSegments: boundary row trimmed past its far edge is dropped', () => {
  // focus at the very start of the last row -> zero-width bottom segment
  const segs = buildSegments(rows, 0, 0, 3, 0);
  assert.equal(segs.length, 3);
  assert.equal(segs[segs.length - 1].ex, 100); // the block ends at row2's right edge
});

test('buildSegments: bridge closes leading and paragraph breaks, not figures', () => {
  // rows 0-2 are 30 apart with 10 of whitespace: bridged into one block
  const tight = buildSegments(rows, 0, 0, 2, 10);
  assert.equal(tight[1].docTop, tight[0].docBottom);
  assert.equal(tight[2].docTop, tight[1].docBottom);
  // the 140px hole (4+ pitches) before row 3 stays unpainted
  const across = buildSegments(rows, 0, 0, 3, 10);
  assert.equal(across[3].docTop, 200);
  assert.ok(across[3].docTop - across[2].docBottom > 100);
});

test('buildSegments: never bridges across pages', () => {
  const twoPages = [row(0, 0, 100, 0), row(30, 0, 100, 0), row(0, 0, 100, 1)];
  const segs = buildSegments(twoPages, 0, 0, 2, 10);
  assert.equal(segs[2].docTop, 0); // the page gap is not filled
});
