#!/usr/bin/env python3
"""check_source_encoding — reject corrupted source text in tracked files.

Catches the failure modes scripted edits keep reintroducing:
  * NUL bytes / control chars inside source (a `\\0` escape collapse)
  * invalid UTF-8 (a cp1252 write of UTF-8 content)
  * mixed CRLF+LF line endings in one file
  * a literal newline inside a C string literal is not directly detectable
    at this layer, but unbalanced-quote lines in .c/.cpp/.h are flagged as
    a heuristic (see --quotes).

Scope: git-tracked files matching source extensions under src/, include/,
tools/, tests/, make/, plus *.mk/CMakeLists — not generated/ or out/.

Flags:
  --quotes   additionally flag lines with an odd number of unescaped `"`
             in C/C++ sources (heuristic, may be noisy — off by default)

Exit 0 clean, 1 on findings.
"""
import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

SOURCE_EXT = {
    ".c", ".h", ".cpp", ".hpp", ".cc", ".hh", ".inl",
    ".py", ".js", ".mjs", ".ts", ".json", ".yml", ".yaml",
    ".mk", ".sh", ".ps1", ".psm1", ".cmake",
}
SOURCE_NAMES = {"Makefile", "CMakeLists.txt", "CMakePresets.json",
                ".clang-format", ".gitignore", ".gitmodules"}
QUOTE_EXT = {".c", ".h", ".cpp", ".hpp", ".cc", ".hh", ".inl"}

CONTROL_BAD = set(range(0x00, 0x09)) | {0x0B, 0x0C} | set(range(0x0E, 0x20))


def tracked_files() -> list[Path]:
    r = subprocess.run(["git", "ls-files"], cwd=ROOT, capture_output=True,
                       text=True, check=True)
    out = []
    for rel in r.stdout.splitlines():
        p = ROOT / rel
        if p.suffix in SOURCE_EXT or p.name in SOURCE_NAMES:
            if rel.startswith(("out/", "generated/", ".splat/")):
                continue
            out.append(p)
    return out


def check_file(path: Path, quotes: bool) -> list[str]:
    problems = []
    try:
        data = path.read_bytes()
    except OSError:
        return problems
    if b"\0" in data:
        problems.append("contains NUL byte(s)")
    bad_ctrl = [b for b in data if b in CONTROL_BAD]
    if bad_ctrl:
        problems.append(f"contains control byte(s): "
                        f"{sorted(set(hex(b) for b in bad_ctrl))}")
    try:
        data.decode("utf-8")
    except UnicodeDecodeError as e:
        problems.append(f"invalid UTF-8 at byte {e.start}")
    crlf = data.count(b"\r\n")
    lf = data.count(b"\n") - crlf
    if crlf and lf:
        problems.append(f"mixed line endings ({crlf} CRLF + {lf} LF)")
    if quotes and path.suffix in QUOTE_EXT:
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError:
            return problems
        for i, line in enumerate(text.splitlines(), 1):
            # count unescaped double quotes
            n, j = 0, 0
            while j < len(line):
                if line[j] == "\\":
                    j += 2
                    continue
                if line[j] == '"':
                    n += 1
                j += 1
            if n % 2:
                problems.append(f"line {i}: odd number of unescaped quotes")
    return problems


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--quotes", action="store_true")
    args = ap.parse_args()

    findings = []
    files = tracked_files()
    for f in files:
        for prob in check_file(f, args.quotes):
            findings.append(f"{f.relative_to(ROOT)}: {prob}")
    if findings:
        print("Source encoding problems:")
        for f in findings:
            print(f"  {f}")
        return 1
    print(f"encoding: {len(files)} tracked source files clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
