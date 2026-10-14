"""Shared netplay test harness — the process/env primitives every e2e
driver under tests/e2e/netplay/ uses. Pure stdlib so the drivers run
identically on Windows, Linux and macOS.

Exit-code contract (kept from the PowerShell drivers these replace):
    0 = expected outcome (in sync / check passed)
    1 = desync or an assertion failed
    2 = no match started / missing trace / harness error
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
LOBBY_HEALTH = "http://127.0.0.1:{port}/health"
LOBBY_NODE = ROOT / "services" / "netplay-lobby" / "src" / "infrastructure" / "local" / "node-lobby.mjs"
LOBBY_SVC = ROOT / "services" / "netplay-lobby"
DEFAULT_BUILD_DIR = ROOT / "out" / "build" / "windows-release" / "Release"
GAME_EXE = "BattleShip.exe" if sys.platform == "win32" else "BattleShip"


def lobby_healthy(port: int = 8799, timeout: float = 3.0) -> bool:
    try:
        return urllib.request.urlopen(
            LOBBY_HEALTH.format(port=port), timeout=timeout).status == 200
    except Exception:
        return False


def ensure_lobby(out_dir: Path, port: int = 8799) -> "subprocess.Popen | None":
    """Guarantee something answers on ws://127.0.0.1:<port>. Returns the
    spawned server process (kill it at teardown) or None if one was already
    up. Prefers the Node adapter — `wrangler dev`'s local ws proxy drops
    binary relay frames and cannot host these tests."""
    if lobby_healthy(port):
        return None
    srv_log = open(out_dir / "server.log", "w", encoding="utf-8")
    srv_err = open(out_dir / "server.err", "w", encoding="utf-8")
    if (LOBBY_SVC / "node_modules" / "ws").exists():
        proc = subprocess.Popen(
            ["node", str(LOBBY_NODE), str(port)],
            cwd=ROOT, stdout=srv_log, stderr=srv_err)
    else:
        proc = subprocess.Popen(
            ["npx", "wrangler", "dev", "--port", str(port), "--ip", "127.0.0.1"],
            cwd=LOBBY_SVC, stdout=srv_log, stderr=srv_err,
            shell=sys.platform == "win32")
    for _ in range(30):
        if lobby_healthy(port, timeout=2):
            return proc
        time.sleep(1)
    kill_tree(proc)
    return None


def spawn_game(build_dir: Path, name: str, env: dict, out_dir: Path,
               exe_dir: "Path | None" = None) -> subprocess.Popen:
    """Start one BattleShip instance. `env` entries are layered over the
    caller's environment; empty-string values are preserved exactly (some
    hooks check presence, not content — mirrors the ps1 drivers)."""
    run_dir = Path(exe_dir) if exe_dir else build_dir
    full_env = os.environ.copy()
    for k, v in env.items():
        full_env[k] = str(v)
    return subprocess.Popen(
        [str(run_dir / GAME_EXE)], cwd=run_dir, env=full_env,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def stop_all(procs, grace: float = 3.0) -> None:
    for p in procs:
        if p.poll() is None:
            p.terminate()
    time.sleep(grace)
    for p in procs:
        if p.poll() is None:
            p.kill()


def kill_tree(proc: "subprocess.Popen | None") -> None:
    if proc is None or proc.poll() is not None:
        return
    if sys.platform == "win32":
        subprocess.run(["taskkill", "/PID", str(proc.pid), "/T", "/F"],
                       capture_output=True)
    else:
        try:
            import signal
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            proc.kill()


def clean_out(out_dir: Path, patterns=("*",)) -> Path:
    out_dir.mkdir(parents=True, exist_ok=True)
    for pat in patterns:
        for f in out_dir.glob(pat):
            if f.is_file():
                f.unlink()
    return out_dir


def tail_log(path: Path, pattern: str, last: int = 8) -> list:
    rx = re.compile(pattern)
    try:
        hits = [l.rstrip("\n") for l in path.read_text(
            encoding="utf-8", errors="replace").splitlines() if rx.search(l)]
    except OSError:
        return []
    return hits[-last:]


def log_matches(path: Path, pattern: str) -> bool:
    rx = re.compile(pattern)
    try:
        return any(rx.search(l) for l in path.read_text(
            encoding="utf-8", errors="replace").splitlines())
    except OSError:
        return False


def read_trace(out_dir: Path, name: str) -> "list | None":
    t = out_dir / f"{name}.trace"
    if not t.exists():
        return None
    return t.read_text(encoding="utf-8", errors="replace").splitlines()


def compare_traces(names, out_dir: Path, ref: str = "host",
                   min_fields: int = 0) -> "tuple[int, str]":
    """Pairwise-compare every peer's confirmed-state trace against `ref`.
    min_fields>0 skips half-written teardown lines (localtest's 13-field
    guard). Returns (exit_code, message)."""
    traces = {n: read_trace(out_dir, n) for n in names}
    for n, t in traces.items():
        if t is None:
            return 2, f"no trace for {n} (match did not start)"
    counts = " ".join(f"{n}={len(traces[n])}" for n in names)
    print(f"traced ticks: {counts}")
    reference = traces[ref]
    worst = 0
    for n in names:
        if n == ref:
            continue
        other = traces[n]
        for i, (a, b) in enumerate(zip(reference, other)):
            if min_fields and (len(a.split()) < min_fields
                               or len(b.split()) < min_fields):
                print(f"skipping truncated trace line {i}")
                continue
            if a != b:
                return 1, (f"DESYNC {ref} vs {n} at line {i}\n"
                           f"  {ref}: {a}\n  {n}: {b}")
        worst = max(worst, min(len(reference), len(other)))
    if worst == 0:
        return 2, "no ticks traced"
    return 0, f"IN SYNC across {len(names)} peers for {worst} ticks"


def log_size_guard(path: Path, max_bytes: int, label: str) -> "str | None":
    try:
        if path.exists() and path.stat().st_size > max_bytes:
            return f"{label}: {path.name} over {max_bytes} bytes (leak?)"
    except OSError:
        pass
    return None


def run(cmd, cwd=None) -> int:
    """Run a subprocess inheriting stdio; return its exit code."""
    return subprocess.call(cmd, cwd=cwd)


def run_powershell(script: Path, *args) -> int:
    """Invoke a remaining .ps1 driver (e.g. rollback_synctest.ps1) from a
    Python driver — keeps one implementation until that script is ported."""
    shell = "powershell" if sys.platform == "win32" else "pwsh"
    return subprocess.call([shell, "-NoProfile", "-File", str(script), *args])
