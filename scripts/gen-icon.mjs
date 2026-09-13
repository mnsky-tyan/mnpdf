// Generates dev-assets/icon-src.png (512x512) without any image dependency:
// dark rounded square, grey "text lines", one translucent yellow highlight bar.
import { deflateSync } from 'node:zlib';
import { writeFileSync, mkdirSync } from 'node:fs';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const W = 512, H = 512;
const px = new Uint8Array(W * H * 4);

function blend(x0, y0, x1, y1, [r, g, b], a = 255) {
  for (let y = Math.max(0, y0); y < Math.min(H, y1); y++) {
    for (let x = Math.max(0, x0); x < Math.min(W, x1); x++) {
      const i = (y * W + x) * 4;
      const na = a / 255;
      px[i] = Math.round(px[i] * (1 - na) + r * na);
      px[i + 1] = Math.round(px[i + 1] * (1 - na) + g * na);
      px[i + 2] = Math.round(px[i + 2] * (1 - na) + b * na);
      px[i + 3] = 255;
    }
  }
}

// background
blend(0, 0, W, H, [26, 27, 32]);

// text lines
const grey = [174, 180, 191];
blend(118, 132, 394, 162, grey);
blend(118, 196, 360, 226, grey);
blend(118, 262, 394, 292, grey);
blend(118, 328, 330, 358, grey);
// highlighter stroke across the middle lines
blend(96, 186, 382, 236, [255, 212, 63], 200);
blend(96, 252, 382, 302, [255, 212, 63], 200);

// ---- PNG encode ----
const CRC_TABLE = (() => {
  const t = new Int32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c;
  }
  return t;
})();
function crc32(buf) {
  let c = -1;
  for (let i = 0; i < buf.length; i++) c = CRC_TABLE[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
  return (c ^ -1) >>> 0;
}
function chunk(type, data) {
  const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
  const td = Buffer.concat([Buffer.from(type, 'ascii'), data]);
  const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(td));
  return Buffer.concat([len, td, crc]);
}
const ihdr = Buffer.alloc(13);
ihdr.writeUInt32BE(W, 0); ihdr.writeUInt32BE(H, 4);
ihdr[8] = 8; ihdr[9] = 6; // 8-bit RGBA
const raw = Buffer.alloc(H * (W * 4 + 1));
for (let y = 0; y < H; y++) {
  raw[y * (W * 4 + 1)] = 0;
  Buffer.from(px.buffer, px.byteOffset + y * W * 4, W * 4).copy(raw, y * (W * 4 + 1) + 1);
}
const png = Buffer.concat([
  Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
  chunk('IHDR', ihdr),
  chunk('IDAT', deflateSync(raw, { level: 9 })),
  chunk('IEND', Buffer.alloc(0)),
]);
const outDir = resolve(dirname(fileURLToPath(import.meta.url)), '..', 'dev-assets');
mkdirSync(outDir, { recursive: true });
writeFileSync(resolve(outDir, 'icon-src.png'), png);
console.log('wrote dev-assets/icon-src.png');
