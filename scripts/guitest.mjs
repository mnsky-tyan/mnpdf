// Automated GUI test of the mnpdf dev server (vite) via system Edge headless.
// Run: node scripts/guitest.mjs [filter]
// Screenshots -> gui-test-screenshots/*.png
import { chromium } from 'playwright-core';
import { mkdirSync } from 'node:fs';
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

async function openApp(url = 'http://localhost:5173/?file=sample.pdf', opts = {}) {
  if (page) { try { await page.close(); } catch {} } // no cross-page sidecar flushes
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

// run a flaky-prone test body up to n attempts
async function withRetry(n, body) {
  for (let i = 1; i <= n; i++) {
    try {
      await body();
      return;
    } catch (e) {
      if (i === n) throw e;
      console.log(`  retry ${i}/${n - 1}: ${String(e).slice(0, 120)}`);
    }
  }
}

// real drag-select over the text layer
async function dragSelect(x1, y1, x2, y2) {
  await page.mouse.move(x1, y1);
  await page.mouse.down();
  await page.mouse.move((x1 + x2) / 2, (y1 + y2) / 2, { steps: 8 });
  await page.mouse.move(x2, y2, { steps: 8 });
  await page.mouse.up();
}

// point safely inside the viewport at a fraction of page 1's box
function inView(geo, fx, fy) {
  return {
    x: Math.round(geo.left + geo.w * fx),
    y: Math.round(Math.min(geo.top + geo.h * fy, 700)),
  };
}

async function page1Geo() {
  return page.evaluate(() => {
    const r = document.querySelector('.pagewrap[data-i="0"]').getBoundingClientRect();
    return { left: r.left, top: r.top, w: r.width, h: r.height };
  });
}

// raw click at an element's center (locator auto-retry can double-fire UI that
// removes itself on click)
async function clickAt(selector, nth = 0) {
  const c = await page.evaluate(([sel, n]) => {
    const e = document.querySelectorAll(sel)[n];
    if (!e) return null;
    const r = e.getBoundingClientRect();
    return { x: r.x + r.width / 2, y: r.y + r.height / 2 };
  }, [selector, nth]);
  if (!c) throw new Error(`clickAt: ${selector}[${nth}] not found`);
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

  // T2 selection alone shows nothing; right-click -> Highlight swatches
  if (!filter || filter === 't2') {
    await openApp();
    await sleep(300);
    const geo = await page1Geo();
    await dragSelect(geo.left + geo.w * 0.15, geo.top + geo.h * 0.34,
                     geo.left + geo.w * 0.85, geo.top + geo.h * 0.40);
    await sleep(400);
    const floating = await page.evaluate(() => ({
      bars: document.querySelectorAll('.selbar').length,
      pops: document.querySelectorAll('#pinpop:not(.hidden)').length,
    }));
    log('t2 selection alone shows no UI', floating.bars === 0 && floating.pops === 0,
        JSON.stringify(floating));
    await shot('t2_selection_quiet');
    // right-click inside the selection
    const pt = inView(geo, 0.5, 0.37);
    await page.mouse.click(pt.x, pt.y, { button: 'right' });
    await sleep(350);
    const swatches = await page.evaluate(() => document.querySelectorAll('.menu-swatches .sw').length);
    await shot('t2_menu');
    log('t2 right-click offers highlight colors', swatches === 5, `swatches=${swatches}`);
    await clickAt('.menu-swatches .sw', 0);
    await sleep(300);
    const hl = await page.evaluate(() => ({
      anns: window.mnpdf.S.anns.length,
      rects: document.querySelectorAll('.hl').length,
      sel: String(window.getSelection()),
    }));
    await shot('t2_highlight');
    log('t2 highlight applied', hl.anns === 1 && hl.rects > 0 && hl.sel === '',
        JSON.stringify(hl.anns) + ' ann, ' + hl.rects + ' rects');
  }

  // T3 undo/redo (keyboard + menu item)
  if (!filter || filter === 't3') {
    await withRetry(2, async () => {
      await openApp();
      await sleep(300);
      const geo = await page1Geo();
      await dragSelect(geo.left + geo.w * 0.2, geo.top + geo.h * 0.34,
                       geo.left + geo.w * 0.7, geo.top + geo.h * 0.38);
      await sleep(350);
      await page.mouse.click(inView(geo, 0.4, 0.36).x, inView(geo, 0.4, 0.36).y, { button: 'right' });
      await sleep(300);
      await clickAt('.menu-swatches .sw', 2);
      await sleep(250);
      const afterAdd = await page.evaluate(() => window.mnpdf.S.anns.length);
      await page.keyboard.press('Control+z');
      await sleep(250);
      const afterUndo = await page.evaluate(() => window.mnpdf.S.anns.length);
      // redo via page context menu (Undo/Redo items live there)
      await page.mouse.click(inView(geo, 0.5, 0.5).x, inView(geo, 0.5, 0.5).y, { button: 'right' });
      await sleep(300);
      await page.locator('.menu-item', { hasText: 'Redo' }).first().click();
      await sleep(250);
      const afterRedo = await page.evaluate(() => window.mnpdf.S.anns.length);
      await shot('t3_undo_redo');
      log('t3 undo/redo (keyboard + menu)', afterAdd === 1 && afterUndo === 0 && afterRedo === 1,
          `add=${afterAdd} undo=${afterUndo} redo=${afterRedo}`);
    });
  }

  // T4 pins: right-click -> Add pin here -> type -> hover shows -> persists
  if (!filter || filter === 't4') {
    await openApp();
    await sleep(300);
    const geo = await page1Geo();
    const pt = inView(geo, 0.5, 0.75);
    await page.mouse.click(pt.x, pt.y, { button: 'right' });
    await sleep(350);
    const menuItems = await page.locator('.menu-item').allTextContents();
    await shot('t4_menu');
    log('t4 page context menu (pin + window actions)',
        menuItems.some((t) => t.includes('Add pin here')) && menuItems.some((t) => t.includes('Undo')),
        menuItems.join(' | ').slice(0, 130));
    await page.locator('.menu-item', { hasText: 'Add pin here' }).click();
    await sleep(300);
    const ta = page.locator('.pin-ta');
    if ((await ta.count()) !== 1) { log('t4 pin editor opens', false, 'no .pin-ta'); }
    else {
      // the dot must be visible NOW (before typing) + typed text must be readable
      const live = await page.evaluate(() => {
        const taEl = document.querySelector('.pin-ta');
        const dot = document.querySelector('.pin.preview');
        const dotR = dot?.getBoundingClientRect();
        return {
          previewDot: !!dot,
          dotPos: dotR ? { x: Math.round(dotR.x), y: Math.round(dotR.y) } : null,
          textColor: taEl ? getComputedStyle(taEl).color : '',
        };
      });
      const dotVisibleNow = live.previewDot && live.textColor.startsWith('rgb(2');
      await shot('t4_pin_editor_live');
      await ta.type('pin text abc');
      await ta.press('Control+Enter');
      await sleep(300);
      const pin = await page.evaluate(() => ({
        pins: window.mnpdf.S.anns.filter((a) => a.type === 'pin').length,
        dom: document.querySelectorAll('.pin').length,
        text: window.mnpdf.S.anns.find((a) => a.type === 'pin')?.text,
      }));
      // hover the pin -> popup shows the text
      await page.hover('.pin');
      await sleep(300);
      const pop = await page.evaluate(() => {
        const p = document.getElementById('pinpop');
        return { visible: !p.classList.contains('hidden'), text: p.textContent };
      });
      await shot('t4_pin_hover');
      log('t4 pin placed + hover popup', pin.pins === 1 && pin.dom === 1 &&
          pin.text === 'pin text abc' && pop.visible && pop.text.includes('pin text abc'),
          JSON.stringify({ pin, pop }));
      // reload: pin must persist via sidecar even without save
      await page.reload();
      await page.waitForTimeout(2200);
      const after = await page.evaluate(() => ({
        pins: window.mnpdf.S.anns.filter((a) => a.type === 'pin').length,
        text: window.mnpdf.S.anns.find((a) => a.type === 'pin')?.text,
      }));
      await page.hover('.pin').catch(() => {});
      await sleep(250);
      const pop2 = await page.evaluate(() => !document.getElementById('pinpop').classList.contains('hidden'));
      await shot('t4_pin_reload');
      log('t4 pin persists after reopen (no save)', after.pins === 1 && after.text === 'pin text abc' && pop2,
          JSON.stringify({ after, popVisible: pop2 }));
    }
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

  // T6 goto page + zoom keys
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
    const geo = await page1Geo();
    const pt7 = inView(geo, 0.5, 0.3);
    await page.mouse.click(pt7.x, pt7.y, { button: 'right' });
    await sleep(350);
    await page.locator('.menu-item', { hasText: 'Rotate clockwise' }).click();
    await sleep(800);
    const rot = await page.evaluate(() => window.mnpdf.S.pageList[0].rot);
    await shot('t7_rotated');
    log('t7 thumbnails + rotate', thumbs === 5 && rot === 90, `thumbs=${thumbs} rot=${rot}`);
  }

  // T8 bake: highlights go into the file, pins do not
  if (!filter || filter === 't8') {
    await openApp();
    await sleep(300);
    const geo = await page1Geo();
    await dragSelect(geo.left + geo.w * 0.15, geo.top + geo.h * 0.30,
                     geo.left + geo.w * 0.85, geo.top + geo.h * 0.36);
    await sleep(350);
    await page.mouse.click(inView(geo, 0.5, 0.33).x, inView(geo, 0.5, 0.33).y, { button: 'right' });
    await sleep(350);
    await clickAt('.menu-swatches .sw', 0);
    await sleep(250);
    // drop one pin so we can verify it survives save via sidecar, not the PDF
    await page.mouse.click(inView(geo, 0.3, 0.8).x, inView(geo, 0.3, 0.8).y, { button: 'right' });
    await sleep(350);
    await page.locator('.menu-item', { hasText: 'Add pin here' }).click();
    await sleep(250);
    await page.locator('.pin-ta').type('survives');
    await page.locator('.pin-ta').press('Control+Enter');
    await sleep(300);
    const info = await page.evaluate(async () => {
      const bytes = await window.mnpdf.bake();
      const blob = new Blob([bytes], { type: 'application/pdf' });
      const url = URL.createObjectURL(blob);
      // replicate doSave's reopen: pins-only sidecar under the new path
      localStorage.setItem('k:doc:' + url, JSON.stringify({
        zoom: window.mnpdf.S.zoom,
        top: 1,
        hl: '#ffd400',
        anns: window.mnpdf.S.anns.filter((a) => a.type === 'pin'),
      }));
      await window.mnpdf.cmd.openPath(url);
      return { len: bytes.length };
    });
    await sleep(2200);
    const after = await page.evaluate(async () => {
      const page1 = await window.mnpdf.S.pdf.getPage(1);
      const tc = await page1.getTextContent();
      const text = tc.items.map((i) => i.str).join(' ');
      return {
        hl: window.mnpdf.S.anns.filter((a) => a.type === 'hl').length,
        pins: window.mnpdf.S.anns.filter((a) => a.type === 'pin').length,
        pinText: window.mnpdf.S.anns.find((a) => a.type === 'pin')?.text,
        textHasPin: text.includes('survives'),
        dirty: window.mnpdf.S.dirty,
      };
    });
    const px = await pixelStats('.pagewrap[data-i="0"] canvas');
    await shot('t8_baked');
    log('t8 bake: hl in file, pin in sidecar',
        info.len > 5000 && after.hl === 0 && after.pins === 1 && after.pinText === 'survives' &&
        !after.textHasPin && !after.dirty && px.yellowFrac > 0.01,
        JSON.stringify({ ...info, ...after, px }));
  }

  // T9 sidecar persistence: zoom+page remembered after reload
  if (!filter || filter === 't9') {
    await openApp();
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
    const after = await page.evaluate(() => window.mnpdf.S.zoom);
    const pill = await page.textContent('#pill');
    await shot('t9_restored');
    log('t9 reopen restores zoom', Math.abs(after - before) < 0.001 && pill.includes('2 / 5'),
        `zoom ${before.toFixed(3)} -> ${after.toFixed(3)} pill="${pill.trim()}"`);
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
    ctx = await browser.newContext({ viewport: { width: 1000, height: 800 } });
  }

  // T11 precise zoom: menu entry (no scroll), pill entry, editor survives scroll
  if (!filter || filter === 't11') {
    await openApp();
    await sleep(300);
    // A: right-click -> "Zoom to…" — no scroll/time pressure needed
    await page.mouse.click(500, 400, { button: 'right' });
    await sleep(350);
    await page.locator('.menu-item', { hasText: 'Zoom to…' }).first().click();
    await sleep(300);
    let okA = await page.evaluate(() => {
      const inp = document.querySelector('#pill input');
      return !!inp && +inp.value >= 25 && +inp.value <= 600;
    });
    if (okA) {
      // scroll while editing: the editor must survive
      await page.mouse.move(500, 300);
      await page.mouse.wheel(0, 80);
      await sleep(400);
      okA = await page.evaluate(() => !!document.querySelector('#pill input'));
      await page.keyboard.press('Escape');
    }
    await sleep(300);
    // B: hover-keep + pill right-half click, scroll mid-edit, then apply 200
    await page.mouse.move(500, 400);
    await page.mouse.wheel(0, 60); // reveal pill
    await sleep(150);
    const r = await page.evaluate(() => {
      const p = document.getElementById('pill').getBoundingClientRect();
      return { x: p.left + p.width * 0.8, y: p.top + p.height / 2 };
    });
    await page.mouse.click(r.x, r.y);
    await sleep(200);
    let inputStill = await page.evaluate(() => !!document.querySelector('#pill input'));
    // wait past the old 1.3s auto-hide window — input must still be alive
    await sleep(1600);
    inputStill = inputStill && (await page.evaluate(() => !!document.querySelector('#pill input')));
    if (inputStill) {
      await page.locator('#pill input').fill('200');
      await page.keyboard.press('Enter');
      await sleep(500);
    }
    const zoom = await page.evaluate(() => window.mnpdf.S.zoom);
    const pill = await page.textContent('#pill');
    await shot('t11_zoom200');
    log('t11 precise zoom entry (stable editor)', okA && inputStill && Math.abs(zoom - 2) < 0.001 && pill.includes('200%'),
        `menuEntry=${okA} inputSurvived=${inputStill} zoom=${zoom} pill="${pill.trim()}"`);
  }

  // T12 touchscreen pinch-zoom (synthetic two-touch gesture)
  if (!filter || filter === 't12') {
    await openApp();
    await sleep(300);
    const z0 = await page.evaluate(() => window.mnpdf.S.zoom);
    await page.evaluate(() => {
      const sc = document.getElementById('scroller');
      const T = (x, y, id) => new Touch({ identifier: id, target: sc, clientX: x, clientY: y });
      const fire = (type, touches) =>
        sc.dispatchEvent(new TouchEvent(type, { touches, bubbles: true, cancelable: true }));
      fire('touchstart', [T(400, 300, 1), T(480, 300, 2)]);
      for (const d of [40, 80, 120, 160]) fire('touchmove', [T(400 - d / 2, 300, 1), T(480 + d / 2, 300, 2)]);
      fire('touchend', []);
    });
    await sleep(500);
    const z1 = await page.evaluate(() => window.mnpdf.S.zoom);
    await shot('t12_pinch');
    log('t12 touch pinch-zoom', z1 > z0 * 1.8, `zoom ${z0.toFixed(2)} -> ${z1.toFixed(2)}`);
  }

  // T14 default open: paper touches window borders; plain launch reopens last file
  if (!filter || filter === 't14') {
    await openApp();
    await sleep(300);
    const m = await page.evaluate(() => {
      const w = document.querySelector('.pagewrap[data-i="0"]').getBoundingClientRect();
      const s = document.getElementById('scroller');
      return { pageW: Math.round(w.width), clientW: s.clientWidth };
    });
    const touching = Math.abs(m.pageW - m.clientW) <= 1;
    // plain launch (no ?file=) must reopen the last document
    await page.goto('http://localhost:5173/');
    await page.waitForTimeout(2500);
    const reopened = await page.evaluate(() => ({ name: window.mnpdf.S.name, n: window.mnpdf.S.nPages }));
    await shot('t14_fit_touch');
    log('t14 fit-width touching + reopen last file',
        touching && reopened.name === 'sample.pdf' && reopened.n === 5,
        JSON.stringify({ ...m, touching, reopened }));
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
