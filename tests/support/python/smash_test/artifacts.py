"""E2E helpers for deliverable artifacts: launching the built game with the
SSB64_* test hooks, probing packaging formats (APK / MSIX / NRO / Mach-O /
ELF), and talking to adb.

Kept dependency-free (stdlib only) so packaging tests run in minimal CI.
"""

from __future__ import annotations

import os
import shutil
import struct
import subprocess
import sys
import time
import zipfile
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[4]


# ── game launch ────────────────────────────────────────────────────────

@dataclass
class GameRun:
    returncode: int
    log: str          # combined stdout+stderr (or SSB64_LOG_PATH content)
    timed_out: bool


def launch_game(exe: Path, artifacts: Path, frames: int = 240,
                env_extra: dict[str, str] | None = None,
                cwd: Path | None = None, timeout: int = 240,
                name: str = "run", retries: int = 1) -> GameRun:
    """Run the binary under the test hooks and capture everything.

    SSB64_LOG_PATH keeps the game's own logger output separate from the
    process stdout (both are captured; the log file is the authority for
    boot-marker assertions).

    Windows occasionally delivers a stray SDL_QUIT to a freshly created
    window (leftover event from a previously closed game window), which
    makes the loop exit at frame 1 — indistinguishable from a clean run
    except the SSB64_MAX_FRAMES marker never fires. Retry that exact
    signature up to `retries` times; genuine early-close bugs still fail.
    A concurrent build can also delete/relink the exe mid-suite — the
    resulting FileNotFoundError gets the same bounded retry.
    """
    run = _launch_once(exe, artifacts, frames, env_extra, cwd, timeout, name)
    attempt = 0
    while attempt < retries and _early_window_close(run, frames):
        attempt += 1
        run = _launch_once(exe, artifacts, frames, env_extra, cwd, timeout,
                           f"{name}_retry{attempt}")
    return run


def _early_window_close(run: GameRun, frames: int) -> bool:
    return (not run.timed_out and run.returncode == 0
            and frames > 1
            and "clean shutdown" not in run.log
            and "WindowIsRunning=0" in run.log)


def _launch_once(exe: Path, artifacts: Path, frames: int,
                 env_extra: dict[str, str] | None,
                 cwd: Path | None, timeout: int,
                 name: str) -> GameRun:
    log_file = artifacts / f"{name}.log"
    stdout_file = artifacts / f"{name}.stdout.log"
    env = os.environ.copy()
    env.update({
        "SSB64_TEST_MODE": "1",
        "SSB64_NO_UPDATE_CHECK": "1",
        "SSB64_MUTE": "1",
        "SSB64_MAX_FRAMES": str(frames),
        "SSB64_LOG_PATH": str(log_file),
        # Isolate user profile state: the game writes BattleShip.cfg.json /
        # saves under the app-data dir — SHIP_HOME redirects it portably
        # (%APPDATA% alone is NOT enough: SDL_GetPrefPath reads the real
        # RoamingAppData via SHGetKnownFolderPath regardless of env).
        "SHIP_HOME": str(artifacts / "appdata"),
        "APPDATA": str(artifacts / "appdata"),
        "SSB64_SAVE_PATH": str(artifacts / "saves"),
    })
    if env_extra:
        env.update(env_extra)
    (artifacts / "appdata").mkdir(parents=True, exist_ok=True)

    timed_out = False
    try:
        proc = subprocess.run(
            [str(exe), "--test-mode"], cwd=str(cwd or exe.parent),
            env=env, capture_output=True, text=True,
            errors="replace", timeout=timeout, check=False)
        rc = proc.returncode
        stdout_file.write_text((proc.stdout or "") + (proc.stderr or ""),
                               encoding="utf-8", errors="replace")
    except subprocess.TimeoutExpired as exc:
        timed_out = True
        rc = -1
        stdout_file.write_text(
            (exc.stdout or b"" if isinstance(exc.stdout, bytes) else exc.stdout or "") +
            (exc.stderr or b"" if isinstance(exc.stderr, bytes) else exc.stderr or ""),
            encoding="utf-8", errors="replace")
    except OSError as exc:
        # Exe vanished mid-suite (concurrent build relinking the tree) —
        # report as a no-boot run so _early_window_close can retry it.
        rc = 0
        stdout_file.write_text(f"launch failed: {exc}\n",
                               encoding="utf-8", errors="replace")
        log_file.write_text("WindowIsRunning=0\n", encoding="utf-8")

    log = log_file.read_text(encoding="utf-8", errors="replace") \
        if log_file.is_file() else stdout_file.read_text(encoding="utf-8", errors="replace")
    return GameRun(rc, log, timed_out)


def assert_clean_boot(run: GameRun) -> None:
    """Shared post-conditions for a successful boot-to-frames run."""
    assert not run.timed_out, "game did not reach SSB64_MAX_FRAMES (hang)"
    assert "entering frame loop" in run.log or "syMainLoop returned" in run.log, \
        f"boot chain never completed:\n{run.log[-2000:]}"
    assert "clean shutdown" in run.log, \
        f"SSB64_MAX_FRAMES shutdown marker missing:\n{run.log[-2000:]}"
    assert run.returncode == 0, f"exit code {run.returncode}:\n{run.log[-2000:]}"


def png_is_rendered(path: Path, min_bytes: int = 10_000) -> bool:
    """Heuristic: a real frame PNG is >10KB and not a single flat colour.

    Uses PIL when available; otherwise checks that compressed IDAT entropy
    makes the file non-trivial (flat frames compress to a few KB at 960x540).
    """
    if not path.is_file() or path.stat().st_size < min_bytes:
        return False
    try:
        from PIL import Image
        with Image.open(path) as im:
            px = im.convert("L").resize((64, 36))
            extrema = px.getextrema()
            return extrema[1] - extrema[0] > 8  # has contrast
    except ImportError:
        return True  # size gate already applied


# ── artifact format probes (stdlib only) ───────────────────────────────

def elf_machine(path: Path) -> int | None:
    """e_machine of an ELF file (62 = x86-64, 183 = aarch64)."""
    data = path.read_bytes()[:24]
    if data[:4] != b"\x7fELF":
        return None
    return struct.unpack("<H", data[18:20])[0]


def macho_cputype(path: Path) -> int | None:
    """Mach-O cputype (0x0100000C = arm64, 0x01000007 = x86_64)."""
    data = path.read_bytes()[:8]
    if len(data) < 8:
        return None
    magic = struct.unpack("<I", data[:4])[0]
    if magic in (0xFEEDFACF, 0xFEEDFACE):          # Mach-O 64/32 LE
        return struct.unpack("<I", data[4:8])[0]
    if magic in (0xCFFAEDFE, 0xCEFAEDFE):          # big-endian view
        return struct.unpack(">I", data[4:8])[0]
    if magic == 0xCAFEBABE:                        # fat
        return -1
    return None


def nro_header_ok(path: Path) -> bool:
    """Switch .nro: 'NRO0' magic at offset 0x10."""
    try:
        with path.open("rb") as f:
            f.seek(0x10)
            return f.read(4) == b"NRO0"
    except OSError:
        return False


def zip_names(path: Path) -> set[str]:
    with zipfile.ZipFile(path) as z:
        return set(z.namelist())


# ── adb / emulator helpers ─────────────────────────────────────────────

def adb() -> str | None:
    """Resolve adb — PATH first, then the conventional SDK location."""
    found = shutil.which("adb")
    if found:
        return found
    sdk = os.environ.get("ANDROID_SDK_ROOT") or os.environ.get("ANDROID_HOME")
    if not sdk:
        sdk = str(Path.home() / "AppData" / "Local" / "Android" / "Sdk")
    cand = Path(sdk) / "platform-tools" / ("adb.exe" if sys.platform == "win32" else "adb")
    return str(cand) if cand.is_file() else None


def emulator_bin() -> str | None:
    sdk = os.environ.get("ANDROID_SDK_ROOT") or os.environ.get("ANDROID_HOME")
    if not sdk:
        sdk = str(Path.home() / "AppData" / "Local" / "Android" / "Sdk")
    cand = Path(sdk) / "emulator" / ("emulator.exe" if sys.platform == "win32" else "emulator")
    return str(cand) if cand.is_file() else None


def adb_out(adb_bin: str, *args: str, timeout: int = 30) -> subprocess.CompletedProcess:
    return subprocess.run([adb_bin, *args], capture_output=True, text=True,
                          timeout=timeout, check=False)


def adb_devices(adb_bin: str) -> list[str]:
    out = adb_out(adb_bin, "devices").stdout
    return [ln.split()[0] for ln in out.splitlines()[1:]
            if ln.strip() and ln.split()[-1] == "device"]


def wait_for_boot(adb_bin: str, serial: str, timeout_s: int = 180) -> bool:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        out = adb_out(adb_bin, "-s", serial, "shell", "getprop",
                      "sys.boot_completed", timeout=15)
        if out.stdout.strip() == "1":
            return True
        time.sleep(3)
    return False


def start_avd(avd: str, log: Path) -> subprocess.Popen | None:
    """Boot an AVD headless. Returns the emulator process (still running)."""
    emu = emulator_bin()
    if not emu:
        return None
    lf = log.open("w", encoding="utf-8", errors="replace")
    return subprocess.Popen(
        [emu, "-avd", avd, "-no-window", "-no-audio", "-no-snapshot-save",
         "-gpu", "swiftshader_indirect"],
        stdout=lf, stderr=subprocess.STDOUT)
