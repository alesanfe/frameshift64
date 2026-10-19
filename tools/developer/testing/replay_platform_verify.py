#!/usr/bin/env python3
"""Cross-platform determinism verifier: play back a recorded .ssr input
replay on this platform's BattleShip binary and prove the simulation is
bit-identical to the reference platform.

Two independent checks:
  1. In-game verify — SSB64_SYNC_VERIFY replays the golden tick-hash trace
     alongside the run; any divergent gated column fails the verdict, and
     SSB64_RIG_EXIT turns that verdict into the process exit code.
  2. Byte-exact trace diff — the platform's own SSB64_SYNC_TRACE is diffed
     against the golden trace line-by-line, so even ungated columns are
     covered.

Usage:
    python tools/developer/testing/replay_platform_verify.py \
        --binary out/build/Release/BattleShip \
        --replay oracle/replay.ssr --oracle-trace oracle/host.sync.trace \
        --work-dir out/xplat/linux

Exit: 0 identical / 1 divergence or playback failure / 2 setup failure.
"""

import argparse
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests" / "support" / "harness" / "netplay"))
import session_runner as sr


def first_divergence(a, b):
    for i, (x, y) in enumerate(zip(a, b, strict=False)):
        if x != y:
            return i
    return len(a) if len(a) != len(b) else None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", type=Path, required=True)
    ap.add_argument("--replay", type=Path, required=True)
    ap.add_argument("--oracle-trace", type=Path, required=True)
    ap.add_argument("--work-dir", type=Path,
                    default=ROOT / "out" / "test-results" / "xplat")
    ap.add_argument("--timeout", type=int, default=300)
    a = ap.parse_args()

    binary = a.binary.resolve()
    replay = a.replay.resolve()
    if replay.is_dir():
        cands = sorted(replay.glob("*.ssr")) + sorted(replay.glob("*.ssbreplay"))
        if not cands:
            print(f"SETUP FAIL: no .ssr under {replay}")
            return 2
        replay = max(cands, key=lambda p: p.stat().st_mtime)
    oracle = a.oracle_trace.resolve()
    for p, what in ((binary, "binary"), (replay, "replay"), (oracle, "oracle trace")):
        if not p.exists():
            print(f"SETUP FAIL: {what} not found: {p}")
            return 2

    work = sr.clean_out(a.work_dir.resolve())
    trace = work / "platform.sync.trace"
    log = work / "platform.log"

    env = {
        "SSB64_REPLAY_PLAY": str(replay),
        "SSB64_SYNC_VERIFY": str(oracle),
        "SSB64_SYNC_TRACE": str(trace),
        "SSB64_RIG_EXIT": "1",
        "SSB64_HEADLESS": "1",
        "SSB64_MUTE": "1",
        "SSB64_NO_UPDATE_CHECK": "1",
        "SSB64_WIZARD_AUTOCANCEL": "1",
        "SSB64_LOG_PATH": str(log),
        "SSB64_SAVE_PATH": str(work / "verify.sav"),
    }
    print(f"playing {replay.name} on {binary.name}")

    proc = sr.spawn_game(binary.parent, "xplat", env, work)
    deadline = time.time() + a.timeout
    rc = None
    while time.time() < deadline:
        rc = proc.poll()
        if rc is not None:
            break
        time.sleep(1)
    if rc is None:
        sr.stop_all([proc], grace=2)
        print(f"FAIL: no verdict within {a.timeout}s")
        for l in sr.tail_log(log, r".", last=15):
            print(l)
        return 1

    for l in sr.tail_log(log, r"Replay|SyncTrace|verify|FAIL", last=15):
        print(l)

    if rc != 0:
        print(f"FAIL: playback exited with code {rc}")
        return 1
    if not trace.exists():
        print("FAIL: platform trace was not written")
        return 1

    gold = oracle.read_text().splitlines()
    plat = trace.read_text().splitlines()
    div = first_divergence(gold, plat)
    if div is None:
        print(f"PASS: {len(plat)} ticks byte-identical to oracle trace")
        return 0
    g = gold[div] if div < len(gold) else "<missing>"
    p = plat[div] if div < len(plat) else "<missing>"
    print(f"FAIL: trace divergence at line {div}\n  oracle:   {g}\n  platform: {p}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
