#!/usr/bin/env python3
"""check_module_deps — enforce the module dependency rules.

Layer contract (docs/architecture/overview.md, proposal §13):

  application   → everything (composition root)
  presentation  → leaf modules + gameplay      — never integration internals, decomp
  gameplay/audio/social/input → leaf modules / integration / netplay — never application/presentation
  distribution  → leaf modules / platform / integration (downloads, updater,
                  signing)                    — never application/presentation
  netplay       → leaf modules / platform / integration — never presentation/features
  platform      → observability only          — never application/gameplay/...
  compatibility → decomp / observability      — the N64 shim layer, no product
  integration   → decomp / libultraship / leaf modules — the adapter layer
  observability / persistence / diagnostics / game / rendering / config
                → leaf modules only           — no product deps
  decomp        → leaf modules + features + integration — port shim +
                  hooks call-outs; never presentation/application/platform

Includes resolve in three ways: qualified `module/...` paths, same-dir
siblings, then a basename index over src/, include/, components/decomp/, components/libultraship/.

Existing violations are listed in config/lint/module-deps-baseline.txt —
the file documents real debt; the lint fails only on NEW violations.
`--rebaseline` regenerates it (use deliberately, never to hide a new dep).
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
BASELINE = ROOT / "config" / "lint" / "module-deps-baseline.txt"

HEADER_ROOTS = ["src", "include", "components/decomp/include", "components/decomp/src",
                "components/libultraship/include"]

# Include-prefix -> module dir. Covers both the old flat prefixes
# (still used by unchanged #include "netplay/..." lines, resolved through
# the group include dirs) and the new qualified group/module prefixes.
INCLUDE_PREFIX_MODULE = {
    "application": "src/application",
    "update": "src/application/update",
    "distribution": "src/application/update",
    "config": "src/config",
    "diagnostics": "src/observability",
    "observability": "src/observability",
    "persistence": "src/persistence",
    "modding": "src/game/modding",
    "gameplay": "src/game/gameplay",
    "integration": "src/game/engine",
    "audio": "src/interfaces/audio",
    "input": "src/interfaces/input",
    "presentation": "src/interfaces/presentation",
    "rendering": "src/interfaces/rendering",
    "netplay": "src/networking/netplay",
    "social": "src/networking/social",
    "platform": "src/platform",
    "compatibility": "src/platform/compatibility",
}
GROUP_PREFIXES = ("game/", "interfaces/", "networking/")

# module -> set of src modules it may NOT include. 'decomp' handled separately.
ALL_PRODUCT = {"src/application", "src/application/update",
               "src/interfaces/presentation", "src/game/gameplay",
               "src/interfaces/audio", "src/networking/social",
               "src/interfaces/input", "src/networking/netplay",
               "src/platform", "src/platform/compatibility",
               "src/game/engine", "src/game/modding",
               "src/game/engine/relocation"}

# module -> set of src modules it may NOT include. 'decomp' handled separately.
FORBIDDEN = {
    # leaf modules — no product deps (external/decomp headers only)
    "src/observability": ALL_PRODUCT | {"src/persistence",
                          "src/config", "src/i18n"},
    # leaf modules may log through observability — it's the shared sink.
    "src/persistence":   ALL_PRODUCT | {"src/config",
                          "src/i18n"},
    "src/config":        ALL_PRODUCT | {"src/persistence",
                          "src/i18n"},
    # i18n leaf — may use platform filesystem probing (lang/ lookup) and the
    # observability log sink; nothing else.
    "src/i18n":          ALL_PRODUCT - {"src/platform"}
                          | {"src/config", "src/persistence"},
    # rendering/cosmetics + widescreen are gameplay features that live in the
    # rendering module — they may pull the gameplay facade (enhancements.h).
    "src/interfaces/rendering": ALL_PRODUCT - {"src/game/gameplay"}
                          | {"src/persistence", "src/config"},
    "src/platform/compatibility": {"src/application", "src/interfaces/presentation",
                          "src/game/gameplay", "src/interfaces/audio",
                          "src/networking/social", "src/interfaces/input",
                          "src/application/update", "src/networking/netplay",
                          "src/game/engine", "src/game/modding",
               "src/game/engine/relocation"},
    "src/platform":      {"src/application", "src/interfaces/presentation",
                          "src/game/gameplay", "src/interfaces/audio",
                          "src/networking/social", "src/interfaces/input",
                          "src/application/update", "src/networking/netplay",
                          "src/game/engine", "src/game/modding",
                          "src/game/engine/relocation",
                          "src/interfaces/rendering",
                          "src/persistence", "src/config",
                          "src/i18n"},
    # game/engine (facade + components/decomp/hooks adapter layer) and game/modding:
    # allowed to see platform/components/decomp/libultraship — never the
    # product top layers.
    "src/game/engine":   {"src/application", "src/application/update",
                          "src/interfaces/presentation"},
    # relocation is the shared asset-reloc ABI — engine peers plus the
    # consumers granted in SSB64_MODULE_SCOPE_* (compatibility/resources/
    # decomp). Same product restrictions as engine itself.
    "src/game/engine/relocation": {"src/application", "src/application/update",
                          "src/interfaces/presentation"},
    "src/game/modding":  {"src/application", "src/interfaces/presentation"},
    "src/application/update":  {"src/application", "src/interfaces/presentation"},
    "src/game/gameplay":      {"src/application", "src/interfaces/presentation"},
    "src/interfaces/audio":         {"src/application", "src/interfaces/presentation"},
    "src/networking/social":        {"src/application", "src/interfaces/presentation"},
    "src/interfaces/input":         {"src/application", "src/interfaces/presentation"},
    "src/networking/netplay":       {"src/application", "src/interfaces/presentation",
                          "src/game/gameplay", "src/interfaces/audio",
                          "src/networking/social", "src/interfaces/input",
                          "src/game/engine/relocation",
                          "src/application/update"},
    "src/interfaces/presentation":  {"src/game/engine", "src/game/modding",
                          "decomp"},
    # application: top of the stack — everything allowed.
    "decomp":            {"src/interfaces/presentation", "src/application",
                          "src/platform"},
}

INC = re.compile(r'#\s*include\s*[<"]([^>"]+)[>"]')

# C/C++ standard headers shadowed by identically-named decomp headers must
# still classify as external (components/decomp/include/math.h etc. exist).
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
        # engine/relocation is its own module — shared ABI consumed by
        # compatibility/resources/decomp per CMake SSB64_MODULE_SCOPE_*.
        if parts[1] == 'game' and parts[2] == 'engine' and \
                len(parts) > 3 and parts[3] == 'relocation':
            return 'src/game/engine/relocation'
        # Group dirs hold leaf modules one level deeper.
        if parts[1] in ('core', 'game', 'interfaces', 'networking'):
            return f"src/{parts[1]}/{parts[2]}"
        if parts[1] == 'application' and parts[2] == 'update':
            return 'src/application/update'
        if parts[1] == 'platform' and parts[2] == 'compatibility':
            return 'src/platform/compatibility'
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
        head = inc.split('/')[0]
        if inc.startswith(GROUP_PREFIXES) and '/' in inc:
            sub = inc.split('/')
            tm = f"src/{sub[0]}/{sub[1]}" if len(sub) > 2 else 'src/' + sub[0]
        elif head in INCLUDE_PREFIX_MODULE:
            tm = INCLUDE_PREFIX_MODULE[head]
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
    for base_dir in ('src', 'components/decomp/src'):
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
    return {l.strip() for l in
            BASELINE.read_text(encoding='utf-8').splitlines()
            if l.strip() and not l.startswith('#')}


def main() -> int:
    viol = collect()
    if '--rebaseline' in sys.argv:
        BASELINE.parent.mkdir(parents=True, exist_ok=True)
        BASELINE.write_text(
            "# Known module-dependency violations (tracked debt — shrink, "
            "never grow).\n# Regenerate: python tools/analysis/lint/"
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
