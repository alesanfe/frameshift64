#!/usr/bin/env python3
"""Declarative test-suite runner.

Reads tests/support/config/test-suites.yml, maps a suite name to a pytest
marker expression, and invokes pytest with it. This is the single entry
point CI and developers share — the .ps1 wrappers under
scripts/testing/ delegate to the same e2e drivers pytest calls.

    python tools/developer/testing/run_tests.py --suite quick
    python tools/developer/testing/run_tests.py --suite netplay --repeat 3
    python tools/developer/testing/run_tests.py --suite pull-request --build-dir out/build/verify/Debug
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SUITES_YML = ROOT / "tests" / "support" / "config" / "test-suites.yml"


def load_suites() -> dict:
    import yaml
    data = yaml.safe_load(SUITES_YML.read_text(encoding="utf-8"))
    return data["suites"]


def marker_expr(suite: dict) -> str:
    """Translate {include, exclude} name lists into a pytest -m expr."""
    include = " or ".join(suite.get("include", [])) or "*"
    exclude = " or ".join(suite.get("exclude", []))
    expr = include
    if exclude:
        expr = f"({include}) and not ({exclude})"
    return expr


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--suite", default="quick",
                    help="suite name from tests/support/config/test-suites.yml")
    ap.add_argument("--list", action="store_true", help="list suites and exit")
    ap.add_argument("--build-dir", type=Path, default=None)
    ap.add_argument("--repeat", type=int, default=1,
                    help="pytest-repeat count (requires pytest-repeat)")
    ap.add_argument("--workers", default=None,
                    help="pytest-xdist worker count, e.g. auto")
    ap.add_argument("--extra", nargs=argparse.REMAINDER, default=[],
                    help="extra args passed verbatim to pytest")
    args = ap.parse_args()

    suites = load_suites()
    if args.list:
        for name, suite in suites.items():
            print(f"{name:15} -> -m '{marker_expr(suite)}'")
        return 0

    if args.suite not in suites:
        print(f"unknown suite '{args.suite}' — available: "
              f"{', '.join(sorted(suites))}", file=sys.stderr)
        return 2

    cmd = [sys.executable, "-m", "pytest", "-m",
           marker_expr(suites[args.suite])]
    if args.build_dir:
        cmd += ["--build-dir", str(args.build_dir)]
    if args.repeat > 1:
        cmd += ["--count", str(args.repeat)]
    if args.workers:
        cmd += ["-n", args.workers]
    cmd += args.extra
    print("+", " ".join(cmd))
    return subprocess.run(cmd, cwd=ROOT, check=False).returncode


if __name__ == "__main__":
    sys.exit(main())
