"""Capture the netplay CONNECTED room state for docs/screenshots/menu.

Spins up the local node-lobby adapter, auto-hosts one BattleShip instance
and auto-joins a second (the same env hooks the e2e suite uses:
SSB64_LOBBY_HOST / SSB64_LOBBY_JOIN / SSB64_LOBBY_CODE_FILE). With no
SSB64_LOBBY_AUTOSTART the match never starts, so both instances sit in the
room and the SSB64_SCREENSHOT_* hooks dump each side's framebuffer with
the menu open on Online > Netplay.

Usage:
    python tools/developer/testing/shoot_netplay_room.py [--bin out/build/verify/Debug]
        [--out docs/screenshots/menu] [--frames 720]

Requires node + services/netplay-lobby/node_modules (npm ci once) and a
dev/test-hooks build for SSB64_MAX_FRAMES.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
LOBBY = REPO_ROOT / "services" / "netplay-lobby" / "src" / "infrastructure" / "local" / "node-lobby.mjs"
SERVER = "ws://127.0.0.1:8799"
HEALTH = "http://127.0.0.1:8799/health"
# Taller than the section gallery: the room block (status, code, player
# list, connection details, start button) sits below the fighter picker.
CAPTURE_SIZE = (960, 800)


def app_cfg_paths(exe_dir: Path) -> list:
    r"""Every location the runtime may resolve its config from. NON_PORTABLE
    builds read only SDL_GetPrefPath (%APPDATA%\BattleShip) — see
    Context::InitConfiguration -> GetPathRelativeToAppDirectory — while a
    portable build reads ./BattleShip.cfg.json. Seed all candidates: the
    unused one is inert, never shadowing."""
    paths = []
    appdata = os.environ.get("APPDATA")
    if appdata:
        paths.append(Path(appdata) / "BattleShip" / "BattleShip.cfg.json")
    else:
        paths.append(Path.home() / ".config" / "BattleShip" / "BattleShip.cfg.json")
    paths.append(exe_dir / "BattleShip.cfg.json")
    return paths


def write_cfg(cfg_path: Path, size: tuple, lang: int = 0) -> None:
    cfg = {}
    if cfg_path.is_file():
        try:
            cfg = json.loads(cfg_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            cfg = {}
    menu = cfg.setdefault("CVars", {}).setdefault("gSettings", {}).setdefault("Menu", {})
    menu["ActiveHeader"] = "Online"
    menu["OnlineSidebarSection"] = "Netplay"
    # Keep room captures canonical-English: write_cfg preserves unknown keys,
    # so a Language seed left over from an i18n run would otherwise leak in.
    cfg["CVars"]["gSettings"]["Language"] = lang
    cfg["CVars"]["gOpenPortMenu"] = 1
    win = cfg.setdefault("Window", {})
    win["Width"], win["Height"] = size
    win.setdefault("Fullscreen", {})["Enabled"] = False
    cfg_path.write_text(json.dumps(cfg, indent=2), encoding="utf-8")


def lobby_healthy(timeout: float = 1.0) -> bool:
    try:
        return urllib.request.urlopen(HEALTH, timeout=timeout).status == 200  # nosec B310 — fixed localhost URL
    except Exception:
        return False


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", type=Path,
                    default=REPO_ROOT / "out" / "build" / "verify" / "Debug",
                    help="Directory containing BattleShip.exe")
    ap.add_argument("--out", type=Path,
                    default=REPO_ROOT / "docs" / "screenshots" / "menu",
                    help="Output directory for the two PNGs")
    ap.add_argument("--frames", type=int, default=720,
                    help="Capture frame (~N/60 seconds); room must settle first")
    ap.add_argument("--size", default=f"{CAPTURE_SIZE[0]}x{CAPTURE_SIZE[1]}",
                    help="Window size WxH")
    ap.add_argument("--work", type=Path,
                    default=REPO_ROOT / "out" / "lobby_shots",
                    help="Scratch dir for code file, saves and raw frames")
    ap.add_argument("--lang", type=int, default=0,
                    help="gSettings.Language to seed (0=en,1=es,2=fr,3=de,4=pt)")
    args = ap.parse_args()

    exe = args.bin.resolve() / "BattleShip.exe"
    if not exe.is_file():
        print(f"error: {exe} not found", file=sys.stderr)
        return 2
    # Absolute paths — SSB64_SCREENSHOT_DIR is resolved against the exe's CWD,
    # so a relative --work would scatter frames under out/build/<preset>/Debug.
    args.work = args.work.resolve()
    args.out = args.out.resolve()
    args.work.mkdir(parents=True, exist_ok=True)
    args.out.mkdir(parents=True, exist_ok=True)

    cfg_paths = app_cfg_paths(exe.parent)
    old_cfgs = {p: p.read_bytes() if p.is_file() else None for p in cfg_paths}
    code_file = args.work / "code.txt"
    code_file.unlink(missing_ok=True)
    procs: list[subprocess.Popen] = []
    lobby: subprocess.Popen | None = None
    exit_frames = args.frames + 60

    try:
        w, _, h = args.size.partition("x")
        for cfg_path in cfg_paths:
            write_cfg(cfg_path, (int(w), int(h)), args.lang)

        lobby_log = open(args.work / "lobby.log", "w", encoding="utf-8")
        lobby = subprocess.Popen(["node", str(LOBBY), "8799"],
                                 stdout=lobby_log, stderr=subprocess.STDOUT)
        for _ in range(60):
            if lobby_healthy():
                break
            time.sleep(0.5)
        else:
            print("lobby server did not start", file=sys.stderr)
            return 2
        if lobby.poll() is not None:
            print("WARNING: spawned lobby exited but :8799 is healthy — "
                  "reusing a stale lobby process; kill leftover node.exe "
                  "if room state looks wrong", file=sys.stderr)
        print(f"lobby up on {SERVER}")

        for role, fkind in (("host", 0), ("client", 1)):
            raw_dir = args.work / role
            raw_dir.mkdir(exist_ok=True)
            env = dict(os.environ)
            env.update({
                "SSB64_LOBBY_CODE_FILE": str(code_file),
                "SSB64_LOBBY_FKIND": str(fkind),
                "SSB64_MUTE": "1",
                "SSB64_NO_UPDATE_CHECK": "1",
                "SSB64_SCREENSHOT_FRAMES": str(args.frames),
                "SSB64_SCREENSHOT_DIR": str(raw_dir),
                "SSB64_MAX_FRAMES": str(exit_frames),
                "SSB64_SAVE_PATH": str(args.work / f"{role}.sav"),
            })
            env["SSB64_LOBBY_HOST" if role == "host" else "SSB64_LOBBY_JOIN"] = SERVER
            procs.append(subprocess.Popen([str(exe)], cwd=exe.parent, env=env,
                                          stdout=subprocess.DEVNULL,
                                          stderr=subprocess.DEVNULL))

        for p in procs:
            try:
                p.wait(timeout=args.frames * 2)
            except subprocess.TimeoutExpired:
                p.kill()

        ok = True
        for role in ("host", "client"):
            src = args.work / role / f"frame_{args.frames}.png"
            dst = args.out / f"online-room-{role}.png"
            if src.is_file() and src.stat().st_size > 10_000:
                shutil.copy2(src, dst)
                print(f"OK   {dst.name}")
            else:
                ok = False
                print(f"FAIL {role} (shot exists={src.exists()})")
        return 0 if ok else 1
    finally:
        for p in procs:
            if p.poll() is None:
                p.kill()
        if lobby is not None and lobby.poll() is None:
            if sys.platform == "win32":
                subprocess.run(["taskkill", "/PID", str(lobby.pid), "/T", "/F"],
                               capture_output=True, check=False)
            else:
                lobby.kill()
        for cfg_path, old_cfg in old_cfgs.items():
            if old_cfg is not None:
                cfg_path.write_bytes(old_cfg)
            elif cfg_path.is_file():
                cfg_path.unlink()


if __name__ == "__main__":
    sys.exit(main())
