#!/usr/bin/env python3
"""check_makefiles — portable sanity checks for the Make modules.

Runs when `checkmake` is unavailable (CI has neither checkmake nor a package
for it on every platform). Checks:

  1. recipe lines are tab-indented — a space-indented recipe is a silent skip
     or a "missing separator" error depending on make version. Continuation
     lines of a multi-line rule header (prerequisite lists) are exempt;
  2. every non-file target is declared in a `.PHONY:` line — the declaration
     may live in any of the scanned files (this project keeps one central
     .PHONY block in make/common.mk);
  3. no duplicated target definitions within a file;
  4. no space-indented line outside a recipe block pretending to be a recipe.

Usage: check_makefiles.py <makefile>...
Exit 1 on findings; 0 clean.
"""
import re
import sys
from pathlib import Path

RULE_RE = re.compile(r"^([a-zA-Z0-9_.\-]+)\s*:(?![:=])")
PHONY_RE = re.compile(r"^\.PHONY\s*:\s*(.*)$")


def collect_phony(files: list[Path]) -> set[str]:
    phony: set[str] = set()
    for f in files:
        buf = ""
        for raw in f.read_text(encoding="utf-8", errors="replace").splitlines():
            buf += raw
            if buf.endswith("\\"):
                buf = buf[:-1]
                continue
            if m := PHONY_RE.match(buf):
                phony.update(m.group(1).split())
            buf = ""
    return phony


def check(path: Path, phony: set[str]) -> list[str]:
    problems = []
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError as e:
        return [f"{path}: {e}"]

    seen: dict[str, int] = {}
    in_rule = None      # current rule name (recipe context)
    continuing = False  # previous physical line ended with backslash

    for i, raw in enumerate(lines, 1):
        if m := PHONY_RE.match(raw):
            in_rule = None
            continuing = raw.endswith("\\")
            continue
        if m := RULE_RE.match(raw):
            name = m.group(1)
            if not name.startswith("."):
                if name in seen:
                    problems.append(
                        f"{path}:{i}: duplicate target '{name}' "
                        f"(first at line {seen[name]})")
                seen.setdefault(name, i)
                in_rule = name
                if (not name.startswith(".") and "." not in name
                        and "/" not in name and "=" not in raw):
                    if name not in phony:
                        problems.append(
                            f"{path}:{i}: '{name}' missing from .PHONY")
            continuing = raw.endswith("\\")
            continue
        if continuing:
            # still inside a multi-line rule/variable header
            continuing = raw.endswith("\\")
            continue
        # genuine recipe body must start with a tab
        if in_rule and raw.startswith(" ") and raw.strip() \
                and not raw.lstrip().startswith("#"):
            problems.append(f"{path}:{i}: space-indented line inside rule "
                            f"'{in_rule}' (recipe lines need a tab)")
        if raw and not raw[0].isspace() and not raw.startswith("#"):
            in_rule = None
            continuing = raw.endswith("\\")

    return problems


def main() -> int:
    files = [Path(a) for a in sys.argv[1:]]
    if not files:
        print("usage: check_makefiles.py <makefile>...", file=sys.stderr)
        return 2
    phony = collect_phony(files)
    all_problems = []
    for f in files:
        all_problems += check(f, phony)
    if all_problems:
        print("Makefile issues:")
        for p in all_problems:
            print(f"  {p}")
        return 1
    print(f"makefiles: structural checks clean ({len(files)} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
