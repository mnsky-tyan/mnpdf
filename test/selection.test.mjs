import { test } from 'node:test';
import assert from 'node:assert/strict';
import { pointNearRect, bandClipRect, buildSegments, linePitch, mergeRows, rowOffsetAtX, rowXOf, spanOffsetAtX, spanXOf } from '../src/selection.js';

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

// ---- rows: merging split pdf.js spans + row-space offset mapping ----

// a pdf.js span entry: glyph box [left,right]x[top,top+20] with its own
// trimmed text occupying [start,end] of its text node
const span = (left, right, top, text, start = 0) => ({
  left, right, top, bottom: top + 20, start, end: start + text.length, text,
});

// a split visual row like the ARC report's: 'totype.' + space span +
// 'Comparisons…' (wide justify gap before the last span)
const splitRow = [
  span(117.6, 172.3, 100, 'totype.'),
  span(172.3, 178, 100, ' '),
  span(187.5, 882.4, 100, 'Comparisons'),
];
const merged = mergeRows(splitRow);
const local = (b, x) => spanOffsetAtX(b, x);

assert.equal(merged.length, 1, 'vertically overlapping spans merge into one row');
assert.equal(merged[0].left, 117.6);
assert.equal(merged[0].right, 882.4);
assert.equal(merged[0].text, 'totype.  Comparisons'); // span texts + the justify-gap space
assert.deepEqual(merged[0].parts, splitRow, 'original span entries stay reachable');

test('mergeRows: rows a pitch apart stay separate', () => {
  const rows3 = mergeRows([span(0, 100, 0, 'aaa'), span(0, 100, 30, 'bbb'), span(0, 100, 60, 'ccc')]);
  assert.equal(rows3.length, 3);
  assert.deepEqual(rows3.map((r) => r.text), ['aaa', 'bbb', 'ccc']);
});

test('mergeRows: interleaved table cells reassemble into visual lines', () => {
  // pdf.js emits wrapped table cells column-by-column: DOM order goes
  // line1-col1, line2-col1, line1-col2, line2-col2. Chaining in that order
  // shattered each visual line into per-cell fragments, so double-clicking a
  // word in column 2 resolved to column 1's fragment.
  const spanAt = (l, r, t, text) => ({ ...span(l, r, t, text), left: l, right: r, top: t, bottom: t + 17.8 });
  const ents = [
    spanAt(144, 236.8, 616.5, 'Deployment'),
    spanAt(250.7, 297, 616.5, 'matu-'),
    spanAt(144, 171.6, 638.6, 'rity'),
    spanAt(316.5, 576.5, 616.5, 'DEV runtime and stub adapters;'),
    spanAt(316.5, 468.2, 638.6, 'official run blocked.'),
  ];
  const rows = mergeRows(ents);
  assert.equal(rows.length, 2, 'two visual lines, not four fragments');
  assert.match(rows[0].text, /^Deployment matu- DEV runtime/);
  assert.match(rows[1].text, /^rity official run/);
});

test('rowOffsetAtX: pointer over the SECOND span of a split row moves the trim past it (stuck-trim regression)', () => {
  const r = merged[0];
  // cursor deep inside the 'Comparisons' span: the offset must land in that
  // span's text, not clamp to the first span's end (the old bug froze the
  // highlight at the span boundary)
  const off = rowOffsetAtX(r, 500, local);
  assert.ok(off >= 9, `offset ${off} must be past the first spans' text`);
  assert.ok(off < r.text.length);
});

test('rowOffsetAtX: inter-span gap snaps to the nearer span edge; row edges clamp', () => {
  const r = merged[0];
  assert.equal(rowOffsetAtX(r, 181, local), 8); // in the justify gap, nearer the space span's end
  assert.equal(rowOffsetAtX(r, 100, local), 0); // left margin
  assert.equal(rowOffsetAtX(r, 990, local), r.text.length); // right margin
});

test('rowXOf: merged offset maps back into the correct span; ends clamp to the row box', () => {
  const r = merged[0];
  const x = rowXOf(r, 11, spanXOf); // offset 11 = 2 glyphs into 'Comparisons'
  assert.ok(x > 187.5 && x < 882.4, `x ${x} inside the Comparisons span`);
  assert.equal(rowXOf(r, 0, spanXOf), 117.6);
  assert.equal(rowXOf(r, 999, spanXOf), 882.4);
});

test('spanOffsetAtX/spanXOf: proportional mapping with edge slop', () => {
  const b = { left: 100, right: 200, start: 5, end: 15 };
  assert.equal(spanOffsetAtX(b, 90), 5); // 2px slop pulls margin presses to the start
  assert.equal(spanOffsetAtX(b, 150), 10);
  assert.equal(spanOffsetAtX(b, 210), 15);
  assert.equal(spanXOf(b, 5), 100);
  assert.equal(spanXOf(b, 10), 150);
  assert.equal(spanXOf(b, 15), 200);
});
