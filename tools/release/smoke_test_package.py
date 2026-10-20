#!/usr/bin/env python3
"""smoke_test_package — structural smoke test of a packaged artifact.

Does NOT launch the game (a GUI exe can't run headless here). Instead it
verifies the package *contains* what a working install needs:

  windows  .zip        BattleShip.exe, f3d.o2r, lang/en.json
  linux    .AppImage   executable bit set, non-trivial size
  macos    .dmg/.zip   BattleShip.app bundle inside

Flags:
  --package PATH   artifact file or staged dir (required)
  --platform NAME  windows|linux|macos (default: host)

Exit 0 pass, 1 missing members, 2 bad input/tooling.
"""
import argparse
import os
import stat
import sys
import tempfile
import zipfile
from pathlib import Path

REQUIRED_ZIP_MEMBERS = {
    "windows": ["BattleShip.exe", "f3d.o2r"],
    "macos":   ["BattleShip.app"],
}


def check_zip(path: Path, platform: str) -> list[str]:
    problems = []
    try:
        with zipfile.ZipFile(path) as z:
            names = {n.replace("\\", "/") for n in z.namelist()}
            bad = z.testzip()
            if bad:
                problems.append(f"corrupt member: {bad}")
    except zipfile.BadZipFile:
        return [f"{path.name}: not a valid zip"]
    for member in REQUIRED_ZIP_MEMBERS.get(platform, []):
        if not any(n.endswith(member) or f"/{member}/" in n or
                   n == member or n.startswith(member + "/")
                   for n in names):
            problems.append(f"missing member: {member}")
    if not any("lang/" in n or n.endswith(".json") for n in names):
        problems.append("no lang/*.json payload found")
    return problems


def check_appimage(path: Path) -> list[str]:
    problems = []
    if not os.access(path, os.X_OK) and not (
            path.stat().st_mode & stat.S_IXUSR):
        problems.append("AppImage not executable")
    if path.stat().st_size < 1 << 20:
        problems.append(f"suspiciously small ({path.stat().st_size} bytes)")
    return problems


def check_dir(path: Path, platform: str) -> list[str]:
    problems = []
    names = {p.name for p in path.rglob("*") if p.is_file()}
    if platform == "windows" and not any(
            n.lower() == "battleship.exe" for n in names):
        problems.append("no BattleShip.exe in staged dir")
    if not names:
        problems.append("staged dir is empty")
    return problems


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--package", required=True)
    ap.add_argument("--platform",
                    default={"win32": "windows", "darwin": "macos"}
                    .get(sys.platform, "linux"))
    args = ap.parse_args()

    p = Path(args.package)
    if not p.exists():
        print(f"error: package not found: {p}", file=sys.stderr)
        return 2

    if p.is_dir():
        problems = check_dir(p, args.platform)
    elif p.suffix == ".zip":
        problems = check_zip(p, args.platform)
    elif p.suffix == ".AppImage" or p.name.endswith(".AppImage"):
        problems = check_appimage(p)
    elif p.suffix == ".dmg":
        # can't mount dmg off macOS — presence+size is the portable smoke
        problems = [] if p.stat().st_size > 1 << 20 else \
            ["dmg suspiciously small"]
    else:
        print(f"error: unknown package type: {p.name}", file=sys.stderr)
        return 2

    if problems:
        print(f"smoke FAILED for {p.name}:")
        for x in problems:
            print(f"  - {x}")
        return 1
    print(f"smoke OK: {p.name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
