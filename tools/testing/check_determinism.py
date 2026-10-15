#!/usr/bin/env python3
"""Determinism checker: replay the same .ssr N times and compare the
confirmed-state traces produced by SSB64_NETPLAY_CONFIRMED_TRACE.

Detects the first divergent tick between runs, reports per-run trace
hashes and writes a JSON report to --output/determinism.json.

Usage:
    python tools/testing/check_determinism.py --binary out/build/.../BattleShip.exe \
        --replay tests/support/fixtures/replays/reference.ssr --runs 10 \
        --output out/test-results/determinism

Exit: 0 deterministic / 1 divergence / 2 setup or playback failure.
"""

import argparse
import hashlib
import json
import os
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "support" / "harness" / "netplay"))
import session_runner as sr  # noqa: E402


def first_divergence(a: list, b: list) -> "int | None":
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            return i
    return len(a) if len(a) != len(b) else None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", type=Path, required=True)
    ap.add_argument("--replay", type=Path, required=True,
                    help="recorded .ssr input replay")
    ap.add_argument("--runs", type=int, default=10)
    ap.add_argument("--output", type=Path,
                    default=ROOT / "out" / "test-results" / "determinism")
    ap.add_argument("--timeout", type=int, default=180)
    a = ap.parse_args()

    binary = a.binary.resolve()
    replay = a.replay.resolve()
    if not binary.exists():
        print(f"binary not found: {binary}")
        return 2
    if not replay.exists():
        print(f"replay not found: {replay}")
        print("record one with SSB64_REPLAY_RECORD_FRAMES=<n> during a match")
        return 2

    out = sr.clean_out(a.output.resolve())
    print(f"determinism: {a.runs} runs of {replay.name}")

    runs = []
    for i in range(a.runs):
        name = f"run{i:02d}"
        trace = out / f"{name}.trace"
        log = out / f"{name}.log"
        env = {
            "SSB64_REPLAY_PLAY": str(replay),
            "SSB64_NETPLAY_CONFIRMED_TRACE": str(trace),
            "SSB64_LOG_PATH": str(log),
            "SSB64_SAVE_PATH": str(out / f"{name}.sav"),
            "SSB64_MUTE": "1",
            "SSB64_HEADLESS": "1",
            "SSB64_NO_UPDATE_CHECK": "1",
        }
        proc = sr.spawn_game(binary.parent, name, env, out)
        deadline = time.time() + a.timeout
        while time.time() < deadline and \
                not sr.log_matches(log, r"playback verify.*result="):
            time.sleep(1)
        sr.stop_all([proc], grace=2)
        if not sr.log_matches(log, r"playback verify.*result=PASS"):
            print(f"{name}: playback did not verify — see {log}")
            return 2
        lines = trace.read_text(errors="replace").splitlines() \
            if trace.exists() else []
        digest = hashlib.sha256("\n".join(lines).encode()).hexdigest()[:16]
        runs.append({"run": name, "ticks": len(lines), "sha256": digest,
                     "lines": lines})
        print(f"{name}: {len(lines)} ticks  sha256={digest}")

    report = {"replay": str(replay), "runs": a.runs,
              "results": [{k: r[k] for k in ("run", "ticks", "sha256")}
                          for r in runs],
              "deterministic": True, "divergence": None}

    ref = runs[0]
    for r in runs[1:]:
        idx = first_divergence(ref["lines"], r["lines"])
        if idx is not None:
            report["deterministic"] = False
            report["divergence"] = {
                "between": [ref["run"], r["run"]],
                "first_divergent_line": idx,
                "expected": ref["lines"][idx] if idx < len(ref["lines"]) else "<eof>",
                "actual": r["lines"][idx] if idx < len(r["lines"]) else "<eof>",
            }
            break

    (out / "determinism.json").write_text(json.dumps(report, indent=2))
    if report["deterministic"]:
        print(f"DETERMINISTIC: {a.runs} runs identical ({ref['ticks']} ticks)")
        return 0
    d = report["divergence"]
    print(f"DIVERGENCE at trace line {d['first_divergent_line']} "
          f"({d['between'][0]} vs {d['between'][1]})")
    print(f"  expected: {d['expected']}")
    print(f"  actual:   {d['actual']}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
