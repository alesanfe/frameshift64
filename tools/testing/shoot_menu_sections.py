"""Capture a screenshot of every PortMenu section for docs.

Launches the built binary once per (header, sidebar) pair, pre-seeding
BattleShip.cfg.json with the menu's selection CVars, and lets the
built-in backbuffer capture (SSB64_SCREENSHOT_FRAMES) write the PNG.
SSB64_MAX_FRAMES exits the process after the capture frame.

Usage:
    python tools/testing/shoot_menu_sections.py [--bin out/build/verify/Debug] \
        [--out docs/screenshots/menu] [--only settings-general online-netplay]

Requires a Debug/dev build (SSB64_TEST_HOOKS) for SSB64_MAX_FRAMES.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

# (header entry, sidebar cvar suffix, sidebar, output filename)
SECTIONS = [
    ("Settings", "SettingsSidebarSection", "General", "settings-general"),
    ("Settings", "SettingsSidebarSection", "Display", "settings-display"),
    ("Settings", "SettingsSidebarSection", "Image Quality", "settings-image-quality"),
    ("Settings", "SettingsSidebarSection", "Shaders", "settings-shaders"),
    ("Settings", "SettingsSidebarSection", "Advanced", "settings-advanced"),
    ("Settings", "SettingsSidebarSection", "Match Rules", "settings-match-rules"),
    ("Settings", "SettingsSidebarSection", "Classic Mode", "settings-classic-mode"),
    ("Settings", "SettingsSidebarSection", "Practice", "settings-practice"),
    ("Settings", "SettingsSidebarSection", "Interface", "settings-interface"),
    ("Settings", "SettingsSidebarSection", "Input Mappings", "settings-input-mappings"),
    ("Settings", "SettingsSidebarSection", "Control Enhancements", "settings-control-enhancements"),
    ("Settings", "SettingsSidebarSection", "Audio", "settings-audio"),
    ("Settings", "SettingsSidebarSection", "Accessibility", "settings-accessibility"),
    ("Settings", "SettingsSidebarSection", "Cheats", "settings-cheats"),
    ("Online", "OnlineSidebarSection", "Netplay", "online-netplay"),
    ("Game Files", "AssetsSidebarSection", "Extraction", "gamefiles-extraction"),
    ("Game Files", "AssetsSidebarSection", "Paths", "gamefiles-paths"),
    ("Game Files", "AssetsSidebarSection", "Texture Packs", "gamefiles-texture-packs"),
    ("Game Files", "AssetsSidebarSection", "Script Mods", "gamefiles-script-mods"),
    ("Support", "SupportSidebarSection", "Actions", "support-actions"),
    ("Support", "SupportSidebarSection", "Updates", "support-updates"),
    ("About", "AboutSidebarSection", "Build", "about-build"),
]

CAPTURE_FRAME = 360   # ~6s at 60fps — past boot/intro with the menu overlaid
EXIT_FRAME = 400


def write_cfg(cfg_path: Path, header: str, sidebar_cvar: str, sidebar: str) -> None:
    cfg = {
        "CVars": {
            "gOpenPortMenu": 1,
            "gSettings": {
                "Menu": {
                    "ActiveHeader": header,
                    sidebar_cvar: sidebar,
                    "BackgroundOpacity": 0.92,
                },
                "PersistentDescriptions": 1,
            },
        }
    }
    cfg_path.write_text(json.dumps(cfg, indent=2), encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", type=Path, default=REPO_ROOT / "out" / "build" / "verify" / "Debug",
                    help="Directory containing BattleShip.exe")
    ap.add_argument("--out", type=Path, default=REPO_ROOT / "docs" / "screenshots" / "menu",
                    help="Output directory for PNGs")
    ap.add_argument("--only", nargs="*", default=None,
                    help="Capture only these output names (e.g. settings-general)")
    ap.add_argument("--keep-config", action="store_true",
                    help="Don't delete the generated BattleShip.cfg.json afterwards")
    args = ap.parse_args()

    exe = args.bin / "BattleShip.exe"
    if not exe.is_file():
        print(f"error: {exe} not found", file=sys.stderr)
        return 2

    cfg_path = args.bin / "BattleShip.cfg.json"
    tmp = args.bin / ".menu_shots_tmp"
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

    old_cfg = cfg_path.read_bytes() if cfg_path.is_file() else None
    results = []
    try:
        for header, scvar, sidebar, name in SECTIONS:
            if args.only and name not in args.only:
                continue
            write_cfg(cfg_path, header, scvar, sidebar)
            shot = tmp / f"frame_{CAPTURE_FRAME}.png"
            shot.unlink(missing_ok=True)
            proc = subprocess.run([str(exe)], cwd=str(args.bin), env=env,
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                  timeout=180)
            dest = args.out / f"{name}.png"
            if shot.is_file() and shot.stat().st_size > 10_000:
                shutil.move(str(shot), str(dest))
                results.append((name, True))
                print(f"OK   {name} ({dest.stat().st_size} bytes)")
            else:
                results.append((name, False))
                print(f"FAIL {name} (rc={proc.returncode})")
    finally:
        if old_cfg is not None:
            cfg_path.write_bytes(old_cfg)
        elif cfg_path.is_file() and not args.keep_config:
            cfg_path.unlink()
        shutil.rmtree(tmp, ignore_errors=True)

    fails = [n for n, ok in results if not ok]
    print(f"\n{len(results) - len(fails)}/{len(results)} captured -> {args.out}")
    if fails:
        print("failed:", ", ".join(fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
