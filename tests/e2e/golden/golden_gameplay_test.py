#!/usr/bin/env python3
"""Golden gameplay verification — replay fixed .ssr fixtures and compare the
per-tick state trace against committed goldens (tests/support/golden/).

Requires extracted assets (BattleShip.o2r + f3d.o2r next to the binary) and a
test-hooks build (SSB64_TEST_HOOKS=ON — the default for test builds). Without
them the game cannot boot a match, so the test reports SKIP (exit 77) rather
than FAIL — CI gates this test on the BASEROM_US_Z64 secret.

Modes:
    verify   (default) — SSB64_REPLAY_PLAY=<ssr> SSB64_SYNC_VERIFY=<trace>
             SSB64_RIG_EXIT=1; PASS iff the process exits 0 and the log shows
             "playback verify ... result=PASS".
    capture  (--capture) — same but SSB64_SYNC_TRACE writes the golden trace;
             used once per build flavor to (re)generate tests/support/golden.

Game exit codes (netreplay.c): 0 verified / 1 input checksum FAIL /
2 match ended early / 3 trace oracle failed to load / 4 DESYNC.

Usage:
    python tests/e2e/golden/golden_gameplay_test.py --build-dir out/build/verify/Debug
    python tests/e2e/golden/golden_gameplay_test.py --build-dir build/Release --capture
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests" / "support" / "harness" / "netplay"))
import session_runner as sr

SKIP = 77
SUPPORT = ROOT / "tests" / "support"
MANIFEST = SUPPORT / "golden" / "manifest.json"
GAME_EXE = "BattleShip.exe" if sys.platform == "win32" else "BattleShip"
REQUIRED_ARCHIVES = ("BattleShip.o2r", "f3d.o2r")


def load_manifest(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def gate(build_dir: Path) -> "str | None":
    """None = runnable; otherwise the reason to skip."""
    exe = build_dir / GAME_EXE
    if not exe.exists():
        return f"binary not found: {exe}"
    missing = [a for a in REQUIRED_ARCHIVES if not (build_dir / a).exists()]
    if missing:
        return (f"missing archives next to {GAME_EXE}: {', '.join(missing)} "
                f"(run the ExtractAssets target with baserom.us.z64 present)")
    return None


# Files/dirs the game opens relative to its cwd — copied into the run
# snapshot so cases are immune to a concurrent build relinking the tree.
SNAPSHOT_DIRS = ("lang", "assets", "bundled-data", "bundled-mods", "config")


def snapshot_binary(build_dir: Path, snap: Path) -> Path:
    """Copy the exe + runtime deps into a private dir.

    A golden run takes minutes and a concurrent ninja build relinking
    BattleShip.exe/*.o2r mid-suite can tear a running process's image
    (seen as silent exit=1 and fast-fail 0xC0000409 crashes that vanish
    under reproduction). Running cases from a stable snapshot removes
    that class of flake entirely.
    """
    if snap.exists():
        shutil.rmtree(snap)
    snap.mkdir(parents=True)
    sources = [build_dir / GAME_EXE, *sorted(build_dir.glob("*.dll")),
               *(build_dir / a for a in REQUIRED_ARCHIVES)]
    for src in sources:
        for attempt in range(5):
            try:
                shutil.copy2(src, snap / src.name)
                break
            except OSError:
                if attempt == 4:
                    raise
                time.sleep(0.5)
    for d in SNAPSHOT_DIRS:
        src = build_dir / d
        if src.is_dir():
            shutil.copytree(src, snap / d)
    return snap


def run_case(build_dir: Path, name: str, ssr: Path, out_dir: Path,
             verify_trace: "Path | None", capture_trace: "Path | None",
             timeout: int) -> "tuple[int, str]":
    """One replay playback. Returns (exit_code, log_path)."""
    log = out_dir / f"{name}.log"
    env = {
        "SSB64_REPLAY_PLAY": str(ssr),
        "SSB64_RIG_EXIT": "1",
        "SSB64_HEADLESS": "1",
        "SSB64_MUTE": "1",
        "SSB64_NO_UPDATE_CHECK": "1",
        "SSB64_LOG_PATH": str(log),
        "SSB64_SAVE_PATH": str(out_dir / f"{name}.sav"),
    }
    if verify_trace is not None:
        env["SSB64_SYNC_VERIFY"] = str(verify_trace)
    if capture_trace is not None:
        env["SSB64_SYNC_TRACE"] = str(capture_trace)

    proc = sr.spawn_game(build_dir, name, env, out_dir)
    try:
        return proc.wait(timeout=timeout), str(log)
    except subprocess.TimeoutExpired:
        sr.kill_tree(proc)
        return -1, str(log)


def trace_ticks(path: Path) -> int:
    try:
        return sum(1 for ln in path.read_text(
            encoding="utf-8", errors="replace").splitlines()
            if ln and not ln.startswith("#"))
    except OSError:
        return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--build-dir", type=Path, required=True,
                    help="directory containing BattleShip.exe + .o2r files")
    ap.add_argument("--fixtures", type=Path, default=SUPPORT,
                    help="tests/support root (manifest, replays, goldens)")
    ap.add_argument("--case", default="all", help="case name or 'all'")
    ap.add_argument("--capture", action="store_true",
                    help="regenerate golden traces instead of verifying")
    ap.add_argument("--output", type=Path,
                    default=ROOT / "out" / "test-results" / "golden")
    ap.add_argument("--timeout", type=int, default=180)
    ap.add_argument("--repeat", type=int, default=1,
                    help="run each case N times (flake hunting)")
    a = ap.parse_args()

    src_dir = a.build_dir.resolve()
    reason = gate(src_dir)
    if reason is not None:
        print(f"SKIP golden gameplay: {reason}")
        return SKIP

    manifest_path = a.fixtures / "golden" / "manifest.json"
    cases = load_manifest(manifest_path)["cases"]
    if a.case != "all":
        cases = [c for c in cases if c["name"] == a.case]
        if not cases:
            print(f"unknown case {a.case!r} — manifest: {manifest_path}")
            return 2

    out = sr.clean_out(a.output.resolve())
    build_dir = snapshot_binary(src_dir, out / "bin")
    mode = "capture" if a.capture else "verify"
    print(f"golden gameplay [{mode}]: {len(cases)} case(s), "
          f"binary={build_dir / GAME_EXE} (snapshot of {src_dir})")

    failures = []
    for case in cases:
        name = case["name"]
        ssr = a.fixtures / case["replay"]
        golden = a.fixtures / case["trace"]
        if not ssr.exists():
            print(f"{name}: FAIL replay fixture missing: {ssr}")
            print("  regenerate: python tools/developer/testing/"
                  f"make_replay.py {name} -o {ssr}")
            failures.append(name)
            continue

        if a.capture:
            rc, log = run_case(build_dir, name, ssr, out,
                               verify_trace=None, capture_trace=golden,
                               timeout=a.timeout)
            ticks = trace_ticks(golden)
            if rc in (0, 3) and ticks >= case["frames"]:
                print(f"{name}: captured {ticks} ticks -> {golden} "
                      f"(exit {rc})")
            else:
                print(f"{name}: FAIL capture rc={rc} ticks={ticks}/"
                      f"{case['frames']} — {log}")
                print("\n".join("  " + l for l in
                      sr.tail_log(Path(log), r".")))
                failures.append(name)
            continue

        if not golden.exists():
            print(f"{name}: FAIL golden trace missing: {golden}")
            print(f"  capture it once on a known-good build: "
                  f"{sys.argv[0]} --build-dir {build_dir} --capture "
                  f"--case {name}")
            failures.append(name)
            continue

        failed = False
        for run in range(1, a.repeat + 1):
            run_name = name if a.repeat == 1 else f"{name}-r{run}"
            rc, log = run_case(build_dir, run_name, ssr, out,
                               verify_trace=golden, capture_trace=None,
                               timeout=a.timeout)
            if rc == 0:
                if a.repeat > 1:
                    print(f"{name}: run {run}/{a.repeat} PASS")
                else:
                    print(f"{name}: PASS ({case['frames']} ticks vs {golden.name})")
                continue

            verdict = {-1: "TIMEOUT", 1: "INPUT-FAIL", 2: "INCOMPLETE",
                       3: "TRACE-LOADFAIL", 4: "DESYNC"}.get(rc, f"rc={rc}")
            print(f"{name}: run {run}/{a.repeat} FAIL {verdict} — {log}")
            print("\n".join("  " + l for l in
                  sr.tail_log(Path(log),
                              r"Replay|SyncTrace|DIVERGENCE|result=")))
            failed = True
            break
        if failed:
            failures.append(name)

    report = {"mode": mode, "build_dir": str(build_dir),
              "cases": len(cases), "repeat": a.repeat, "failures": failures}
    (out / "golden_report.json").write_text(json.dumps(report, indent=2))
    if failures:
        print(f"FAIL: {len(failures)}/{len(cases)} golden case(s): "
              + ", ".join(failures))
        return 1
    print(f"PASS: {len(cases)} golden case(s) {mode}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
    sys.exit(main())
