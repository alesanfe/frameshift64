#!/usr/bin/env python3
"""check_public_api — enforce the public-header contract.

Reads config/architecture/public-api.yml (the declared surface — see
docs/architecture/public-api.md) and verifies, for every `public:` header:

1. the file exists;
2. its quoted `#include "x.h"` dependencies resolve only to other public
   headers, siblings in the same directory, or names that don't resolve
   under src/ at all (external deps like libultraship — warned, not failed).

A public header including a private module header silently couples external
consumers to internals — that is a semver break waiting to happen.

Exits 0 clean, 1 on violations.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
MANIFEST = ROOT / "config" / "architecture" / "public-api.yml"
SRC = ROOT / "src"

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*"([^"]+)"')


def parse_manifest() -> tuple[set[str], dict[str, str]]:
    """Return (public paths, {retired: note}). Minimal YAML subset parser —
    the manifest is deliberately flat."""
    public, retired = set(), {}
    section = None
    for raw in MANIFEST.read_text(encoding="utf-8").splitlines():
        line = raw.split("#", 1)[0].rstrip()
        if not line.strip():
            continue
        if m := re.match(r"^(\w+):", line):
            section = m.group(1)
            continue
        if m := re.match(r"^\s*-\s*(\S+)(?:\s*#?\s*(.*))?", line):
            if section == "public":
                public.add(m.group(1))
            elif section == "retired":
                retired[m.group(1)] = (m.group(2) or "").strip()
    return public, retired


def src_basename_index() -> dict[str, list[Path]]:
    idx: dict[str, list[Path]] = {}
    for p in SRC.rglob("*.h"):
        idx.setdefault(p.name, []).append(p)
    return idx


def main() -> int:
    public, retired = parse_manifest()
    public_paths = {str(Path(p)) for p in public}
    index = src_basename_index()
    problems: list[str] = []
    warnings: list[str] = []

    for rel in sorted(public):
        path = ROOT / rel
        if not path.is_file():
            problems.append(f"public header missing: {rel}")
            continue
        for no, line in enumerate(path.read_text(
                encoding="utf-8", errors="replace").splitlines(), 1):
            m = INCLUDE_RE.match(line)
            if not m:
                continue
            inc = m.group(1)
            candidates = index.get(Path(inc).name, [])
            resolved = [c for c in candidates
                        if str(c.relative_to(ROOT)) in public_paths]
            siblings = [c for c in candidates if c.parent == path.parent]
            if resolved:
                continue  # public -> public: fine
            if siblings:
                continue  # sibling private header: fine
            if candidates:
                where = ", ".join(str(c.relative_to(ROOT)) for c in candidates)
                problems.append(
                    f"{rel}:{no} includes private header '{inc}' ({where})")
            else:
                warnings.append(
                    f"{rel}:{no} '{inc}' unresolved under src/ "
                    "(external dep? keep an eye on it)")

    for rel in retired:
        if (ROOT / rel).is_file():
            warnings.append(f"retired header still on disk: {rel}")

    for w in warnings:
        print(f"[public-api warn] {w}")
    for p in problems:
        print(f"[public-api] {p}")
    if problems:
        print(f"{len(problems)} violation(s) — see docs/architecture/public-api.md")
        return 1
    print(f"public api clean ({len(public)} headers)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
