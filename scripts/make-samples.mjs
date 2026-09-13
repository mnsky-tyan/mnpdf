// Generates test PDFs into public/ (served by vite dev, fetchable via ?file=).
import { PDFDocument, StandardFonts, rgb } from 'pdf-lib';
import { writeFileSync, mkdirSync } from 'node:fs';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const pub = resolve(root, 'public');
mkdirSync(pub, { recursive: true });

const PARA = `Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore et dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea commodo consequat. Duis aute irure dolor in reprehenderit in voluptate velit esse cillum dolore eu fugiat nulla pariatur. Excepteur sint occaecat cupidatat non proident, sunt in culpa qui officia deserunt mollit anim id est laborum. Curabitur pretium tincidunt lacus, nulla gravida orci a odio, aliquet tempor quam ac purus interdum.`;

function wrap(text, font, size, maxW) {
  const words = text.split(/\s+/);
  const lines = [];
  let line = '';
  for (const w of words) {
    const t = line ? line + ' ' + w : w;
    if (font.widthOfTextAtSize(t, size) > maxW && line) { lines.push(line); line = w; }
    else line = t;
  }
  if (line) lines.push(line);
  return lines;
}

async function make(name, pageCount, paraRepeat) {
  const doc = await PDFDocument.create();
  const font = await doc.embedFont(StandardFonts.Helvetica);
  const bold = await doc.embedFont(StandardFonts.HelveticaBold);
  for (let p = 0; p < pageCount; p++) {
    const page = doc.addPage([612, 792]);
    const { width } = page.getSize();
    page.drawText('mnpdf sample document', { x: 56, y: 736, size: 10, font, color: rgb(0.55, 0.57, 0.6) });
    page.drawText(`Chapter ${p + 1} — The quick brown fox`, { x: 56, y: 700, size: 18, font: bold });
    let y = 664;
    for (let r = 0; r < paraRepeat; r++) {
      for (const line of wrap(PARA + (r % 2 ? ' Segitiga empat lima enam tujuh.' : ''), font, 12, width - 112)) {
        page.drawText(line, { x: 56, y, size: 12, font, color: rgb(0.13, 0.13, 0.15) });
        y -= 18;
      }
      y -= 14;
    }
    page.drawText(`${p + 1}`, { x: width / 2 - 4, y: 32, size: 10, font, color: rgb(0.5, 0.5, 0.55) });
  }
  const bytes = await doc.save();
  writeFileSync(resolve(pub, name), bytes);
  console.log(`wrote public/${name} (${bytes.length} bytes, ${pageCount} pages)`);
}

await make('sample.pdf', 5, 5);
await make('big.pdf', 60, 6);
