#!/usr/bin/env python3
"""check_switch_sdk — fail informatively when the Switch toolchain is absent.

The Switch port is gated (SW-08): platform code lives under platforms/switch/
and src/platform/switch/, but the build needs the private toolchain
(devkitPro devkitA64 + libnx or the vendor SDK, depending on the tree's
switch port lineage — see docs/development/switch.md).

Checks:
  * platforms/switch/ exists (the port wrapper is committed)
  * DEVKITPRO env var or a devenv toolchain file referenced by presets
  * the `switch` CMake preset exists

Exit 0 = toolchain usable, 1 = informative failure.
"""
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def main() -> int:
    problems = []
    if not (ROOT / "platforms/switch").is_dir():
        problems.append("platforms/switch/ missing from the tree")

    devkitpro = os.environ.get("DEVKITPRO", "")
    has_dkp = bool(devkitpro) and (Path(devkitpro) / "devkitA64").exists()
    if not has_dkp:
        problems.append(
            "devkitPro devkitA64 not found (DEVKITPRO env unset or no "
            "devkitA64 under it)")

    has_preset = False
    presets = ROOT / "CMakePresets.json"
    if presets.exists():
        has_preset = '"switch"' in presets.read_text(encoding="utf-8")
    if not has_preset:
        problems.append("no 'switch' configure preset in CMakePresets.json "
                        "(SW-08 gate still closed)")

    if problems:
        print("Switch SDK unavailable:")
        for p in problems:
            print(f"  - {p}")
        print("  This target requires the private Switch environment "
              "documented in docs/platforms/switch_port.md (SW-08).")
        return 1
    print("switch SDK: devkitA64 + preset present")
    return 0


if __name__ == "__main__":
    sys.exit(main())
