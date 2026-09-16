import { test } from 'node:test';
import assert from 'node:assert/strict';
import { pointNearRect, bandClipRect, selectionOffsetsForLine } from '../src/selection.js';

const line = (left, top, right, bottom) => ({ left, top, right, bottom, width: right - left, height: bottom - top });

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
