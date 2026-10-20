#!/usr/bin/env python3
"""check_make_help — every public Make target must carry a `##` doc comment.

Public = a `name:` rule that is not:
  * a pattern/variable rule (`print-%`, `%.o:`, `.PHONY`, assignments `x :=`)
  * an internal helper, marked by either a `guard-*`/`_*` name prefix or a
    trailing `# internal` comment on the rule line.

Usage: check_make_help.py Makefile make/*.mk

Exit 1 listing undocumented public targets; 0 clean.
"""
import re
import sys
from pathlib import Path

RULE_RE = re.compile(r"^([a-zA-Z0-9_.\-]+)\s*:(?![:=])")
DOC_RE = re.compile(r"^([a-zA-Z0-9_.\-]+)\s*:[^=]*?##")

INTERNAL_PREFIXES = ("guard-", "print-", "_")


def undocumented(path: Path) -> list[str]:
    missing = []
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError as e:
        print(f"error: {path}: {e}", file=sys.stderr)
        return missing
    # Join backslash-continuations so a `##` on the last physical line of a
    # multi-line prerequisite list counts as the rule's documentation.
    logical = []
    buf = ""
    for raw in lines:
        buf += raw
        if buf.endswith("\\"):
            buf = buf[:-1]
            continue
        logical.append(buf)
        buf = ""
    if buf:
        logical.append(buf)
    for line in logical:
        m = RULE_RE.match(line)
        if not m:
            continue
        name = m.group(1)
        if name.startswith(".") or name.startswith("%"):
            continue
        if any(name.startswith(p) for p in INTERNAL_PREFIXES):
            continue
        if "# internal" in line:
            continue
        if DOC_RE.match(line):
            continue
        missing.append(name)
    return missing


def main() -> int:
    files = [Path(a) for a in sys.argv[1:]]
    if not files:
        print("usage: check_make_help.py <makefile>...", file=sys.stderr)
        return 2
    bad = {}
    for f in files:
        missing = undocumented(f)
        if missing:
            bad[f] = missing
    if bad:
        print("Undocumented public targets (add `## <desc>` or `# internal`):")
        for f, names in bad.items():
            for n in names:
                print(f"  {f}: {n}")
        return 1
    print(f"make-docs: all public targets documented ({len(files)} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
