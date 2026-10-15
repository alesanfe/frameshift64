#!/usr/bin/env python3
"""check_env_vars — every SSB64_* env var read by the binary must be catalogued.

Scans source for env reads (getenv / std::getenv / GetEnvironmentVariable /
the GetEnvInt wrappers) and fails if any `SSB64_*` name is not documented in
docs/development/env_vars.md. Brace groups like `SSB64_SIM_{A,B}` in the doc
expand to the covered names.

Exit 0 = catalog complete. Exit 1 = undocumented env reads (list printed).
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
DOC = ROOT / "docs" / "development" / "env_vars.md"

SCAN_DIRS = ["src", "components/decomp/src", "include", "platforms", "services", "tools", "mods"]
SRC_EXT = {".c", ".cpp", ".h", ".hpp", ".cc", ".m", ".mm", ".js", ".ts"}

ENV_READ = re.compile(r'(?:getenv|GetEnvironmentVariable|GetEnvInt|env_uint)\s*\(')
VAR_LIT = re.compile(r'\bSSB64_[A-Z0-9_]+')
DOC_VAR = re.compile(r'SSB64_[A-Z0-9_{},]+')


def brace_expand(pattern: str) -> set[str]:
    """Expand one level of `{a,b,c}` groups inside a doc token."""
    m = re.search(r'\{([^{}]+)\}', pattern)
    if m is None:
        return {pattern}
    out = set()
    for alt in m.group(1).split(','):
        out |= brace_expand(pattern[:m.start()] + alt + pattern[m.end():])
    return out


def documented() -> set[str]:
    text = DOC.read_text(encoding='utf-8')
    names: set[str] = set()
    for tok in DOC_VAR.findall(text):
        names |= brace_expand(tok)
    return names


def discovered() -> dict[str, list[str]]:
    found: dict[str, list[str]] = {}
    for d in SCAN_DIRS:
        base = ROOT / d
        if not base.exists():
            continue
        for path in base.rglob('*'):
            if path.suffix not in SRC_EXT or not path.is_file():
                continue
            try:
                lines = path.read_text(encoding='utf-8', errors='replace').splitlines()
            except OSError:
                continue
            for i, line in enumerate(lines, 1):
                if not ENV_READ.search(line):
                    continue
                for name in VAR_LIT.findall(line):
                    found.setdefault(name, []).append(f"{path.relative_to(ROOT)}:{i}")
    return found


def main() -> int:
    if not DOC.exists():
        print(f"check_env_vars: missing {DOC.relative_to(ROOT)}")
        return 1
    doc = documented()
    found = discovered()
    missing = {k: v for k, v in found.items() if k not in doc}
    if missing:
        print("check_env_vars: undocumented env reads:")
        for name in sorted(missing):
            for site in missing[name]:
                print(f"  {name}  ({site})")
        print(f"\ndocument them in {DOC.relative_to(ROOT)}")
        return 1
    print(f"check_env_vars: {len(found)} env var(s) catalogued, 0 undocumented")
    return 0


if __name__ == '__main__':
    sys.exit(main())
