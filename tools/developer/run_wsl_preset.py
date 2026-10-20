#!/usr/bin/env python3
"""run_wsl_preset — run a make target inside WSL (for linux-only presets).

TSan and some sanitizer flows only work on Linux toolchains. On Windows
this script delegates to `wsl make <target>` inside the repo; on a native
Linux host it execs make directly. When WSL is absent it fails
informatively — a skip that reads as a failure, not silent success.

Usage: run_wsl_preset.py --preset linux-tsan --targets "configure build"
       [--label tsan]   (optional ctest label)

`--targets` is a space-separated list of LEAF targets (configure, build,
ctest — never an aggregator, or the delegation recurses).
"""
import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--preset", required=True)
    ap.add_argument("--targets", required=True)
    ap.add_argument("--label")
    args = ap.parse_args()

    targets = args.targets
    if sys.platform.startswith("linux"):
        cmd = ["make", *targets.split(), f"PRESET={args.preset}"]
        if args.label:
            cmd.append(f"TEST_LABEL={args.label}")
        return subprocess.run(cmd, cwd=ROOT).returncode

    wsl = shutil.which("wsl") or shutil.which("wsl.exe")
    if not wsl:
        print("error: this preset needs a Linux toolchain", file=sys.stderr)
        print("  options: run inside WSL (`wsl make %s`), a Linux runner, "
              "or a container", file=sys.stderr)
        return 2

    win_root = str(ROOT)
    wsl_root = subprocess.run(
        [wsl, "wslpath", "-a", win_root.replace("\\", "/")],
        capture_output=True, text=True).stdout.strip()
    if not wsl_root:
        print("error: could not translate repo path into WSL", file=sys.stderr)
        return 2
    inner = f"cd '{wsl_root}' && make {targets} PRESET={args.preset}"
    if args.label:
        inner += f" TEST_LABEL={args.label}"
    return subprocess.run([wsl, "bash", "-lc", inner]).returncode


if __name__ == "__main__":
    sys.exit(main())
