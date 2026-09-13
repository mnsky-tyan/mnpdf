// Automated GUI test of the mnpdf dev server (vite) via system Edge headless.
// Run: node scripts/guitest.mjs [filter]
// Screenshots -> gui-test-screenshots/*.png
import { chromium } from 'playwright-core';
import { mkdirSync, writeFileSync } from 'node:fs';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const shots = resolve(root, 'gui-test-screenshots');
mkdirSync(shots, { recursive: true });

const results = [];
const filter = process.argv[2] || '';
let browser, page, ctx;

function shot(name) {
  const p = resolve(shots, name + '.png');
  return page.screenshot({ path: p }).then(() => p);
}
function log(name, ok, note = '') {
  results.push({ name, ok, note });
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${note ? '  — ' + note : ''}`);
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function openApp(url = 'http://localhost:5173/?file=sample.pdf') {
  page = await ctx.newPage();
  page.on('console', (m) => {
    if (m.type() === 'error' && !m.text().includes('404')) console.log('  [console.error]', m.text().slice(0, 300));
  });
  page.on('pageerror', (e) => console.log('  [pageerror]', String(e).slice(0, 300)));
  await page.goto('http://localhost:5173/');
  await page.evaluate(() => localStorage.clear()); // isolate per-doc sidecars between tests
  await page.goto(url);
  await page.waitForLoadState('domcontentloaded');
  await page.waitForTimeout(2200);
  return page;
}

// point safely inside the viewport at a fraction of page 1's box
function inView(geo, fx, fy) {
  return {
    x: Math.round(geo.left + geo.w * fx),
    y: Math.round(Math.min(geo.top + geo.h * fy, 700)),
  };
}

// real drag-select over the text layer
async function dragSelect(x1, y1, x2, y2) {
  await page.mouse.move(x1, y1);
  await page.mouse.down();
  await page.mouse.move((x1 + x2) / 2, (y1 + y2) / 2, { steps: 8 });
  await page.mouse.move(x2, y2, { steps: 8 });
  await page.mouse.up();
}

// raw click (locator.click auto-retries when the swatch bar detaches mid-click,
// which would double-fire pointerdown handlers)
async function clickSwatch(n) {
  const c = await page.evaluate((n) => {
    const btn = document.querySelectorAll('.selbar-sw')[n];
    if (!btn) return null;
    const r = btn.getBoundingClientRect();
    return { x: r.x + r.width / 2, y: r.y + r.height / 2 };
  }, n);
  if (!c) throw new Error('selbar-sw[' + n + '] not found');
  await page.mouse.click(c.x, c.y);
}

async function pixelStats(sel) {
  return page.evaluate((sel) => {
    const c = document.querySelector(sel);
    if (!c || !c.width) return null;
    const t = document.createElement('canvas');
    const k = 80 / Math.max(c.width, 1);
    t.width = 80; t.height = Math.max(1, Math.round(c.height * k));
    const g = t.getContext('2d');
    g.fillStyle = '#fff'; g.fillRect(0, 0, t.width, t.height);
    g.drawImage(c, 0, 0, t.width, t.height);
    const d = g.getImageData(0, 0, t.width, t.height).data;
    let dark = 0, yellowish = 0, n = t.width * t.height;
    for (let i = 0; i < d.length; i += 4) {
      if (d[i] < 120 && d[i + 1] < 120 && d[i + 2] < 120) dark++;
      if (d[i] > 180 && d[i + 1] > 150 && d[i + 2] < 140) yellowish++;
    }
    return { darkFrac: +(dark / n).toFixed(4), yellowFrac: +(yellowish / n).toFixed(4) };
  }, sel);
}

async function main() {
  browser = await chromium.launch({
    executablePath: 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',
    headless: true,
  });
  ctx = await browser.newContext({ viewport: { width: 1000, height: 800 } });

  // T1 render + pill
  if (!filter || filter === 't1') {
    await openApp();
    await shot('t1_open');
    const pill = await page.textContent('#pill').catch(() => '');
    log('t1 render page 1 + pill', pill.includes('1 / 5'), `pill="${pill.trim()}"`);
    const sel = await page.evaluate(() => {
      const c = document.querySelector('.pagewrap[data-i="0"] canvas');
      return { w: c?.width, h: c?.height, wraps: document.querySelectorAll('.pagewrap').length,
               tl: document.querySelectorAll('.textLayer span').length };
    });
    const px = await pixelStats('.pagewrap[data-i="0"] canvas');
    log('t1 canvas + textlayer', sel.wraps === 5 && sel.tl >= 25 && sel.w > 400 && px.darkFrac > 0.005,
        JSON.stringify(sel) + ' pixels=' + JSON.stringify(px));
  }

  // T2 select text -> swatch bar -> highlight
  if (!filter || filter === 't2') {
    const p = await openApp();
    await sleep(300);
    // select second paragraph line region on page 1 (page is centered)
    const geo = await page.evaluate(() => {
      const w = document.querySelector('.pagewrap[data-i="0"]');
      const r = w.getBoundingClientRect();
      return { left: r.left, top: r.top, w: r.width, h: r.height };
    });
    await dragSelect(
      geo.left + geo.w * 0.15, geo.top + geo.h * 0.34,
      geo.left + geo.w * 0.85, geo.top + geo.h * 0.40,
    );
    await sleep(400);
    const hasBar = await page.locator('.selbar').count();
    await shot('t2_selection');
    log('t2 selection swatch bar', hasBar === 1, `selbar=${hasBar}`);
    // click yellow swatch
    await clickSwatch(0);
    await sleep(300);
    const hl = await page.evaluate(() => ({
      anns: window.mnpdf.S.anns.length,
      rects: document.querySelectorAll('.hl').length,
      sel: String(window.getSelection()),
    }));
    await shot('t2_highlight');
    log('t2 highlight applied', hl.anns === 1 && hl.rects > 0 && hl.sel === '',
        JSON.stringify(hl.anns) + ' ann, ' + hl.rects + ' rects, selected text len ok');
  }

  // T3 undo/redo
  if (!filter || filter === 't3') {
    await openApp();
    await sleep(300);
    const geo = await page.evaluate(() => {
      const w = document.querySelector('.pagewrap[data-i="0"]');
      const r = w.getBoundingClientRect();
      return { left: r.left, top: r.top, w: r.width, h: r.height };
    });
    await dragSelect(geo.left + geo.w * 0.2, geo.top + geo.h * 0.34,
                     geo.left + geo.w * 0.7, geo.top + geo.h * 0.38);
    await sleep(350);
    await clickSwatch(2);
    await sleep(250);
    const afterAdd = await page.evaluate(() => window.mnpdf.S.anns.length);
    await page.keyboard.press('Control+z');
    await sleep(250);
    const afterUndo = await page.evaluate(() => window.mnpdf.S.anns.length);
    await page.keyboard.press('Control+y');
    await sleep(250);
    const afterRedo = await page.evaluate(() => window.mnpdf.S.anns.length);
    await shot('t3_undo_redo');
    log('t3 undo/redo', afterAdd === 1 && afterUndo === 0 && afterRedo === 1,
        `add=${afterAdd} undo=${afterUndo} redo=${afterRedo}`);
  }

  // T4 right-click context menu + add note
  if (!filter || filter === 't4') {
    await openApp();
    await sleep(300);
    const geo = await page.evaluate(() => {
      const w = document.querySelector('.pagewrap[data-i="0"]');
      const r = w.getBoundingClientRect();
      return { left: r.left, top: r.top, w: r.width, h: r.height };
    });
    const pt = inView(geo, 0.5, 0.75);
    await page.mouse.click(pt.x, pt.y, { button: 'right' });
    await sleep(350);
    const menuItems = await page.locator('.menu-item').allTextContents();
    await shot('t4_menu');
    log('t4 page context menu', menuItems.some((t) => t.includes('Add text here')), menuItems.join(' | ').slice(0, 120));
    await page.locator('.menu-item', { hasText: 'Add text here' }).click();
    await sleep(300);
    const ta = page.locator('.note-ta');
    const taCount = await ta.count();
    if (taCount) {
      await ta.type('hello mnpdf note');
      await ta.press('Control+Enter');
      await sleep(300);
    }
    const note = await page.evaluate(() => ({
      anns: window.mnpdf.S.anns.filter((a) => a.type === 'note').length,
      dom: document.querySelectorAll('.note').length,
      text: document.querySelector('.note')?.textContent,
    }));
    await shot('t4_note');
    log('t4 add note', note.anns === 1 && note.dom === 1 && note.text === 'hello mnpdf note',
        JSON.stringify(note));
  }

  // T5 search
  if (!filter || filter === 't5') {
    await openApp();
    await sleep(300);
    await page.keyboard.press('Control+f');
    await sleep(250);
    const boxVisible = await page.locator('#search').isVisible();
    await page.fill('#search-input', 'laboris');
    await sleep(900);
    const count1 = await page.textContent('#search-count');
    await shot('t5_search');
    const rects = await page.evaluate(() => document.querySelectorAll('.srch').length);
    await page.keyboard.press('F3');
    await sleep(400);
    const count2 = await page.textContent('#search-count');
    await page.keyboard.press('Escape');
    await sleep(250);
    const closed = !(await page.locator('#search').isVisible());
    log('t5 search', boxVisible && rects > 0 && count1 !== count2 && closed,
        `first="${count1.trim()}" afterF3="${count2.trim()}" rects=${rects}`);
  }

  // T6 goto page + zoom
  if (!filter || filter === 't6') {
    await openApp();
    await sleep(300);
    await page.keyboard.press('Control+g');
    await sleep(250);
    await page.locator('#pill input').fill('3');
    await page.keyboard.press('Enter');
    await sleep(700);
    const pill3 = await page.textContent('#pill');
    await shot('t6_goto3');
    await page.keyboard.press('Control+0');
    await sleep(500);
    await page.keyboard.press('+');
    await sleep(400);
    const pillZoom = await page.textContent('#pill');
    log('t6 goto+zoom', pill3.includes('3 / 5') && pillZoom.includes('1'),
        `goto="${pill3.trim()}" zoom="${pillZoom.trim()}"`);
  }

  // T7 thumbnails + rotate
  if (!filter || filter === 't7') {
    await openApp();
    await sleep(400);
    await page.keyboard.press('F9');
    await sleep(700);
    const thumbs = await page.evaluate(() => document.querySelectorAll('.thumb').length);
    await shot('t7_thumbs');
    // rotate page 1 via context menu on page
    const geo = await page.evaluate(() => {
      const w = document.querySelector('.pagewrap[data-i="0"]');
      const r = w.getBoundingClientRect();
      return { left: r.left, top: r.top, w: r.width, h: r.height };
    });
    const pt7 = inView(geo, 0.5, 0.3);
    await page.mouse.click(pt7.x, pt7.y, { button: 'right' });
    await sleep(350);
    await page.locator('.menu-item', { hasText: 'Rotate clockwise' }).click();
    await sleep(800);
    const rot = await page.evaluate(() => window.mnpdf.S.pageList[0].rot);
    await shot('t7_rotated');
    log('t7 thumbnails + rotate', thumbs === 5 && rot === 90, `thumbs=${thumbs} rot=${rot}`);
  }

  // T8 bake + reload saved bytes (highlight + note persisted into PDF)
  if (!filter || filter === 't8') {
    await openApp();
    await sleep(300);
    const geo = await page.evaluate(() => {
      const w = document.querySelector('.pagewrap[data-i="0"]');
      const r = w.getBoundingClientRect();
      return { left: r.left, top: r.top, w: r.width, h: r.height };
    });
    await dragSelect(geo.left + geo.w * 0.15, geo.top + geo.h * 0.30,
                     geo.left + geo.w * 0.85, geo.top + geo.h * 0.36);
    await sleep(350);
    await clickSwatch(0);
    await sleep(250);
    const info = await page.evaluate(async () => {
      const bytes = await window.mnpdf.bake();
      const blob = new Blob([bytes], { type: 'application/pdf' });
      const url = URL.createObjectURL(blob);
      await window.mnpdf.cmd.openPath(url);
      return { len: bytes.length };
    });
    await sleep(2000);
    const after = await page.evaluate(() => ({
      anns: window.mnpdf.S.anns.length,
      dirty: window.mnpdf.S.dirty,
      name: window.mnpdf.S.name,
    }));
    await shot('t8_baked');
    log('t8 bake+reload', info.len > 5000 && after.anns === 0 && after.dirty === false,
        JSON.stringify({ ...info, ...after }));
  }

  // T9 sidecar persistence: zoom+page remembered after reload
  if (!filter || filter === 't9') {
    const p = await openApp();
    await sleep(300);
    await page.keyboard.press('Control+g');
    await sleep(200);
    await page.locator('#pill input').fill('2');
    await page.keyboard.press('Enter');
    await sleep(600);
    await page.keyboard.press('+');
    await sleep(600);
    const before = await page.evaluate(() => window.mnpdf.S.zoom);
    await sleep(1000);
    await page.reload();
    await page.waitForTimeout(2200);
    const after = await page.evaluate(() => ({ zoom: window.mnpdf.S.zoom, top: window.mnpdf.S }));
    const pill = await page.textContent('#pill');
    await shot('t9_restored');
    log('t9 reopen restores zoom', Math.abs(after.zoom - before) < 0.001 && pill.includes('2 / 5'),
        `zoom ${before.toFixed(3)} -> ${after.zoom.toFixed(3)} pill="${pill.trim()}"`);
  }

  // T10 dark mode visual
  if (!filter || filter === 't10') {
    ctx = await browser.newContext({
      viewport: { width: 1000, height: 800 },
      colorScheme: 'dark',
    });
    await openApp();
    await shot('t10_dark');
    const bg = await page.evaluate(() => getComputedStyle(document.body).backgroundColor);
    log('t10 dark follows OS', /rgb\((2[0-9]|1[0-9]),/.test(bg), 'body bg=' + bg);
  }

  await browser.close();
  const failed = results.filter((r) => !r.ok);
  console.log(`\n${results.length - failed.length}/${results.length} passed`);
  process.exit(failed.length ? 1 : 0);
}

main().catch((e) => {
  console.error('GUI test crashed:', e);
  process.exit(2);
});
