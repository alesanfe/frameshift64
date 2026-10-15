#!/usr/bin/env python3
"""Merge LLVM coverage from instrumented test runs.

Workflow:
  1. Build with clang instrumentation:
       cmake -B out/build/<preset> -DSSB64_ENABLE_COVERAGE=ON && build
  2. Run the suite with LLVM_PROFILE_FILE set:
       LLVM_PROFILE_FILE=$PWD/out/coverage/native/raw/%p-%m.profraw \
         python tools/developer/testing/run_tests.py --suite pull-request
  3. Merge + report:
       python tools/developer/testing/coverage.py \
         --binary out/build/<preset>/BattleShip \
         --output-dir out/coverage/native

Produces coverage.profdata, an HTML report, and an lcov.info for CI
upload. components/decomp/, components/libultraship/, components/game-runtime/, tests/ and generated files are
excluded from the report — the metric tracks the modern port layer.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
IGNORE = r"(decomp|libultraship|torch|tests|generated|vcpkg_installed)/"


def run(cmd: list[str], stdout=None) -> None:
    print("+", " ".join(cmd))
    subprocess.run(cmd, check=True, stdout=stdout)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", type=Path, required=True,
                    help="instrumented BattleShip binary")
    ap.add_argument("--objects", type=Path, nargs="*", default=[],
                    help="extra instrumented objects (shared libs, test exes)")
    ap.add_argument("--output-dir", type=Path,
                    default=ROOT / "out" / "coverage" / "native")
    args = ap.parse_args()

    for tool in ("llvm-profdata", "llvm-cov"):
        if shutil.which(tool) is None:
            sys.exit(f"{tool} not found in PATH (install LLVM tools)")

    raw_dir = args.output_dir / "raw"
    profdata = args.output_dir / "coverage.profdata"
    html_dir = args.output_dir / "html"
    lcov_file = args.output_dir / "lcov.info"

    profiles = sorted(raw_dir.glob("*.profraw"))
    if not profiles:
        sys.exit(f"No LLVM .profraw profiles in {raw_dir} — run the suite "
                 "with LLVM_PROFILE_FILE pointed there")

    run(["llvm-profdata", "merge", "-sparse",
         *map(str, profiles), "-o", str(profdata)])

    # llvm-cov takes the primary binary positionally and extra
    # instrumented objects via -object= flags.
    obj_flags = [str(args.binary),
                 *(f"-object={o}" for o in args.objects)]

    run(["llvm-cov", "show", *obj_flags,
         f"-instr-profile={profdata}",
         "-format=html",
         f"-output-dir={html_dir}",
         f"-ignore-filename-regex={IGNORE}"])

    with lcov_file.open("w", encoding="utf-8") as out:
        run(["llvm-cov", "export", *obj_flags,
             f"-instr-profile={profdata}",
             "-format=lcov",
             f"-ignore-filename-regex={IGNORE}"], stdout=out)

    # Terminal summary
    run(["llvm-cov", "report", *obj_flags,
         f"-instr-profile={profdata}",
         f"-ignore-filename-regex={IGNORE}"])
    print(f"\nHTML: {html_dir}/index.html\nLCOV: {lcov_file}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
