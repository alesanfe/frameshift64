#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# convert_gliden64_pack.py — re-key a third-party N64 hires texture pack
# (GLideN64 / Rice-PNG filename grammar) into the decoded-RGBA8 CRC32 keys
# this port's HiResPack looks up, so the pack loads by dropping it into
# mods/.
#
# Adapted from GhostlyDark's battleship.py (MIT,
# https://github.com/GhostlyDark/SSB-Reloaded). Like the original this is
# a pure rename pass — no image decoding, no GPL CRC code; hashes come
# from the vendored dictionary tools/modding/vendor/battleship.json.
#
# Source name grammar (GLideN64 dumps and most Rice-PNG packs):
#     "<prefix>#<crc8>#<fmt>#<siz>[#<palCRC8>]_<fmtTag>.png"
# Output name grammar (HiResPack::ParseFilename):
#     "#<ourHash8>#<fmt>#<siz>.png"
#
# Folder layout below each matched PNG is preserved under --out — the
# port scans mods/ recursively and keys purely off the filename, so the
# nesting is cosmetic (kept only so the output stays browsable).
#
# Usage:
#   python3 tools/modding/convert_gliden64_pack.py \
#       --pack ~/Downloads/SSB-Reloaded-png \
#       --out  "mods/My Pack"

import argparse
import json
import re
import shutil
import sys
import zipfile
from pathlib import Path

PACK_NAME_RE = re.compile(
    r'^.*#([0-9A-Fa-f]{8})#([0-9])#([0-9])(?:#([0-9A-Fa-f]{8}))?_(\w+)\.png$',
    re.IGNORECASE,
)

DEFAULT_MAPPING = Path(__file__).resolve().parent / 'vendor' / 'battleship.json'


def out_filename(our_hash: str, fmt: int, siz: int) -> str:
    """Filename layout the port's runtime parser accepts
    (src/resources/textures/hires/HiResPack.cpp)."""
    return f'#{our_hash.upper()}#{fmt:X}#{siz:X}.png'


def map_key(m: re.Match):
    crc = int(m.group(1), 16)
    fmt = int(m.group(2))
    siz = int(m.group(3))
    pal = int(m.group(4), 16) if m.group(4) else None
    if pal is not None:
        key = f'{crc:08X}#{fmt:X}#{siz:X}#{pal:08X}'
    else:
        key = f'{crc:08X}#{fmt:X}#{siz:X}'
    return key, fmt, siz


def write_converted(mapping, out_dir, rel_dir, src_name, write_bytes):
    m = PACK_NAME_RE.match(src_name)
    if not m:
        return 'bad'
    key, fmt, siz = map_key(m)
    our_hash = mapping.get(key)
    if our_hash is None:
        return 'unmatched'
    dst_dir = out_dir / rel_dir
    dst_dir.mkdir(parents=True, exist_ok=True)
    dst = dst_dir / out_filename(our_hash, fmt, siz)
    write_bytes(dst)
    return 'matched'


def convert_dir(mapping, pack: Path, out_dir: Path):
    matched = unmatched = bad = 0
    for src in sorted(pack.rglob('*')):
        if not src.is_file() or src.suffix.lower() != '.png':
            continue
        rel_dir = src.parent.relative_to(pack)
        result = write_converted(mapping, out_dir, rel_dir, src.name,
                                 lambda dst, s=src: shutil.copyfile(s, dst))
        if result == 'matched':
            matched += 1
        elif result == 'unmatched':
            unmatched += 1
        else:
            bad += 1
    return matched, unmatched, bad


def convert_zip(mapping, pack: Path, out_dir: Path):
    matched = unmatched = bad = 0
    with zipfile.ZipFile(pack) as zf:
        for info in zf.infolist():
            name = info.filename
            if name.endswith('/') or not name.lower().endswith('.png'):
                continue
            base = name.rsplit('/', 1)[-1]
            rel_dir = Path(name).parent
            rel_dir = Path('.') if str(rel_dir) == '.' else rel_dir

            def writer(dst, i=info):
                with zf.open(i) as fin, open(dst, 'wb') as fout:
                    shutil.copyfileobj(fin, fout)

            result = write_converted(mapping, out_dir, rel_dir, base, writer)
            if result == 'matched':
                matched += 1
            elif result == 'unmatched':
                unmatched += 1
            else:
                bad += 1
    return matched, unmatched, bad


def main():
    ap = argparse.ArgumentParser(
        description='Re-key a third-party N64 hires pack for BattleShip.')
    ap.add_argument('--mapping', type=Path, default=DEFAULT_MAPPING,
                    help='Hash dictionary (default: vendored battleship.json)')
    ap.add_argument('--pack', required=True, type=Path,
                    help='Source pack: a directory of PNGs or a .zip')
    ap.add_argument('--out', required=True, type=Path,
                    help='Destination pack folder (point inside mods/)')
    args = ap.parse_args()

    if not args.mapping.is_file():
        sys.exit(f'--mapping not found: {args.mapping}')
    mapping = json.loads(args.mapping.read_text())
    print(f'Loaded {len(mapping)} mapping entries from {args.mapping}')

    args.out.mkdir(parents=True, exist_ok=True)

    if args.pack.is_dir():
        counts = convert_dir(mapping, args.pack, args.out)
    elif args.pack.is_file() and args.pack.suffix.lower() == '.zip':
        counts = convert_zip(mapping, args.pack, args.out)
    else:
        sys.exit(f'--pack must be a directory or .zip: {args.pack}')

    matched, unmatched, bad = counts
    print(f'Done. {matched} matched, {unmatched} no-mapping, {bad} unparseable. '
          f'Total PNGs scanned: {matched + unmatched + bad}.')
    print(f'Output: {args.out}')
    if matched == 0:
        sys.exit('error: nothing converted — is this a GLideN64/Rice-PNG pack?')


if __name__ == '__main__':
    main()
