import { test } from 'node:test';
import assert from 'node:assert/strict';
import { combineSpans, groupLineSpans, insertPageLines, lineAt, offsetAtX, pointNearRect, bandClipRect, selectionOffsetsForLine, wordModeSpan, xOfOffset } from '../src/selection.js';

const line = (left, top, right, bottom) => ({ left, top, right, bottom, width: right - left, height: bottom - top });

// a visual line pdf.js split into spans, as spanVisibleBox/allLines see it:
// 'deep' + 'indent trial' on one box, each span already trimmed
const SPLIT_SPANS = [
  { node: {}, text: 'deep', start: 0, left: 10, right: 42, top: 10, bottom: 24 },
  { node: {}, text: 'indent trial', start: 0, left: 50, right: 98, top: 10, bottom: 24 },
];
const SPLIT = combineSpans(SPLIT_SPANS);

// a tall inline glyph merged with the adjacent body line, as spanVisibleBox
// sees them: the 13px overlap covers half the 13px short box, so groupLineSpans
// folds them into one logical line. Painting that line's union band would
// cover the other span's glyphs even when only one span is selected, so each
// combined seg keeps its own vertical extent.
const TALL = { node: {}, text: 'Big', start: 0, left: 10, right: 40, top: 10, bottom: 38 };
const SMALL = { node: {}, text: 'body', start: 0, left: 44, right: 72, top: 24, bottom: 37 };
const MERGED = combineSpans([TALL, SMALL]);

// the rect composition buildSegments performs for one logical line: one rect
// per span painted at that span's own vertical extent, plus the covered
// inter-span spaces so a split line paints continuously
const charRects = (vis, offA, offF) => {
  const o = selectionOffsetsForLine(0, 0, offA, 0, offF, vis.start, vis.end);
  const sOff = Math.min(o.start, o.end), eOff = Math.max(o.start, o.end);
  const rects = [];
  const bands = [];
  for (let k = 0; k < vis.segs.length; k++) {
    const seg = vis.segs[k];
    const s = Math.max(sOff, seg.start), e = Math.min(eOff, seg.end);
    if (e > s) {
      const sx = xOfOffset(vis, s), ex = xOfOffset(vis, e);
      if (ex - sx >= 0.5) {
        rects.push([Math.min(sx, ex), Math.max(sx, ex), seg.top, seg.bottom]);
        bands[k] = [seg.top, seg.bottom];
      }
    }
    const next = vis.segs[k + 1];
    if (!next || sOff > seg.end || eOff < next.start) continue;
    const l = xOfOffset(vis, seg.end);
    const r = xOfOffset(vis, next.start);
    if (r - l < 0.5) continue;
    const band = bands[k] || bands[k + 1] || [seg.top, seg.bottom];
    rects.push([l, r, band[0], band[1]]);
    if (!bands[k]) bands[k] = [band[0], band[1]];
  }
  return rects;
};

// what annos.js buildSegments + drawSelection compose for one logical line:
// per-span paint bounds joined into the copied text
const charText = (vis, offA, offF) => {
  const o = selectionOffsetsForLine(0, 0, offA, 0, offF, vis.start, vis.end);
  const sOff = Math.min(o.start, o.end), eOff = Math.max(o.start, o.end);
  const parts = [];
  for (const seg of vis.segs) {
    const s = Math.max(sOff, seg.start), e = Math.min(eOff, seg.end);
    if (e > s) parts.push(seg.text.slice(s - seg.start, e - seg.start));
  }
  return parts.join(' ');
};

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

test('combineSpans: same-visual-line spans share one coordinate space', () => {
  assert.equal(SPLIT.text, 'deep indent trial');
  assert.equal(SPLIT.start, 0);
  assert.equal(SPLIT.end, 17);
  assert.deepEqual(SPLIT.segs.map((s) => [s.start, s.end]), [[0, 4], [5, 17]]);
  assert.equal(SPLIT.left, 10);
  assert.equal(SPLIT.right, 98);
});

test('combineSpans: a single span keeps its own node and exact offsets', () => {
  const single = combineSpans([SPLIT_SPANS[0]]);
  assert.equal(single.node, SPLIT_SPANS[0].node);
  assert.equal(single.text, 'deep');
  assert.deepEqual(single.segs.map((s) => [s.start, s.end]), [[0, 4]]);
});

test('lineAt: press resolves the visual line under the pointer, y first', () => {
  const above = { docTop: 0, docBottom: 14, vis: { left: 10, right: 90 } };
  const below = { docTop: 20, docBottom: 34, vis: { left: 10, right: 90 } };
  assert.equal(lineAt([above, below], 25, 50), below);
  assert.equal(lineAt([above, below], 8, 50), above);
});

test('lineAt: callers without x resolve by vertical position alone', () => {
  const above = { docTop: 0, docBottom: 14, vis: { left: 10, right: 40 } };
  const below = { docTop: 20, docBottom: 34, vis: { left: 10, right: 40 } };
  assert.equal(lineAt([above, below], 25), below);
  assert.equal(lineAt([above, below], 5), above);
});

test('lineAt: an x outside the line resolves the nearest line horizontally', () => {
  const near = { docTop: 0, docBottom: 14, vis: { left: 10, right: 40 } };
  const far = { docTop: 0, docBottom: 14, vis: { left: 200, right: 230 } };
  assert.equal(lineAt([near, far], 7, 60), near);
});

test('character drag: leftward across the split spans paints exactly between both anchors', () => {
  // anchor inside 'trial' (span offset 9 -> combined 5 + 9 = 14), focus inside
  // 'deep' (span offset 2 -> combined 2): pre-fix this read as a cross-line
  // drag and painted the complement ('de' + 'trial')
  assert.equal(charText(SPLIT, 14, 2), 'ep indent tr');
});

test('character drag: rightward across the split spans keeps both anchors', () => {
  assert.equal(charText(SPLIT, 2, 14), 'ep indent tr');
});

test('character drag: both anchors inside one span keep span-local behavior', () => {
  assert.equal(charText(SPLIT, 6, 10), 'nden');
});

test('word mode: drag across the split spans keeps whole words', () => {
  // double-click 'trial' [12, 17), drag left into 'indent' (combined 8)
  const [iS, offS, iE, offE] = wordModeSpan(0, 12, 17, 0, SPLIT.text, SPLIT.start, 8);
  assert.deepEqual([iS, offS, iE, offE], [0, 5, 0, 17]);
  assert.equal(charText(SPLIT, offS, offE), 'indent trial');
});

test('groupLineSpans: spans sharing one vertical line box merge into one group', () => {
  const groups = groupLineSpans(SPLIT_SPANS);
  assert.equal(groups.length, 1);
  assert.equal(combineSpans(groups[0]).text, 'deep indent trial');
});

test('groupLineSpans: mixed-size spans of one line still merge', () => {
  // a smaller inline span riding the same baseline (superscript-style):
  // 8px of the 10px short box overlaps the 14px box -> same line
  const groups = groupLineSpans([
    { top: 10, bottom: 24 },
    { top: 12, bottom: 22 },
  ]);
  assert.equal(groups.length, 1);
});

test('groupLineSpans: adjacent tight lines never merge', () => {
  // leading (13px) smaller than the font size (14px): the boxes overlap by
  // 1px, which the any-overlap predicate read as one line. Pre-fix this
  // merged both lines into one logical line, so a press on the lower line
  // could resolve a span on the upper one.
  const groups = groupLineSpans([
    { top: 10, bottom: 24 },
    { top: 23, bottom: 37 },
  ]);
  assert.equal(groups.length, 2);
});

test('groupLineSpans: a tight line after a merged split line stays separate', () => {
  // the real sequence on a split-line page: two spans of one visual line,
  // then the next (tight) line
  const groups = groupLineSpans([
    ...SPLIT_SPANS,
    { top: 23, bottom: 37 },
  ]);
  assert.equal(groups.length, 2);
  assert.equal(groups[0].length, 2);
  assert.equal(groups[1].length, 1);
});

test('groupLineSpans: a chain of tight lines stays one-per-group', () => {
  // footnotes / dense reference lists: every line overlaps the next by 2px
  const groups = groupLineSpans([
    { top: 0, bottom: 14 },
    { top: 12, bottom: 26 },
    { top: 24, bottom: 38 },
  ]);
  assert.equal(groups.length, 3);
});

test('groupLineSpans: an empty layer yields no lines', () => {
  assert.deepEqual(groupLineSpans([]), []);
});

test('offsetAtX/xOfOffset: a merged line maps through the span under the pointer', () => {
  // inside the first span
  assert.ok(offsetAtX(SPLIT, 30) < 5, 'offset inside the first span');
  // in the trimmed gap: nearest span wins, never a phantom character
  assert.equal(offsetAtX(SPLIT, 46), 4);
  // inside the second span
  assert.ok(offsetAtX(SPLIT, 70) >= 5 && offsetAtX(SPLIT, 70) < 17);
  // offsets round-trip through pointer x within one span
  const off = 5 + 3;
  assert.ok(Math.abs(xOfOffset(SPLIT, off) - xOfOffset(SPLIT, off)) < 0.01);
  assert.ok(xOfOffset(SPLIT, 0) <= xOfOffset(SPLIT, 4));
  assert.ok(xOfOffset(SPLIT, 4) < xOfOffset(SPLIT, 5));
  assert.ok(xOfOffset(SPLIT, 17) > xOfOffset(SPLIT, 16));
});

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

test('groupLineSpans: a tall inline glyph merges with the adjacent line', () => {
  // the reachable precondition of the union-band defect: the 28px glyph
  // swallows the 13px line below it into one logical line
  assert.equal(groupLineSpans([TALL, SMALL]).length, 1);
});

test('combineSpans: a merged line keeps each span its own vertical extent', () => {
  assert.deepEqual(
    MERGED.segs.map((s) => [s.top, s.bottom]),
    [[10, 38], [24, 37]],
  );
});

test('paint: one span of a merged line paints its own box, not the line union', () => {
  // selection confined to the small span (combined offsets [4, 8)): pre-fix
  // its rect was painted at the merged line's union [10, 38], covering the
  // tall glyph's text, which the copied string ('body') does not include
  assert.deepEqual(charRects(MERGED, 4, 8), [[44, 72, 24, 37]]);
});

test('paint: the tall span paints its own box, and both spans keep their extents', () => {
  assert.deepEqual(charRects(MERGED, 0, 3), [[10, 40, 10, 38]]);
  // a selection covering both spans also paints the space between them
  assert.deepEqual(charRects(MERGED, 0, 8), [
    [10, 40, 10, 38], [40, 44, 10, 38], [44, 72, 24, 37],
  ]);
});

test('paint: a covered inter-span space paints between the two spans', () => {
  // char drag 'ep...tr' across the SPLIT line covers the trimmed space at
  // combined offset 4, so the band is continuous: [26..42] + [42..50] + [50..86]
  assert.deepEqual(charRects(SPLIT, 14, 2), [
    [26, 42, 10, 24], [42, 50, 10, 24], [50, 86, 10, 24],
  ]);
});

test('paint: a selection ending exactly on a space leaves it unpainted', () => {
  assert.deepEqual(charRects(SPLIT, 2, 4), [[26, 42, 10, 24]]);
  assert.equal(charText(SPLIT, 2, 4), 'ep');
});

test('paint: a selection that is only the space paints exactly the space', () => {
  assert.deepEqual(charRects(SPLIT, 4, 5), [[42, 50, 10, 24]]);
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

test('insertPageLines: a page rendered mid-drag merges after the cached pages in order', () => {
  const a = { pageIdx: 0, docTop: 0 };
  const b = { pageIdx: 0, docTop: 20 };
  const fresh = [{ pageIdx: 2, docTop: 100 }, { pageIdx: 2, docTop: 120 }];
  const merged = insertPageLines([a, b], 2, fresh);
  assert.deepEqual(
    merged.map((l) => [l.pageIdx, l.docTop]),
    [[0, 0], [0, 20], [2, 100], [2, 120]],
  );
  // cached entries keep identity, so an anchor already resolved stays valid
  assert.equal(merged[0], a);
  assert.equal(merged[1], b);
});

test('insertPageLines: a page above the cache lands before it, reading order kept', () => {
  const fresh = [{ pageIdx: 1, docTop: 10 }, { pageIdx: 1, docTop: 30 }];
  const merged = insertPageLines([{ pageIdx: 3, docTop: 50 }], 1, fresh);
  assert.deepEqual(
    merged.map((l) => [l.pageIdx, l.docTop]),
    [[1, 10], [1, 30], [3, 50]],
  );
});

test('insertPageLines: a page between cached pages inserts at its place', () => {
  const lo = { pageIdx: 0 };
  const hi = { pageIdx: 4 };
  assert.deepEqual(insertPageLines([lo, hi], 2, [{ pageIdx: 2 }]), [lo, { pageIdx: 2 }, hi]);
});

test('insertPageLines: a page yielding no lines leaves the cache untouched', () => {
  const cached = [{ pageIdx: 0 }];
  assert.deepEqual(insertPageLines(cached, 1, []), cached);
});
