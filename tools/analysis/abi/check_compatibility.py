#!/usr/bin/env python3
"""check_compatibility — semver gate on the port_* ABI surface.

Reads config/abi/exports.json (the committed baseline) plus the exported
symbols of the current build, and enforces:

  * REMOVED or RENAMED symbols  → PORT_ABI_VERSION_MAJOR must have bumped
    relative to config/abi/version.json's last released major.
  * ADDED symbols               → PORT_ABI_VERSION_MINOR must have bumped.
  * No change                   → version may stay identical.

The released version record is config/abi/version.json:
    {"last_release": {"major": 1, "minor": 0}}

If that file is absent (first release cycle), the check passes with a note —
the baseline gate in check_exports.py still catches raw drift.

Flags:
  --baseline DIR   directory holding exports.json + version.json
  --build DIR      build tree (used to locate the binary via make-style
                   EXE_DIR resolution; pass --binary to skip search)
  --binary PATH    binary to inspect directly

Exit 0 compatible, 1 incompatible, 2 on tooling failure.
"""
import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
VERSION_H = ROOT / "src/game/engine/port_api/port_api_version.h"

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_exports import dump_symbols, filter_prefix, load_baseline  # noqa: E402


def current_version() -> tuple[int, int]:
    text = VERSION_H.read_text(encoding="utf-8")
    major = int(re.search(r"PORT_ABI_VERSION_MAJOR\s+(\d+)", text).group(1))
    minor = int(re.search(r"PORT_ABI_VERSION_MINOR\s+(\d+)", text).group(1))
    return major, minor


def find_binary(build: Path) -> Path | None:
    for name in ("BattleShip.exe", "BattleShip", "BattleShip-JP.exe",
                 "BattleShip-JP"):
        for sub in ("", "Release", "Debug", "bin"):
            cand = build / sub / name
            if cand.exists():
                return cand
    return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--baseline", default=str(ROOT / "config/abi"))
    ap.add_argument("--build", default=str(ROOT / "out/build"))
    ap.add_argument("--binary")
    args = ap.parse_args()

    base = Path(args.baseline)
    exports_file = base / "exports.json"
    version_file = base / "version.json"

    if not exports_file.exists():
        print("note: no ABI baseline yet — compatibility gate inactive "
              "(run ALLOW_UPDATE=1 make abi-baseline to seed it)")
        return 0
    if not version_file.exists():
        print("note: config/abi/version.json absent — first release cycle, "
              "no released version to compare against")
        return 0

    binary = Path(args.binary) if args.binary else find_binary(Path(args.build))
    if binary is None:
        print("error: no binary found under --build; build first or pass "
              "--binary", file=sys.stderr)
        return 2

    dumped, tool = dump_symbols(binary)
    if dumped is None:
        print("error: no symbol dumper available", file=sys.stderr)
        return 2

    baseline = load_baseline(exports_file)
    current = filter_prefix(dumped, ("port_", "Port"))
    added = set(current) - set(baseline)
    removed = set(baseline) - set(current)

    released = json.loads(version_file.read_text(encoding="utf-8"))["last_release"]
    r_major, r_minor = released["major"], released["minor"]
    c_major, c_minor = current_version()

    problems = []
    if removed and c_major <= r_major:
        problems.append(
            f"{len(removed)} symbol(s) removed but major still {c_major} "
            f"(released {r_major}.{r_minor}) — bump PORT_ABI_VERSION_MAJOR")
    if added and c_major == r_major and c_minor <= r_minor:
        problems.append(
            f"{len(added)} symbol(s) added but version still "
            f"{c_major}.{c_minor} — bump PORT_ABI_VERSION_MINOR")
    if (c_major, c_minor) < (r_major, r_minor):
        problems.append(
            f"version {c_major}.{c_minor} is below released "
            f"{r_major}.{r_minor} — anti-downgrade")

    if problems:
        print("ABI compatibility FAILED:")
        for p in problems:
            print(f"  {p}")
        return 1
    print(f"abi-compat: compatible ({c_major}.{c_minor} vs released "
          f"{r_major}.{r_minor}; +{len(added)}/-{len(removed)} symbols, "
          f"via {tool})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
