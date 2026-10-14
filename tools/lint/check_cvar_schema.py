#!/usr/bin/env python3
"""check_cvar_schema.py — every literal `g*` CVar key used in src/ must be
declared in src/config/schema.cpp (TD-005).

The schema is the single catalog of first-party config keys: it drives
boot-time range clamping, type-mismatch warnings, the unknown-key policy,
and the netplay affects_gameplay metadata. A CVar read in code that isn't
in the catalog silently opts out of all of it — this check fails CI when
that happens.

Keys composed at runtime via string tables (e.g. the AnalogRemap per-player
arrays) still appear as string literals in the .cpp and are covered.
"""
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SRC_DIR = REPO_ROOT / "src"
SCHEMA = SRC_DIR / "config" / "schema.cpp"

# A CVar key: "g" + uppercase + dotted segments, e.g. "gNetplay.Delay".
# The g<Upper> anchor excludes false positives like "gameloop.h",
# "gl_FragColor.rgb", and "game".
KEY_RE = re.compile(r'"(g[A-Z][A-Za-z0-9_]*(?:\.[A-Za-z0-9_]+)*)"')


def keys_in(path: Path):
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return set()
    return set(KEY_RE.findall(text))


def main() -> int:
    if not SCHEMA.is_file():
        print(f"check_cvar_schema: {SCHEMA} missing")
        return 1

    schema_keys = keys_in(SCHEMA)
    missing = {}
    for path in sorted(SRC_DIR.rglob("*")):
        if path.suffix not in (".c", ".cpp", ".h", ".hpp"):
            continue
        for key in keys_in(path) - schema_keys:
            missing.setdefault(key, []).append(path.relative_to(REPO_ROOT).as_posix())

    if missing:
        print("check_cvar_schema: g* CVar keys used in src/ but not declared in "
              "src/config/schema.cpp:")
        for key in sorted(missing):
            print(f"  {key}")
            for src in missing[key][:3]:
                print(f"      {src}")
        return 1

    print(f"check_cvar_schema: {len(schema_keys)} schema keys cover all g* "
          "literals in src/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
