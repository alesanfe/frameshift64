#!/usr/bin/env python3
"""Android cross-platform determinism verifier — drives the same
replay+verify rig as replay_platform_verify.py, but on a device/emulator
over adb.

Android apps can't receive environment variables, so the port reads
`ssb64_test_env.txt` (one KEY=VALUE per line) from the app's
externalFilesDir at process start (SSB64_TEST_HOOKS builds only) and
inject them with setenv(). This script:

  1. adb install -r <apk>
  2. pushes BattleShip.o2r, the .ssr replay and the golden sync trace into
     /sdcard/Android/data/<pkg>/files/
  3. writes ssb64_test_env.txt pointing at those device paths
  4. launches BootActivity and waits for the process to exit
     (SSB64_RIG_EXIT) or the timeout
  5. pulls the platform trace + log, greps the verdict, and byte-diffs the
     trace against the golden one

Usage:
    python tools/developer/testing/android_replay_verify.py \
        --apk app-debug.apk --o2r BattleShip.o2r \
        --replay oracle/replay.ssr --oracle-trace oracle/host.sync.trace \
        --work-dir out/xplat/android

Exit: 0 identical / 1 divergence or playback failure / 2 setup failure.
"""

import argparse
import subprocess
import sys
import tempfile
import time
from pathlib import Path

PKG = "com.jrickey.battleship.debug"
# applicationIdSuffix adds .debug to the package, but the Java class still
# lives under com.jrickey.battleship — spell the component out fully.
ACTIVITY = f"{PKG}/com.jrickey.battleship.BootActivity"
FILES_DIR = f"/sdcard/Android/data/{PKG}/files"

ROOT = Path(__file__).resolve().parents[3]


def adb(serial, *args, timeout=60, check=True):
    cmd = ["adb"]
    if serial:
        cmd += ["-s", serial]
    cmd += list(args)
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, check=False)
    if check and r.returncode != 0:
        raise RuntimeError(f"{' '.join(cmd)} failed: {r.stderr.strip() or r.stdout.strip()}")
    return r


def first_divergence(a, b):
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            return i
    return len(a) if len(a) != len(b) else None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--apk", type=Path, required=True)
    ap.add_argument("--o2r", type=Path, required=True,
                    help="BattleShip.o2r extracted on the host (platform-independent)")
    ap.add_argument("--replay", type=Path, required=True)
    ap.add_argument("--oracle-trace", type=Path, required=True)
    ap.add_argument("--serial", default="")
    ap.add_argument("--timeout", type=int, default=600,
                    help="max seconds to wait for the game process to exit")
    ap.add_argument("--work-dir", type=Path,
                    default=ROOT / "out" / "test-results" / "xplat-android")
    a = ap.parse_args()

    replay = a.replay
    if replay.is_dir():
        cands = sorted(replay.glob("*.ssr")) + sorted(replay.glob("*.ssbreplay"))
        if not cands:
            print(f"SETUP FAIL: no .ssr under {replay}")
            return 2
        replay = max(cands, key=lambda p: p.stat().st_mtime)

    for p, what in ((a.apk, "apk"), (a.o2r, "o2r"),
                    (replay, "replay"), (a.oracle_trace, "oracle trace")):
        if not p.exists():
            print(f"SETUP FAIL: {what} not found: {p}")
            return 2

    work = a.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=True)

    dev_trace = f"{FILES_DIR}/xplat.sync.trace"
    dev_log = f"{FILES_DIR}/xplat.log"
    dev_replay = f"{FILES_DIR}/xplat.ssr"
    dev_oracle = f"{FILES_DIR}/xplat.oracle.trace"

    env_lines = [
        f"SSB64_REPLAY_PLAY={dev_replay}",
        f"SSB64_SYNC_VERIFY={dev_oracle}",
        f"SSB64_SYNC_TRACE={dev_trace}",
        "SSB64_RIG_EXIT=1",
        "SSB64_HEADLESS=1",
        "SSB64_MUTE=1",
        "SSB64_NO_UPDATE_CHECK=1",
        "SSB64_WIZARD_AUTOCANCEL=1",
        f"SSB64_LOG_PATH={dev_log}",
        f"SSB64_SAVE_PATH={FILES_DIR}/xplat.sav",
    ]

    print("installing apk…")
    # Emulator system images are userdebug — `adb root` lifts the scoped-
    # storage restriction on /sdcard/Android/data. Harmless when it fails
    # (physical devices): the pushes below then rely on the shell user
    # still having access, which holds on most emulators.
    adb(a.serial, "root", check=False)
    adb(a.serial, "wait-for-device")
    adb(a.serial, "install", "-r", str(a.apk), timeout=300)

    print("pushing assets + test vector…")
    adb(a.serial, "shell", "mkdir", "-p", FILES_DIR)
    adb(a.serial, "push", str(a.o2r), f"{FILES_DIR}/BattleShip.o2r", timeout=300)
    adb(a.serial, "push", str(replay), dev_replay)
    adb(a.serial, "push", str(a.oracle_trace), dev_oracle)
    adb(a.serial, "shell", "rm", "-f", dev_trace, dev_log)

    with tempfile.NamedTemporaryFile(
            "w", suffix=".txt", delete=False, newline="\n") as f:
        f.write("\n".join(env_lines) + "\n")
        env_local = f.name
    adb(a.serial, "push", env_local, f"{FILES_DIR}/ssb64_test_env.txt")

    print("force-stop + launch…")
    adb(a.serial, "shell", "am", "force-stop", PKG, check=False)
    adb(a.serial, "shell", "am", "start", "-n", ACTIVITY)

    print(f"waiting for verdict (timeout {a.timeout}s)…")
    deadline = time.time() + a.timeout
    # First wait for the process to APPEAR — pidof racing `am start` would
    # read "no process" as "exited" and produce a false failure.
    started = False
    while time.time() < deadline:
        r = adb(a.serial, "shell", "pidof", PKG, check=False)
        if r.returncode == 0 and r.stdout.strip():
            started = True
            break
        time.sleep(2)
    if not started:
        print("FAIL: game process never started")
        return 1
    exited = False
    while time.time() < deadline:
        r = adb(a.serial, "shell", "pidof", PKG, check=False)
        if r.returncode != 0 or not r.stdout.strip():
            exited = True
            break
        time.sleep(5)
    if not exited:
        adb(a.serial, "shell", "am", "force-stop", PKG, check=False)
        print("FAIL: game did not exit (no verdict)")
        return 1

    adb(a.serial, "pull", dev_log, str(work / "platform.log"), check=False)
    adb(a.serial, "pull", dev_trace, str(work / "platform.sync.trace"), check=False)

    log = work / "platform.log"
    if log.exists():
        lines = [l for l in log.read_text(errors="replace").splitlines()
                 if any(k in l for k in ("Replay", "SyncTrace", "verify", "FAIL"))]
        for l in lines[-15:]:
            print(l)

    plat = work / "platform.sync.trace"
    if not plat.exists() or not plat.read_text(errors="replace").strip():
        print("FAIL: platform trace missing or empty — playback never ran")
        return 1

    gold = a.oracle_trace.read_text().splitlines()
    mine = plat.read_text().splitlines()
    div = first_divergence(gold, mine)
    if div is None:
        print(f"PASS: {len(mine)} ticks byte-identical to oracle trace")
        return 0
    g = gold[div] if div < len(gold) else "<missing>"
    p = mine[div] if div < len(mine) else "<missing>"
    print(f"FAIL: trace divergence at line {div}\n  oracle:  {g}\n  android: {p}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
