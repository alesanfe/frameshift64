#!/usr/bin/env python3
"""
png_to_ia8_inc_c.py — convert RGBA PNGs to N64 IA8 raw bytes as a .tex.inc.c file.

IA8 is 8-bit-per-pixel: 4-bit intensity + 4-bit alpha. Suited for monochrome
sprites with anti-aliased edges (text, glyph atlases, B&W silhouettes).

Output format matches png_to_rgba16_inc_c.py's convention:
    0xHH, 0xHH, ...    (16 hex bytes per line)

Per-pixel encoding:
    pixel = (intensity_4bit << 4) | alpha_4bit
where intensity = (R + G + B) / 3 quantized to 4 bits, and alpha is the
alpha channel quantized to 4 bits.

Usage:
    png_to_ia8_inc_c.py <input.png> <output.tex.inc.c>
    png_to_ia8_inc_c.py --batch <input_dir> <output_dir>
"""

from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image
from png_tex_inc_c import run_cli


def png_to_ia8_bytes(png_path: Path) -> tuple[bytearray, int, int]:
    """Load an RGBA PNG and return (raw_ia8_bytes, width, height)."""
    img = Image.open(png_path).convert("RGBA")
    w, h = img.size
    px = img.load()
    raw = bytearray(w * h)

    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            i = (r + g + b) // 3
            i4 = (i >> 4) & 0xF
            a4 = (a >> 4) & 0xF
            raw[y * w + x] = (i4 << 4) | a4

    return raw, w, h


def main(argv: list[str]) -> int:
    return run_cli(argv, png_to_ia8_bytes, "png_to_ia8_inc_c.py", "IA8", "IA8", __doc__)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
