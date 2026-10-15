"""Smoke-test the i18n runtime by capturing the menu in each language.

For every shipped lang/<code>.json the script seeds gSettings.Language into
the user config, launches the binary once, and lets the built-in backbuffer
capture (SSB64_SCREENSHOT_FRAMES) write the PNG. It also covers the three
fallback paths the loader must survive silently:

  missing file   — lang/<code>.json temporarily moved away -> English
  malformed JSON — file replaced with garbage             -> English
  invalid index  — gSettings.Language out of range        -> English

All touched files (config, lang tables) are restored afterwards.

Usage:
    python tools/developer/testing/shoot_languages.py \
        [--bin out/build/verify/Debug] [--out out/screens/lang-test] \
        [--only es fr]

Requires a test-hooks build (SSB64_MAX_FRAMES) and lang/*.json staged next
to the executable.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]

# gSettings.Language index -> (code, expected behaviour)
LANGUAGES = {1: "es", 2: "fr", 3: "de", 4: "pt"}

CAPTURE_FRAME = 360
EXIT_FRAME = 400
CAPTURE_SIZE = (960, 540)


def set_cvar(cfg: dict, dotted: str, value) -> None:
    node = cfg.setdefault("CVars", {})
    parts = dotted.split(".")
    for part in parts[:-1]:
        node = node.setdefault(part, {})
    node[parts[-1]] = value


def write_cfg(cfg_path: Path, language: int) -> None:
    cfg = {}
    if cfg_path.is_file():
        try:
            cfg = json.loads(cfg_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            cfg = {}
    set_cvar(cfg, "gOpenPortMenu", 1)
    set_cvar(cfg, "gSettings.Language", language)
    set_cvar(cfg, "gSettings.Menu.ActiveHeader", "Settings")
    set_cvar(cfg, "gSettings.Menu.SettingsSidebarSection", "General")
    win = cfg.setdefault("Window", {})
    win["Width"], win["Height"] = CAPTURE_SIZE
    win.setdefault("Fullscreen", {})["Enabled"] = False
    cfg_path.write_text(json.dumps(cfg, indent=2), encoding="utf-8")


def app_cfg_paths(exe_dir: Path) -> list:
    paths = []
    appdata = os.environ.get("APPDATA")
    if appdata:
        paths.append(Path(appdata) / "BattleShip" / "BattleShip.cfg.json")
    else:
        paths.append(Path.home() / ".config" / "BattleShip" / "BattleShip.cfg.json")
    paths.append(exe_dir / "BattleShip.cfg.json")
    return paths


def kill_leftovers() -> None:
    """Kill crashed-but-alive BattleShip.exe processes from earlier runs.

    A zombie keeps running after its window dies and can write the shared
    user config late, stomping the seed of the next run (observed: the pt
    capture rendered German because a leftover de process rewrote the cfg).
    """
    if os.name == "nt":
        subprocess.run(["taskkill", "/F", "/IM", "BattleShip.exe"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       check=False)
        time.sleep(1.5)   # let file locks / late config writes settle


def run_shot(exe: Path, cwd: Path, env: dict, tmp: Path, dest: Path) -> bool:
    shot = tmp / f"frame_{CAPTURE_FRAME}.png"
    shot.unlink(missing_ok=True)
    try:
        proc = subprocess.run([str(exe)], cwd=str(cwd), env=env,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                              timeout=180, check=False)
        rc = proc.returncode
    except subprocess.TimeoutExpired:
        rc = "timeout"
        kill_leftovers()
    if shot.is_file() and shot.stat().st_size > 10_000:
        shutil.move(str(shot), str(dest))
        return True
    print(f"    (rc={rc}, shot exists={shot.exists()})")
    return False


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", type=Path, default=None,
                    help="Directory containing BattleShip.exe + lang/ "
                         "(auto-detected when omitted)")
    ap.add_argument("--out", type=Path,
                    default=REPO_ROOT / "out" / "screens" / "lang-test")
    ap.add_argument("--only", nargs="*", default=None,
                    help="Only these language codes (e.g. es fr)")
    args = ap.parse_args()

    if args.bin is None:
        # Build trees are deleted/recreated often; pick the first candidate
        # that actually has an executable instead of hardcoding one path.
        candidates = [REPO_ROOT / "out" / "build" / "components-verify",
                      REPO_ROOT / "out" / "build" / "verify" / "Debug"]
        args.bin = next((c for c in candidates
                         if (c / "BattleShip.exe").is_file()), candidates[0])
        print(f"--bin not given; using {args.bin}")

    args.bin = args.bin.resolve()
    args.out = args.out.resolve()
    exe = args.bin / "BattleShip.exe"
    if not exe.is_file():
        print(f"error: {exe} not found", file=sys.stderr)
        return 2

    lang_dir = args.bin / "lang"
    cfg_paths = app_cfg_paths(args.bin)
    tmp = args.bin / ".lang_shots_tmp"
    args.out.mkdir(parents=True, exist_ok=True)
    tmp.mkdir(exist_ok=True)

    env = dict(os.environ)
    env.update({
        "SSB64_SCREENSHOT_FRAMES": str(CAPTURE_FRAME),
        "SSB64_SCREENSHOT_DIR": str(tmp),
        "SSB64_MAX_FRAMES": str(EXIT_FRAME),
        "SSB64_MUTE": "1",
        "SSB64_NO_UPDATE_CHECK": "1",
    })

    old_cfgs = {p: p.read_bytes() if p.is_file() else None for p in cfg_paths}
    moved: list[tuple[Path, Path]] = []   # (original, parked) lang files
    mangled: list[tuple[Path, bytes]] = []  # (file, original bytes)
    results = []

    def seed_and_shoot(name: str, language: int) -> bool:
        kill_leftovers()                    # zombies can stomp the seed
        for p in cfg_paths:
            write_cfg(p, language)
        ok = run_shot(exe, args.bin, env, tmp, args.out / f"{name}.png")
        results.append((name, ok))
        print(f"{'OK  ' if ok else 'FAIL'} {name}")
        return ok

    try:
        langs = {i: c for i, c in LANGUAGES.items()
                 if (args.only is None or c in args.only)}
        for idx, code in sorted(langs.items()):
            if (lang_dir / f"{code}.json").is_file():
                seed_and_shoot(f"menu-language-{code}", idx)
            else:
                print(f"SKIP {code}: {lang_dir / (code + '.json')} not staged")
                results.append((f"menu-language-{code}", False))

        # --- fallback paths (only when running the full set) ----------------
        if args.only is None:
            # 1. Missing file: park de.json, seed German -> English render.
            de = lang_dir / "de.json"
            if de.is_file():
                parked = de.with_suffix(".json.parked")
                de.rename(parked)
                moved.append((de, parked))
                seed_and_shoot("menu-language-missing-fallback", 3)

            # 2. Malformed JSON: overwrite pt.json with garbage, restore after.
            pt = lang_dir / "pt.json"
            if pt.is_file():
                original = pt.read_bytes()
                mangled.append((pt, original))
                pt.write_text("{ not json !!", encoding="utf-8")
                seed_and_shoot("menu-language-badjson-fallback", 4)

            # 3. Out-of-range index clamps to English.
            seed_and_shoot("menu-language-badindex-fallback", 99)
    finally:
        for p, old in old_cfgs.items():
            if old is not None:
                p.write_bytes(old)
            elif p.is_file():
                p.unlink()
        for target, parked in moved:
            if parked.is_file():
                parked.rename(target)
        for f, data in mangled:
            f.write_bytes(data)
        shutil.rmtree(tmp, ignore_errors=True)

    # Translated menus must differ across languages: identical PNGs mean the
    # seed was stomped by a leftover process or translations did not render.
    seen_hashes: dict[str, str] = {}
    for name, ok in results:
        png = args.out / f"{name}.png"
        if not ok or not name.startswith("menu-language-") \
                or name.endswith("-fallback") or not png.is_file():
            continue
        h = hashlib.sha256(png.read_bytes()).hexdigest()
        if h in seen_hashes:
            print(f"FAIL {name} identical to {seen_hashes[h]} "
                  "(seed race or untranslated render)")
            results.append((name, False))
        else:
            seen_hashes[h] = name

    fails = [n for n, ok in results if not ok]
    print(f"\n{len(results) - len(fails)}/{len(results)} captured -> {args.out}")
    if fails:
        print("failed:", ", ".join(fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
