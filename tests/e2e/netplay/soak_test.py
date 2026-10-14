#!/usr/bin/env python3
"""Soak / rematch driver (testplan s.14). Two halves:

  --rematch-rounds N   - N back-to-back localtest sessions. Each round
                         spawns fresh processes, reconnects, plays, tears
                         down — the rematch cycle approximated at process
                         granularity.
  --synctest-minutes M - one long SyncTest run (0 = skip) for snapshot/hash
                         integrity over time.

Failure = any round desyncs, errors, hangs, crashes, or a per-round log
grows unboundedly (>200 MB leak signature).

Replaces scripts/testing/netplay_soak.ps1 (kept as a wrapper).
Exit: 0 all rounds clean, 1 a round failed, 2 harness error.
"""

import argparse
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "harness" / "netplay"))
import session_runner as sr  # noqa: E402

MAX_LOG_BYTES = 200 * 1024 * 1024
LOCALTEST = Path(__file__).resolve().parent / "localtest_test.py"
SYNCTEST_PS1 = sr.ROOT / "scripts" / "development" / "rollback_synctest.ps1"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--rematch-rounds", type=int, default=3)
    ap.add_argument("--seconds", type=int, default=45)
    ap.add_argument("--synctest-minutes", type=int, default=0)
    ap.add_argument("--synctest-distance", type=int, default=3)
    ap.add_argument("--build-dir", type=Path, default=sr.DEFAULT_BUILD_DIR)
    ap.add_argument("--out-dir", type=Path, default=sr.ROOT / "out" / "logs" / "netplay-test")
    ap.add_argument("--latency-ms", type=int, default=50)
    ap.add_argument("--jitter-ms", type=int, default=15)
    ap.add_argument("--loss-pct", type=int, default=1)
    ap.add_argument("--dup-pct", type=int, default=1)
    ap.add_argument("--reorder-pct", type=int, default=2)
    a = ap.parse_args()

    out = a.out_dir.resolve()
    failures = []

    for rnd in range(1, a.rematch_rounds + 1):
        print(f"=== rematch round {rnd}/{a.rematch_rounds} ===")
        code = subprocess.call([
            sys.executable, str(LOCALTEST),
            "--build-dir", str(a.build_dir.resolve()),
            "--out-dir", str(out),
            "--seconds", str(a.seconds),
            "--seed", str(4242 + rnd * 101),
            "--base-port", str(7801 + rnd * 10),
            "--latency-ms", str(a.latency_ms),
            "--jitter-ms", str(a.jitter_ms),
            "--loss-pct", str(a.loss_pct),
            "--dup-pct", str(a.dup_pct),
            "--reorder-pct", str(a.reorder_pct)])
        if code != 0:
            failures.append(f"round {rnd} exit={code}")
            continue
        for f in ("host", "client"):
            log = out / f"{f}.log"
            leak = sr.log_size_guard(log, MAX_LOG_BYTES, f"round {rnd}")
            if leak:
                failures.append(leak)
            # First-chance vectored exceptions and the crash-handler boot
            # line are normal noise — only fatal events count.
            if sr.log_matches(log, r"second-chance|unhandled|SIGSEGV|abort\(\)|Fatal"):
                failures.append(f"round {rnd} {f}.log shows crash signature")

    if a.synctest_minutes > 0:
        print(f"=== long synctest {a.synctest_minutes}min distance "
              f"{a.synctest_distance} ===")
        code = sr.run_powershell(
            SYNCTEST_PS1,
            "-BuildDir", str(a.build_dir.resolve()),
            "-Minutes", str(a.synctest_minutes),
            "-Distance", str(a.synctest_distance),
            "-TimeoutSeconds", str(max(400, a.synctest_minutes * 70)))
        if code != 0:
            failures.append(f"synctest exit={code}")

    if failures:
        print("SOAK FAIL:")
        for f in failures:
            print(f"  {f}")
        return 1
    suffix = (f" + {a.synctest_minutes}min synctest"
              if a.synctest_minutes > 0 else "")
    print(f"SOAK PASS - {a.rematch_rounds} rounds{suffix}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
