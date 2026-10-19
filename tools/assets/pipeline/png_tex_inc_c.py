#!/usr/bin/env python3
"""Shared PNG → .tex.inc.c driver for the png_to_*_inc_c.py converters.

Each converter supplies an `encode(png_path) -> (raw_bytes, w, h)` function
implementing its N64 pixel format; this module owns the hex-dump emission,
batch mode, and CLI plumbing so the pair stays in lockstep.
"""

from __future__ import annotations

import argparse
import sys
from collections.abc import Callable
from pathlib import Path

Encoder = Callable[[Path], "tuple[bytearray, int, int]"]


def write_inc_c(raw: bytes, output_path: Path, width: int, height: int,
                src_name: str, fmt_name: str) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with open(output_path, "w", encoding="utf-8") as f:
        f.write(f"/* Auto-generated from PNG via {src_name} — do not edit. */\n")
        f.write(f"/* Format: N64 {fmt_name} ({width}x{height}, {len(raw)} bytes raw) */\n")
        for i in range(0, len(raw), 16):
            chunk = raw[i:i + 16]
            f.write("    " + ", ".join(f"0x{b:02X}" for b in chunk) + ",\n")


def convert_one(encode: Encoder, png_path: Path, out_path: Path,
                src_name: str, fmt_name: str, fmt_label: str) -> None:
    raw, w, h = encode(png_path)
    write_inc_c(raw, out_path, w, h, src_name, fmt_name)
    print(f"  {png_path.name}  ({w}x{h}) -> {out_path.name}  [{len(raw)} bytes {fmt_label}]")


def run_cli(argv: list[str], encode: Encoder, src_name: str, fmt_name: str,
            fmt_label: str, description: str) -> int:
    p = argparse.ArgumentParser(description=description,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--batch", action="store_true", help="treat inputs as input_dir + output_dir")
    p.add_argument("inputs", nargs=2, metavar=("INPUT", "OUTPUT"))
    args = p.parse_args(argv)

    if args.batch:
        in_dir = Path(args.inputs[0])
        out_dir = Path(args.inputs[1])
        if not in_dir.is_dir():
            print(f"error: {in_dir} is not a directory", file=sys.stderr)
            return 1
        pngs = sorted(in_dir.glob("*.png"))
        if not pngs:
            print(f"error: no PNG files in {in_dir}", file=sys.stderr)
            return 1
        print(f"Converting {len(pngs)} PNGs from {in_dir} -> {out_dir}:")
        for png in pngs:
            out = out_dir / f"{png.stem}.tex.inc.c"
            convert_one(encode, png, out, src_name, fmt_name, fmt_label)
        return 0
    png = Path(args.inputs[0])
    out = Path(args.inputs[1])
    if not png.is_file():
        print(f"error: {png} is not a file", file=sys.stderr)
        return 1
    convert_one(encode, png, out, src_name, fmt_name, fmt_label)
    return 0
