"""File-size ratchet for the modern port layer.

No new file may exceed SIZE_LIMIT lines; files already over the limit are
pinned at their current size in config/file-size-baseline.txt so they can
only shrink, never grow.

    python tools/analysis/lint/check_file_size.py           # check vs baseline
    python tools/analysis/lint/check_file_size.py --update  # regenerate baseline
    python tools/analysis/lint/check_file_size.py --report  # list oversize files

Same ratchet contract as check_complexity.py: a baseline entry is a cap,
not a quota — shrinking a file under its cap is a NOTE prompting baseline
regeneration that locks the improvement in.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
BASELINE = ROOT / "config" / "file-size-baseline.txt"

SCAN_DIRS = ["src", "include", "platforms", "tools", "services", "mods"]
EXTENSIONS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx",
              ".mjs", ".js", ".py", ".ps1", ".sh", ".bat"}
SKIP_PARTS = {"node_modules", "__pycache__", ".venv-lint", "build", "out",
              ".wrangler"}
SIZE_LIMIT = 1000


def collect_sizes() -> dict[str, int]:
    """relpath -> line count for every scannable own-code file."""
    sizes: dict[str, int] = {}
    for top in SCAN_DIRS:
        base = ROOT / top
        if not base.exists():
            continue
        for path in base.rglob("*"):
            if (not path.is_file() or path.suffix.lower() not in EXTENSIONS or
                    SKIP_PARTS & set(path.parts)):
                continue
            try:
                n = sum(1 for _ in path.open(encoding="utf-8", errors="replace"))
            except OSError:
                continue
            sizes[path.relative_to(ROOT).as_posix()] = n
    return sizes


def load_baseline() -> dict[str, int]:
    out: dict[str, int] = {}
    if not BASELINE.exists():
        return out
    for line in BASELINE.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        path, _, size = line.rpartition(" ")
        out[path.strip()] = int(size)
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--update", action="store_true",
                    help="pin current sizes of over-limit files as the caps")
    ap.add_argument("--report", action="store_true",
                    help="list every file over SIZE_LIMIT")
    args = ap.parse_args()

    sizes = collect_sizes()
    oversize = {p: n for p, n in sizes.items() if n > SIZE_LIMIT}

    if args.report:
        for path, n in sorted(oversize.items(), key=lambda kv: -kv[1]):
            print(f"{path}: {n} lines")
        print(f"{len(oversize)} file(s) over {SIZE_LIMIT} lines")

    if args.update:
        lines = [f"{p} {n}" for p, n in sorted(oversize.items())]
        BASELINE.write_text(
            "# Per-file line-count caps for files already over the "
            f"{SIZE_LIMIT}-line limit.\n"
            "# Regenerate with: python tools/analysis/lint/check_file_size.py "
            "--update\n" + "\n".join(lines) + "\n", encoding="utf-8")
        print(f"baseline -> {BASELINE.relative_to(ROOT)} "
              f"({len(lines)} oversize file(s) capped)")
        return 0

    baseline = load_baseline()
    regressions = []
    notes = []
    for path, n in sorted(oversize.items()):
        cap = baseline.get(path, SIZE_LIMIT)
        if n > cap:
            regressions.append((path, n, cap))
        elif n < cap:
            notes.append((path, n, cap))
    for path, cap in sorted(baseline.items()):
        if path not in oversize:
            notes.append((path, sizes.get(path, 0), cap))

    for path, n, cap in regressions:
        print(f"FAIL {path}: {n} lines > cap {cap}")
    for path, n, cap in notes:
        print(f"NOTE {path}: {n} lines now (cap {cap}) — shrink the baseline "
              f"to lock it in")
    if regressions:
        print(f"\n{len(regressions)} file(s) grew past their cap. Split the "
              f"file under {SIZE_LIMIT} lines, or if the growth is "
              "intentional run --update and justify it in review.")
        return 1
    print(f"file-size ratchet OK: {len(oversize)} oversize file(s), all "
          f"within caps (new files must stay under {SIZE_LIMIT})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
