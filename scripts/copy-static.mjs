// Copies pdf.js runtime assets (worker + cmaps) into public/ so they are
// served in dev and bundled into dist/ for the desktop build.
import { cpSync, copyFileSync, existsSync, mkdirSync } from 'node:fs';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const src = resolve(root, 'node_modules/pdfjs-dist');
if (!existsSync(src)) {
  console.log('pdfjs-dist not installed yet; skipping static copy');
  process.exit(0);
}
mkdirSync(resolve(root, 'public'), { recursive: true });
cpSync(resolve(src, 'cmaps'), resolve(root, 'public/cmaps'), { recursive: true });
copyFileSync(
  resolve(src, 'build/pdf.worker.min.mjs'),
  resolve(root, 'public/pdf.worker.min.mjs'),
);
console.log('copied pdf.js worker + cmaps to public/');
