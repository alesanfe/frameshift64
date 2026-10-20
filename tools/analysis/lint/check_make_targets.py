#!/usr/bin/env python3
"""check_make_targets — flag references to Make targets that are not defined.

Collects every `name:` target from the given makefiles, then scans recipe
lines and prerequisite lists for invocations of sibling targets:

  * `$(MAKE) foo` / `make foo` inside recipes
  * `target: dep1 dep2` prerequisite names

A referenced name that is neither a defined target, a known variable, a file
path (contains `/` or `.`), nor a `-flag` is reported. False positives from
prose in comments are excluded by stripping comments first.

Usage: check_make_targets.py --makefiles Makefile make/*.mk
       [--search dir ...]   (also scans docs/CI for `make <target>` refs)

Exit 1 on undefined references; 0 clean.
"""
import argparse
import re
import sys
from pathlib import Path

RULE_RE = re.compile(r"^([a-zA-Z0-9_.\-]+)\s*:(?![:=])\s*(.*)$")
VAR_RE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)\s*[:+?]?=")
MAKE_CALL_RE = re.compile(r"(?:\$\(MAKE\)|\bmake\b)\s+(?:-[a-zA-Z]+\s+)*"
                          r"([a-zA-Z][a-zA-Z0-9_\-]*)")

# Things that look like targets but are variables/options/context.
IGNORED = {
    "sure", "the", "a", "it", "this", "every", "sense", "raw",
    "audio", "clang", "online", "peers", "wall-clock", "second-iteration",
    "extracted", "freezes", "further", "nproc", "j",
}


def logical_lines(path: Path) -> list[tuple[int, str]]:
    """Yield (first_lineno, logical_line) with backslash-continuations joined."""
    out = []
    buf, start = "", 0
    for i, raw in enumerate(
        path.read_text(encoding="utf-8", errors="replace").splitlines(), 1
    ):
        if not buf:
            start = i
        buf += raw
        if buf.endswith("\\"):
            buf = buf[:-1]
            continue
        out.append((start, buf))
        buf = ""
    if buf:
        out.append((start, buf))
    return out
VARS_BUILTIN = {
    "C", "j", "n", "k", "f", "s", "B", "C", "w", "W", "q", "p", "r",
    "MAKE", "MAKEFLAGS", "MAKEFILE_LIST", "CURDIR", "SHELL", "MAKELEVEL",
}


def defined_targets(files: list[Path]) -> set[str]:
    names = set()
    for f in files:
        for _, line in logical_lines(f):
            if m := RULE_RE.match(line):
                if not m.group(1).startswith("."):
                    names.add(m.group(1))
            if m := VAR_RE.match(line):
                names.add(m.group(1))
    return names


def scan_makefiles(files: list[Path], defined: set[str]) -> list[str]:
    problems = []
    for f in files:
        for i, raw in logical_lines(f):
            line = raw.split("#", 1)[0]
            # prerequisite names: `target: dep dep`
            if m := RULE_RE.match(line):
                if m.group(1).startswith("."):
                    continue  # .PHONY/.DEFAULT_GOAL/etc list names, not deps
                for dep in m.group(2).split():
                    dep = dep.strip()
                    if (not dep or dep in defined or dep in VARS_BUILTIN
                            or dep.startswith(("$", "%", "-", "@", "|"))
                            or "/" in dep or "." in dep or "=" in dep):
                        continue
                    if dep.isupper():
                        continue
                    problems.append(f"{f}:{i}: prerequisite '{dep}' "
                                    f"is not a defined target")
            # `$(MAKE) foo` / `make foo` in recipes
            for mm in MAKE_CALL_RE.finditer(line):
                name = mm.group(1)
                if (name in defined or name in VARS_BUILTIN
                        or name.isupper() or name in IGNORED):
                    continue
                problems.append(f"{f}:{i}: 'make {name}' has no target '{name}'")
    return problems


def scan_search_dirs(dirs: list[str], defined: set[str]) -> list[str]:
    problems = []
    pat = re.compile(r"\bmake\s+(?:-[a-zA-Z]+\s+)*([a-zA-Z][a-zA-Z0-9_\-]*)")
    for d in dirs:
        root = Path(d)
        if not root.exists():
            continue
        files = [root] if root.is_file() else [
            p for p in root.rglob("*")
            if p.suffix in (".md", ".yml", ".yaml", ".ps1", ".sh", ".py")
        ]
        for p in files:
            try:
                text = p.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            for i, line in enumerate(text.splitlines(), 1):
                for mm in pat.finditer(line):
                    name = mm.group(1)
                    if (name in defined or name in IGNORED or name.isupper()
                            or name in VARS_BUILTIN):
                        continue
                    # likely prose ("make sure", "make it") — lowercase word
                    # followed by another lowercase word is sentence noise
                    ctx = line[mm.end():mm.end() + 24]
                    if re.match(r"\s+(sure|it|the|a|an|this|sense|sense|up|"
                                r"out|use|room|way|progress|decisions|contact)",
                                ctx):
                        continue
                    problems.append(
                        f"{p}:{i}: 'make {name}' — no such target")
    return problems


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--makefiles", nargs="+", required=True)
    ap.add_argument("--search", nargs="*", default=[])
    args = ap.parse_args()

    files = [Path(p) for p in args.makefiles]
    defined = defined_targets(files)
    problems = scan_makefiles(files, defined)
    if args.search:
        problems += scan_search_dirs(args.search, defined)

    if problems:
        print("Undefined Make target references:")
        for p in sorted(set(problems)):
            print(f"  {p}")
        return 1
    print(f"make-targets: all references resolve ({len(defined)} targets)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
