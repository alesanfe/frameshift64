#!/usr/bin/env python3
"""check_release_binary — shipped binaries must not contain gated test hooks.

Reads the "Test hooks (gated" section of docs/development/env_vars.md,
expands brace groups, and fails if any of those `SSB64_*` names appears in
the given binary (or any file under a given directory). Packaging scripts
call this after building with -DSSB64_TEST_HOOKS=OFF.

Usage: check_release_binary.py <file-or-dir> [more paths...]
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_env_vars import DOC, DOC_VAR, brace_expand


def gated_names() -> set[str]:
    text = DOC.read_text(encoding='utf-8')
    marker = 'Test hooks (gated'
    idx = text.find(marker)
    if idx < 0:
        raise SystemExit("check_release_binary: gated section not found in env_vars.md")
    tail = text[idx:]
    end = tail.find('\n## ')
    if end >= 0:
        tail = tail[:end]
    names: set[str] = set()
    for tok in DOC_VAR.findall(tail):
        names |= brace_expand(tok)
    return names


def iter_files(paths):
    for p in paths:
        p = Path(p)
        if p.is_dir():
            yield from (f for f in p.rglob('*') if f.is_file())
        elif p.is_file():
            yield p
        else:
            print(f"check_release_binary: not found: {p}")


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: check_release_binary.py <file-or-dir> [...]")
        return 2
    names = gated_names()
    if not names:
        print("check_release_binary: empty denylist")
        return 1
    needles = [n.encode() for n in sorted(names)]
    hits = []
    for f in iter_files(sys.argv[1:]):
        try:
            data = f.read_bytes()
        except OSError:
            continue
        for n in needles:
            if n in data:
                hits.append((f, n.decode()))
    if hits:
        print("check_release_binary: gated test-hook strings found:")
        for f, n in hits:
            print(f"  {n}  in {f}")
        return 1
    print(f"check_release_binary: clean ({len(names)} gated names absent)")
    return 0


if __name__ == '__main__':
    sys.exit(main())
