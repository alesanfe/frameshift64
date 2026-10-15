#!/usr/bin/env python3
"""Mutation suite (testplan s.21): each mutant removes exactly one defense,
then a probe stream hits the hole. Every mutated run must FAIL — a mutant
that still passes means the check wasn't load-bearing.

  skip_rng_restore   - SyncTest: snapshot load skips RNG state -> resim
                       must diverge
  accept_bad_session - localtest: wrong-session packets (valid checksum)
                       probed into the drain -> must desync
  accept_bad_checksum- localtest: checksum-broken packets probed -> desync
  no_forward_guard   - localtest: FORWARD datagrams off the host link
                       probed -> must desync (relay topology check is
                       load-bearing)

Replaces scripts/testing/netplay_mutation_suite.ps1 (kept as a wrapper).
Exit: 0 every mutant detected, 1 at least one undetected, 2 harness error.
"""

import argparse
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "support" / "harness" / "netplay"))
import session_runner as sr  # noqa: E402

LOCALTEST = Path(__file__).resolve().parent / "localtest_test.py"
SYNCTEST_PS1 = sr.ROOT / "scripts" / "development" / "rollback_synctest.ps1"
FAILED = []


def expect_fail(name: str, code: int) -> None:
    if code == 1:
        print(f"PASS mutant={name} (detected: exit 1)")
    elif code == 0:
        print(f"FAIL mutant={name} went UNDETECTED (exit 0)")
        FAILED.append(name)
    else:
        print(f"FAIL mutant={name} harness error (exit {code})")
        FAILED.append(name)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", type=Path, default=sr.DEFAULT_BUILD_DIR)
    ap.add_argument("--out-dir", type=Path, default=sr.ROOT / "out" / "logs" / "netplay-test")
    ap.add_argument("--minutes", type=int, default=1)
    ap.add_argument("--seconds", type=int, default=45)
    a = ap.parse_args()

    build = str(a.build_dir.resolve())
    out = str(a.out_dir.resolve())

    def synctest(*extra):
        return sr.run_powershell(
            SYNCTEST_PS1, "-BuildDir", build, "-Minutes", str(a.minutes),
            "-Distance", "1", *extra)

    def localtest(*extra):
        return subprocess.call([
            sys.executable, str(LOCALTEST),
            "--build-dir", build, "--out-dir", out,
            "--seconds", str(a.seconds), *extra])

    # Control runs first: the same harness without a mutant must pass.
    if synctest() != 0:
        print("baseline synctest broken")
        return 2
    if localtest() != 0:
        print("baseline localtest broken")
        return 2

    expect_fail("skip_rng_restore", synctest("-Mutant", "skip_rng_restore"))
    expect_fail("accept_bad_session",
                localtest("--mutant", "accept_bad_session"))
    expect_fail("accept_bad_checksum",
                localtest("--mutant", "accept_bad_checksum"))
    expect_fail("no_forward_guard", localtest("--mutant", "no_forward_guard"))

    if FAILED:
        print(f"MUTATION SUITE FAIL - undetected: {', '.join(FAILED)}")
        return 1
    print("MUTATION SUITE PASS - all mutants detected")
    return 0


if __name__ == "__main__":
    sys.exit(main())
