import { test } from 'node:test';
import assert from 'node:assert/strict';
import { pointNearRect, bandClipRect, selectionOffsetsForLine, wordModeSpan } from '../src/selection.js';

const line = (left, top, right, bottom) => ({ left, top, right, bottom, width: right - left, height: bottom - top });

// Word-mode regression fixtures mirroring the reported ARC-AGI-3 drag: the
// anchor line reads 'To incentivize this' with the double-clicked word
// 'incentivize' at offsets [3, 14); a line above/below reads
// 'red-team ARC-AGI-3.'.
const ANCHOR_LINE = 'To incentivize this';
const A_START = 0, A_END = 19;
const WORD_START = 3, WORD_END = 14;
const FOCUS_LINE = 'red-team ARC-AGI-3.';
const F_START = 0, F_END = 19;
const A_SPAN = { text: ANCHOR_LINE, start: A_START };
const R_SPAN = { text: FOCUS_LINE, start: F_START };

// the exact composition applySelectionAt/buildSegments/drawSelection perform:
// the word-mode span feeds per-line paint bounds and the line slices join
// into the copied text
const wordText = (iA, aStart, aEnd, iF, lines, off) => {
  const [iS, offS, iE, offE] = wordModeSpan(iA, aStart, aEnd, iF, lines[iF].text, lines[iF].start, off);
  const parts = [];
  for (let i = Math.min(iS, iE); i <= Math.max(iS, iE); i++) {
    const l = lines[i];
    const o = selectionOffsetsForLine(i, iS, offS, iE, offE, l.start, l.start + l.text.length);
    parts.push(l.text.slice(o.start - l.start, o.end - l.start));
  }
  return { span: [iS, offS, iE, offE], text: parts.join(' ').replace(/\s+/g, ' ').trim() };
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

test('wordModeSpan: same-line leftward drag onto an earlier word selects through to the anchor word end', () => {
  // pointer released inside 'To' (offset 1): pre-fix this painted only the
  // inter-word gap [2, 3)
  const { span, text } = wordText(0, WORD_START, WORD_END, 0, [A_SPAN], 1);
  assert.deepEqual(span, [0, 0, 0, 14]);
  assert.equal(text, 'To incentivize');
});

test('wordModeSpan: same-line leftward drag onto the inter-word gap keeps the anchor word', () => {
  // pointer released on the space between 'To' and 'incentivize' (offset 2):
  // must not collapse to whitespace-only or empty
  const { text } = wordText(0, WORD_START, WORD_END, 0, [A_SPAN], 2);
  assert.equal(text, 'incentivize');
});

test('wordModeSpan: same-line rightward drag grows through the trailing gap then onto the next word', () => {
  // released on the space after 'incentivize' (offset 14): gap kept as the
  // open end so the selection grows smoothly
  assert.equal(wordText(0, WORD_START, WORD_END, 0, [A_SPAN], 14).text, 'incentivize');
  // released inside 'this': word end boundary
  const { text } = wordText(0, WORD_START, WORD_END, 0, [A_SPAN], 16);
  assert.equal(text, 'incentivize this');
});

test('wordModeSpan: focus inside the anchor word keeps the whole word', () => {
  assert.equal(wordText(0, WORD_START, WORD_END, 0, [A_SPAN], 7).text, 'incentivize');
  assert.equal(wordText(0, WORD_START, WORD_END, 0, [A_SPAN], 13).text, 'incentivize');
});

test('wordModeSpan: trimmed span with leading whitespace keeps word-exact boundaries', () => {
  // models the span text '  To incentivize this': vis.text is the trimmed
  // slice at vis.start = 2, the anchor word lives at full offsets [5, 16),
  // and offsets arrive in full-span coordinates (the annos.js call shape)
  const TRIMMED = { text: 'To incentivize this', start: 2 };
  // drag right while still over the anchor word (full offset 10)
  const { span, text } = wordText(0, 5, 16, 0, [TRIMMED], 10);
  assert.deepEqual(span, [0, 5, 0, 16]);
  assert.equal(text, 'incentivize');
  // leftward onto the gap before the word (full offset 4)
  assert.equal(wordText(0, 5, 16, 0, [TRIMMED], 4).text, 'incentivize');
  // leftward onto 'To' (full offset 3)
  assert.equal(wordText(0, 5, 16, 0, [TRIMMED], 3).text, 'To incentivize');
  // rightward onto 'this' (full offset 18)
  assert.equal(wordText(0, 5, 16, 0, [TRIMMED], 18).text, 'incentivize this');
});

test('wordModeSpan: drag to a line below snaps the focus to its word end, anchor keeps its word', () => {
  // released inside 'ARC' on the lower line: the whitespace-delimited token
  // is 'ARC-AGI-3.', so its end boundary is the line end
  const { span, text } = wordText(0, WORD_START, WORD_END, 1, [A_SPAN, R_SPAN], 10);
  assert.deepEqual(span, [0, 3, 1, 19]);
  // focus line is the last of the range
  assert.deepEqual(selectionOffsetsForLine(1, 0, 3, 1, 19, F_START, F_END), { start: 0, end: 19 });
  assert.equal(FOCUS_LINE.slice(0, 19), 'red-team ARC-AGI-3.');
  // anchor line is the first of the range
  assert.deepEqual(selectionOffsetsForLine(0, 0, 3, 1, 19, A_START, A_END), { start: 3, end: 19 });
  assert.equal(ANCHOR_LINE.slice(3, 19), 'incentivize this');
  assert.equal(text, 'incentivize this red-team ARC-AGI-3.');
});

test('wordModeSpan: drag to a line above starts at the focus word and ends at the anchor word', () => {
  // released inside 'ARC' on the upper line: the painted block runs from the
  // focus word start through the focus line end, then from the anchor line
  // start through the anchor word end
  const { span, text } = wordText(1, WORD_START, WORD_END, 0, [R_SPAN, A_SPAN], 10);
  assert.deepEqual(span, [0, 9, 1, 14]);
  // focus line is the first of the range: focus word start through line end
  assert.deepEqual(selectionOffsetsForLine(0, 0, 9, 1, 14, F_START, F_END), { start: 9, end: 19 });
  assert.equal(FOCUS_LINE.slice(9, 19), 'ARC-AGI-3.');
  // anchor line is the last of the range: line start through anchor word end
  assert.deepEqual(selectionOffsetsForLine(1, 0, 9, 1, 14, A_START, A_END), { start: 0, end: 14 });
  assert.equal(ANCHOR_LINE.slice(0, 14), 'To incentivize');
  assert.equal(text, 'ARC-AGI-3. To incentivize');
});

test('wordModeSpan: upward multi-line drag paints every line between in full', () => {
  const MID = { text: 'models must be', start: 0 };
  const { span, text } = wordText(2, WORD_START, WORD_END, 0, [R_SPAN, MID, A_SPAN], 10);
  assert.deepEqual(span, [0, 9, 2, 14]);
  // middle line is painted in full
  assert.deepEqual(selectionOffsetsForLine(1, 0, 9, 2, 14, 0, 14), { start: 0, end: 14 });
  assert.equal(text, 'ARC-AGI-3. models must be To incentivize');
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
