#!/usr/bin/env python3
"""Regenerate mnpdf's icon pixels from the mn-brand 2048x2048 render.

    python3 scripts/gen-icon.py [source-2048.png]

Defaults to the mn-brand checkout's mnpdf render. Writes, in the repo root:

  - mnpdf.ico  - the Windows icon, 6 entries: 16, 24, 32, 48, 64, 256 px
  - mnpdf.png  - the Linux window/launcher artwork, 512 px

Both come from the same pipeline, so every .ico entry and the shipped PNG are
the same pixels by construction (each size is rendered independently from the
2048 crop, never resized from another icon size). Needs Pillow; the outputs
are committed, nothing is generated at build time.

What this does to the artwork (and what it deliberately does NOT do):

- Crops to the brand card's outer edge (its 1.5-unit white stroke included).
  The mnpdf card is <rect x="12" y="12" width="278" height="270" rx="43"/>
  in the 300-unit mn master viewBox, so the stroke-outer box is 279.5x271.5
  units, which at the render's 2048/300 scale measures (77, 77, 1985, 1930)
  - measured on the render itself (the first/last near-white #fffefa stroke
  pixels), not read off the SVG alone. That is CARD_BOX below.
- Fits the card into a square canvas with transparent top/bottom padding
  (~1.5% of the edge each) instead of cropping into the stroke.
- Clips to the brand corner radius, r = 43/277 of the icon edge (~15.5%),
  with a 4x4 supersampled alpha mask. Outside the card only alpha changes;
  NO pixel inside the card is altered. This is a presentation clip done at
  swap time, not a mn-brand edit: the artwork itself still changes only in
  the mn-brand pipeline. The radius constant and the method come from
  mnbrand docs/icon-corner-radius.md - keep it identical across apps - and
  the reference implementation is mnPaper's linux/gen_icon_tables.py.

Honesty: the mn-brand icons are source-derived candidates; their RGB pixels
are NOT identical to the originally supplied artwork. This mask changes
nothing about that. Never derive these pixels from the 300x300 control copy
in mnbrand/reference/originals - that is an audit control, not a source.
"""

import argparse
import os
import sys

from PIL import Image

SIZES_ICO = [16, 24, 32, 48, 64, 256]   # what mnpdf.rc embeds via mnpdf.ico
PNG_SIZE = 512                            # the shipped mnpdf.png

DEFAULT_SOURCE = os.path.expanduser(
    "~/mnsky/mnbrand/icons/mnpdf.png")

# Stroke-outer card box in the 2048 render (see module docstring).
CARD_BOX = (77, 77, 1985, 1930)           # PIL crop box: x0, y0, x1, y1
RX, CARD_W = 43, 277                      # master-units radius / width


def rounded_alpha(w, h, x0, y0, cw, ch, radius):
    """4x4-supersampled rounded-rect alpha for a (cw,ch) rect at (x0,y0)."""
    a = bytearray(w * h)
    for y in range(y0, y0 + ch):
        for x in range(x0, x0 + cw):
            hit = 0
            for sy in range(4):
                py = y + (sy + 0.5) / 4.0
                dy = max(abs(py - (y0 + ch / 2.0)) - (ch / 2.0 - radius), 0.0)
                for sx in range(4):
                    px = x + (sx + 0.5) / 4.0
                    dx = max(abs(px - (x0 + cw / 2.0)) - (cw / 2.0 - radius), 0.0)
                    if dx * dx + dy * dy <= radius * radius:
                        hit += 1
            a[y * w + x] = round(255 * hit / 16)
    return a


def render(src, size):
    card = src.crop(CARD_BOX)
    k = size / card.width
    h2 = round(card.height * k)
    rgb = card.resize((size, h2), Image.Resampling.LANCZOS).convert("RGBA")
    canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    canvas.paste(rgb, (0, (size - h2) // 2))
    r = size * RX / CARD_W                  # keep fractional; the mask is AA
    alpha = rounded_alpha(size, size, 0, (size - h2) // 2, size, h2, r)
    canvas.putalpha(Image.frombytes("L", (size, size), bytes(alpha)))
    return canvas


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source", nargs="?", default=DEFAULT_SOURCE)
    ap.add_argument("--out-dir", default=".")
    args = ap.parse_args()

    src = Image.open(args.source).convert("RGB")
    ico_all = [render(src, s) for s in SIZES_ICO]
    big = ico_all[-1]
    others = ico_all[:-1]
    ico_path = os.path.join(args.out_dir, "mnpdf.ico")
    big.save(ico_path, format="ICO", append_images=others,
             sizes=[(s, s) for s in SIZES_ICO])

    png_path = os.path.join(args.out_dir, "mnpdf.png")
    render(src, PNG_SIZE).save(png_path)

    print("wrote %s and %s" % (ico_path, png_path), file=sys.stderr)


if __name__ == "__main__":
    main()
