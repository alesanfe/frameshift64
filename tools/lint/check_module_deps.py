#!/usr/bin/env python3
"""check_module_deps — enforce the module dependency rules.

Layer contract (docs/architecture/overview.md, proposal §13):

  application → presentation / features / netplay / bridge / core / platform
  presentation → core (+ interfaces)         — never bridge internals, decomp
  features    → core / bridge / netplay      — never application/presentation
  netplay     → core / platform / bridge     — never presentation
  resources   → core / bridge               — never application/presentation
  platform    → core                         — never application/features/...
  compatibility → decomp / core              — the N64 shim layer, no product
  bridge      → decomp / libultraship / core — the adapter layer
  core        → no product module            — external/decomp headers only
  decomp      → src/core + src/features only — port shim + hooks call-outs;
                never presentation/application/platform

Includes resolve in three ways: qualified `module/...` paths, same-dir
siblings, then a basename index over src/, include/, decomp/, libultraship/.

Existing violations are listed in config/lint/module-deps-baseline.txt —
the file documents real debt; the lint fails only on NEW violations.
`--rebaseline` regenerates it (use deliberately, never to hide a new dep).
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BASELINE = ROOT / "config" / "lint" / "module-deps-baseline.txt"

HEADER_ROOTS = ["src", "include", "decomp/include", "decomp/src",
                "libultraship/include"]

MODULES = ("application", "bridge", "core", "features", "netplay",
           "platform", "presentation", "resources", "compatibility")

# module -> set of src modules it may NOT include. 'decomp' handled separately.
FORBIDDEN = {
    "src/core":          {"src/application", "src/presentation", "src/features",
                          "src/netplay", "src/platform", "src/resources",
                          "src/bridge"},
    "src/compatibility": {"src/application", "src/presentation", "src/features",
                          "src/netplay", "src/bridge"},
    "src/platform":      {"src/application", "src/presentation", "src/features",
                          "src/netplay", "src/bridge", "src/resources"},
    "src/resources":     {"src/application", "src/presentation", "src/features",
                          "src/netplay"},
    "src/features":      {"src/application", "src/presentation"},
    "src/netplay":       {"src/application", "src/presentation", "src/features",
                          "src/resources"},
    "src/presentation":  {"src/bridge", "decomp"},
    # bridge, application: top of the stack — everything allowed.
    "decomp":            {"src/presentation", "src/application",
                          "src/platform"},
}

INC = re.compile(r'#\s*include\s*[<"]([^>"]+)[>"]')

# C/C++ standard headers shadowed by identically-named decomp headers must
# still classify as external (decomp/include/math.h etc. exist).
STDLIB = {
    "assert.h", "ctype.h", "errno.h", "fenv.h", "float.h", "inttypes.h",
    "limits.h", "locale.h", "math.h", "setjmp.h", "signal.h", "stdarg.h",
    "stdbool.h", "stddef.h", "stdint.h", "stdio.h", "stdlib.h", "string.h",
    "time.h", "wchar.h", "wctype.h", "algorithm", "array", "atomic",
    "bitset", "cassert", "cctype", "cerrno", "cfenv", "cfloat", "chrono",
    "cinttypes", "climits", "clocale", "cmath", "condition_variable",
    "csetjmp", "csignal", "cstdarg", "cstdbool", "cstddef", "cstdint",
    "cstdio", "cstdlib", "cstring", "ctime", "cwchar", "cwctype", "deque",
    "exception", "filesystem", "fstream", "functional", "initializer_list",
    "iomanip", "ios", "iostream", "istream", "iterator", "limits", "list",
    "locale", "map", "memory", "mutex", "new", "numeric", "optional",
    "ostream", "queue", "set", "shared_mutex", "sstream", "stack",
    "stdexcept", "string", "string_view", "system_error", "thread",
    "tuple", "type_traits", "typeinfo", "unordered_map", "unordered_set",
    "utility", "variant", "vector",
}


def build_index() -> dict[str, list[str]]:
    idx: dict[str, list[str]] = {}
    for d in HEADER_ROOTS:
        base = ROOT / d
        if not base.exists():
            continue
        for p in base.rglob('*'):
            if p.suffix in ('.h', '.hpp'):
                idx.setdefault(p.name, []).append(
                    p.relative_to(ROOT).as_posix())
    return idx


def module_of(rel: str) -> str:
    parts = rel.split('/')
    if parts[0] == 'src' and len(parts) > 2:
        return 'src/' + parts[1]
    if parts[0] == 'decomp':
        return 'decomp'
    if parts[0] == 'include':
        return 'include'
    if parts[0] == 'libultraship':
        return 'libultraship'
    return 'external'


def scan_file(path: Path, idx) -> list[tuple[str, str]]:
    """Return [(target_module, include)] for each cross-module include."""
    rel = path.relative_to(ROOT).as_posix()
    fm = module_of(rel)
    out = []
    for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
        m = INC.search(line)
        if not m:
            continue
        inc = m.group(1)
        if inc.startswith(tuple(f"{mod}/" for mod in MODULES)):
            tm = 'src/' + inc.split('/')[0]
        elif inc.startswith('smash_classic/'):
            tm = 'include'
        elif inc in STDLIB:
            tm = 'external'
        elif (path.parent / inc).exists():
            tm = fm  # same-dir sibling
        else:
            cands = idx.get(Path(inc).name, [])
            tm = module_of(cands[0]) if cands else 'external'
        if tm != fm:
            out.append((tm, inc))
    return out


def collect() -> list[str]:
    idx = build_index()
    viol = []
    for base_dir in ('src', 'decomp/src'):
        for f in sorted((ROOT / base_dir).rglob('*')):
            if f.suffix not in ('.c', '.cpp', '.h', '.hpp'):
                continue
            fm = module_of(f.relative_to(ROOT).as_posix())
            for tm, inc in scan_file(f, idx):
                if tm in FORBIDDEN.get(fm, set()):
                    viol.append(
                        f"{f.relative_to(ROOT).as_posix()} -> {inc}")
    return sorted(set(viol))


def load_baseline() -> set[str]:
    if not BASELINE.exists():
        return set()
    return {l.strip() for l in BASELINE.read_text().splitlines()
            if l.strip() and not l.startswith('#')}


def main() -> int:
    viol = collect()
    if '--rebaseline' in sys.argv:
        BASELINE.parent.mkdir(parents=True, exist_ok=True)
        BASELINE.write_text(
            "# Known module-dependency violations (tracked debt — shrink, "
            "never grow).\n# Regenerate: python tools/lint/"
            "check_module_deps.py --rebaseline\n" +
            "\n".join(viol) + "\n")
        print(f"baseline rewritten: {len(viol)} known violation(s)")
        return 0
    known = load_baseline()
    new = [v for v in viol if v not in known]
    fixed = [v for v in known if v not in viol]
    if fixed:
        print("check_module_deps: violations FIXED since baseline "
              "(remove them from module-deps-baseline.txt):")
        for v in fixed:
            print(f"  {v}")
    if new:
        print("check_module_deps: NEW forbidden dependencies:")
        for v in new:
            print(f"  {v}")
        return 1
    print(f"check_module_deps: {len(viol)} known violation(s), 0 new")
    return 0


if __name__ == '__main__':
    sys.exit(main())
