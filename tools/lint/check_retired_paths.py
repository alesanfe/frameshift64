#!/usr/bin/env python3
"""check_retired_paths — the pre-reorganization layout must not come back.

Two checks, both cheap:

1. No retired top-level directory exists. The old paths must never be
   reintroduced — their replacements are listed for the error message.

2. No first-party source/build file resolves an old path. Scans #include
   directives in src/ and path literals in CMakeLists.txt + cmake/ for
   the retired prefixes. Documentation is exempt on purpose: bug
   write-ups in docs/ legitimately describe the old layout.

Exits 0 when clean, 1 listing every violation.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# retired path -> its replacement
RETIRED_DIRS = {
    "port": "src/",
    "server": "services/",
    "debug_tools": "tools/debugging/",
    "workspace": "mods/",
    "android": "platforms/android/",
    "uwp": "platforms/uwp/",
    "ci": ".github/workflows/ + scripts/ci/",
    "yamls": "assets/configuration/extraction/",
}

# path fragments that must not appear in source includes / build files
RETIRED_REF = re.compile(
    r'(port/(?:bridge|net|gui|audio|resource|interpolation|widescreen'
    r'|enhancements|mods|hooks|stubs|assets)/'
    r'|server/cloudflare'
    r'|debug_tools/'
    r'|workspace/(?:hooktest|playertint|template))'
)

SCAN_EXT = {".c", ".cpp", ".h", ".hpp", ".cmake", ".txt"}


def main() -> int:
    failures = []

    for retired, replacement in RETIRED_DIRS.items():
        if (ROOT / retired).is_dir():
            failures.append(
                f"retired directory '{retired}/' exists again — use '{replacement}'"
            )

    for base in ("src", "cmake"):
        for path in (ROOT / base).rglob("*"):
            if path.suffix not in SCAN_EXT or not path.is_file():
                continue
            for lineno, line in enumerate(
                path.read_text(encoding="utf-8", errors="replace").splitlines(), 1
            ):
                hit = RETIRED_REF.search(line)
                if hit:
                    rel = path.relative_to(ROOT)
                    failures.append(f"{rel}:{lineno}: old path '{hit.group(1)}'")

    for line in (ROOT / "CMakeLists.txt").read_text(encoding="utf-8").splitlines():
        hit = RETIRED_REF.search(line)
        if hit:
            failures.append(f"CMakeLists.txt: old path '{hit.group(1)}'")

    if failures:
        print("check_retired_paths FAILED:")
        for f in failures:
            print(f"  {f}")
        return 1
    print("check_retired_paths: clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
