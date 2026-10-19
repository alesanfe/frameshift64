#!/usr/bin/env python3
"""
png_to_rgba16_inc_c.py — convert RGBA PNGs to N64 RGBA5551 raw bytes as a .tex.inc.c file.

For full-color sprites where IA8 (intensity + alpha) loses too much, RGBA16
is the right format: 16 bits per pixel — 5 bits R, 5 bits G, 5 bits B, 1 bit A.
Alpha is binary (>=128 -> 1, else 0). Each 32x32 sprite produces 2048 bytes;
each 32x48 produces 3072 bytes.

Output format matches upstream extract_inc_c.py's .tex.inc.c convention:
    0xHH, 0xHH, ...    (16 hex bytes per line, comma-separated, trailing comma OK)

Bytes are emitted in big-endian order — pixel = (R<<11) | (G<<6) | (B<<1) | A,
high byte first, matching N64 GBI's expectation when loaded with
G_IM_FMT_RGBA / G_IM_SIZ_16b.

Usage:
    png_to_rgba16_inc_c.py <input.png> <output.tex.inc.c>
    png_to_rgba16_inc_c.py --batch <input_dir> <output_dir>
"""

from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image
from png_tex_inc_c import run_cli


def png_to_rgba16_bytes(png_path: Path) -> tuple[bytearray, int, int]:
    """Load an RGBA PNG and return (raw_rgba16_bytes_be, width, height)."""
    img = Image.open(png_path).convert("RGBA")
    w, h = img.size
    px = img.load()
    raw = bytearray(w * h * 2)

    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            r5 = r >> 3
            g5 = g >> 3
            b5 = b >> 3
            a1 = 1 if a >= 128 else 0
            pixel = (r5 << 11) | (g5 << 6) | (b5 << 1) | a1
            off = (y * w + x) * 2
            raw[off]     = (pixel >> 8) & 0xFF
            raw[off + 1] = pixel & 0xFF

    return raw, w, h


def main(argv: list[str]) -> int:
    return run_cli(argv, png_to_rgba16_bytes, "png_to_rgba16_inc_c.py",
                   "RGBA5551", "RGBA16", __doc__)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
