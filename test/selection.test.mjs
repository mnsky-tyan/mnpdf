import { test } from 'node:test';
import assert from 'node:assert/strict';
import { pointNearRect, bandClipRect, selectionOffsetsForLine, wordModeOffsets } from '../src/selection.js';

const line = (left, top, right, bottom) => ({ left, top, right, bottom, width: right - left, height: bottom - top });

// Word-mode regression fixtures mirroring the reported ARC-AGI-3 drag: the
// anchor line reads 'To incentivize this' with the double-clicked word
// 'incentivize' at offsets [3, 14); a second line reads 'red-team ARC-AGI-3.'.
const ANCHOR_LINE = 'To incentivize this';
const A_START = 0, A_END = 19;
const WORD_START = 3, WORD_END = 14;
const FOCUS_LINE = 'red-team ARC-AGI-3.';
const F_START = 0, F_END = 19;

// the exact composition applySelectionAt/buildSegments perform: the word-mode
// offset pair feeds the per-line paint bounds
const wordPaint = (rel, off, focus = { text: ANCHOR_LINE, start: A_START, end: A_END }) => {
  const [offA, offF] = wordModeOffsets(rel, WORD_START, WORD_END, focus.text, focus.start, focus.end, off);
  return { offA, offF, painted: selectionOffsetsForLine(0, 0, offA, 0, offF, focus.start, focus.end) };
};

test('selectionOffsetsForLine: same-line left-to-right drag keeps both anchors', () => {
  assert.deepEqual(selectionOffsetsForLine(2, 2, 4, 2, 11, 0, 20), { start: 4, end: 11 });
});

test('selectionOffsetsForLine: same-line right-to-left drag keeps both anchors', () => {
  assert.deepEqual(selectionOffsetsForLine(2, 2, 11, 2, 4, 0, 20), { start: 4, end: 11 });
});

test('selectionOffsetsForLine: cross-line trims follow drag direction', () => {
  assert.deepEqual(selectionOffsetsForLine(1, 1, 4, 3, 11, 0, 20), { start: 4, end: 20 });
  assert.deepEqual(selectionOffsetsForLine(3, 1, 4, 3, 11, 0, 20), { start: 0, end: 11 });
  assert.deepEqual(selectionOffsetsForLine(1, 3, 11, 1, 4, 0, 20), { start: 0, end: 4 });
  assert.deepEqual(selectionOffsetsForLine(3, 3, 11, 1, 4, 0, 20), { start: 11, end: 20 });
});

test('wordModeOffsets: same-line leftward drag onto an earlier word selects through to the anchor word end', () => {
  // pointer released inside 'To' (offset 1): pre-fix this painted only the
  // inter-word gap [2, 3)
  const { painted } = wordPaint(0, 1);
  assert.deepEqual(painted, { start: 0, end: 14 });
  assert.equal(ANCHOR_LINE.slice(painted.start, painted.end), 'To incentivize');
});

test('wordModeOffsets: same-line leftward drag onto the inter-word gap keeps the anchor word', () => {
  // pointer released on the space between 'To' and 'incentivize' (offset 2):
  // must not collapse to whitespace-only or empty
  const { painted } = wordPaint(0, 2);
  assert.deepEqual(painted, { start: 3, end: 14 });
  assert.equal(ANCHOR_LINE.slice(painted.start, painted.end), 'incentivize');
});

test('wordModeOffsets: same-line rightward drag grows through the trailing gap then onto the next word', () => {
  // released on the space after 'incentivize' (offset 14): gap kept as the
  // open end so the selection grows smoothly
  assert.deepEqual(wordPaint(0, 14).painted, { start: 3, end: 14 });
  // released inside 'this': word end boundary
  const { painted } = wordPaint(0, 16);
  assert.deepEqual(painted, { start: 3, end: 19 });
  assert.equal(ANCHOR_LINE.slice(painted.start, painted.end), 'incentivize this');
});

test('wordModeOffsets: focus inside the anchor word keeps the whole word', () => {
  assert.deepEqual(wordPaint(0, 7).painted, { start: 3, end: 14 });
  assert.deepEqual(wordPaint(0, 13).painted, { start: 3, end: 14 });
});

test('wordModeOffsets: drag to a line below snaps the focus to its word end, anchor keeps its word', () => {
  // released inside 'ARC' on the lower line: the whitespace-delimited token is
  // 'ARC-AGI-3.', so its end boundary is the line end
  const [offA, offF] = wordModeOffsets(1, WORD_START, WORD_END, FOCUS_LINE, F_START, F_END, 10);
  assert.equal(offA, 3);
  assert.equal(offF, 19);
  // focus line is the last of the range
  assert.deepEqual(selectionOffsetsForLine(1, 0, offA, 1, offF, F_START, F_END), { start: 0, end: 19 });
  assert.equal(FOCUS_LINE.slice(0, 19), 'red-team ARC-AGI-3.');
  // anchor line is the first of the range
  assert.deepEqual(selectionOffsetsForLine(0, 0, offA, 1, offF, A_START, A_END), { start: 3, end: 19 });
});

test('wordModeOffsets: drag to a line above snaps the focus to its word start, anchor keeps its word', () => {
  // released inside 'ARC' on the upper line
  const [offA, offF] = wordModeOffsets(-1, WORD_START, WORD_END, FOCUS_LINE, F_START, F_END, 10);
  assert.equal(offA, 3);
  assert.equal(offF, 9);
  // focus line is the first of the range
  assert.deepEqual(selectionOffsetsForLine(0, 1, offA, 0, offF, F_START, F_END), { start: 0, end: 9 });
  assert.equal(FOCUS_LINE.slice(0, 9), 'red-team ');
  // anchor line is the last of the range
  assert.deepEqual(selectionOffsetsForLine(1, 1, offA, 0, offF, A_START, A_END), { start: 3, end: 19 });
});

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
