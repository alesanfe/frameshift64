#!/usr/bin/env python3
"""generate_checksums — SHA-256 manifest over a directory of artifacts.

Writes a classic SHA256SUMS file (relpath-sorted, `hash  path` lines) so
`sha256sum -c` verifies it out of the box on Linux/macOS, and a
PowerShell Get-FileHash comparison covers Windows.

Usage: generate_checksums.py --input <dir> --output <file>
"""
import argparse
import hashlib
import sys
from pathlib import Path

CHUNK = 1 << 20


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        while b := f.read(CHUNK):
            h.update(b)
    return h.hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--input", required=True)
    ap.add_argument("--output", required=True)
    args = ap.parse_args()

    root = Path(args.input)
    if not root.is_dir():
        print(f"error: not a directory: {root}", file=sys.stderr)
        return 2

    files = sorted(p for p in root.rglob("*") if p.is_file()
                   and p.name != Path(args.output).name)
    if not files:
        print(f"error: no files under {root}", file=sys.stderr)
        return 2

    lines = [f"{sha256(p)}  {p.relative_to(root).as_posix()}"
             for p in files]
    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"wrote {out} ({len(lines)} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
