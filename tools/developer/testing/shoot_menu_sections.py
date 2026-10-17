"""Capture a screenshot of every PortMenu section for docs.

Launches the built binary once per (header, sidebar) pair, pre-seeding the
user config (BattleShip.cfg.json in the app-data dir) with the menu's
selection CVars, and lets the built-in backbuffer capture
(SSB64_SCREENSHOT_FRAMES) write the PNG. SSB64_MAX_FRAMES exits the process
after the capture frame. The pre-existing config file is merged in place and
restored afterwards.

Usage:
    python tools/developer/testing/shoot_menu_sections.py [--bin out/build/verify/Debug] \
        [--out docs/screenshots/menu] [--only settings-general online-netplay]

Requires a dev/test-hooks build for SSB64_MAX_FRAMES.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]

# (header entry, sidebar cvar suffix, sidebar, output filename)
SECTIONS = [
    ("Play", "PlaySidebarSection", "Jump to Screen", "play-jump-to-screen"),
    ("Play", "PlaySidebarSection", "Records", "play-records"),
    ("Play", "PlaySidebarSection", "Replays", "play-replays"),
    ("Online", "OnlineSidebarSection", "Netplay", "online-netplay"),
    ("Settings", "SettingsSidebarSection", "General", "settings-general"),
    ("Settings", "SettingsSidebarSection", "Modified", "settings-modified"),
    ("Settings", "SettingsSidebarSection", "Display", "settings-display"),
    ("Settings", "SettingsSidebarSection", "Image Quality", "settings-image-quality"),
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
    ("Customization", "CustomizationSidebarSection", "Shaders", "customization-shaders"),
    ("Customization", "CustomizationSidebarSection", "Texture Packs", "customization-texture-packs"),
    ("Customization", "CustomizationSidebarSection", "Script Mods", "customization-script-mods"),
    ("Game Files", "AssetsSidebarSection", "Extraction", "gamefiles-extraction"),
    ("Game Files", "AssetsSidebarSection", "Paths", "gamefiles-paths"),
    ("Support", "SupportSidebarSection", "Actions", "support-actions"),
    ("Support", "SupportSidebarSection", "Diagnostics", "support-diagnostics"),
    ("Support", "SupportSidebarSection", "Updates", "support-updates"),
    ("About", "AboutSidebarSection", "Build", "about-build"),
]

# Floating windows / alternate states — captured by toggling their visibility
# CVars instead of menu selection.
#   name    — output filename
#   cvars   — extra CVars written into the config
#   sidebar — menu sidebar to select (default: Settings>General)
#   size    — window size override (default: CAPTURE_SIZE)
# Note: window-online-status was removed — the Online Status window only has
# meaningful content during a live netplay session; capture it manually.
WINDOWS = [
    {"name": "window-stats", "cvars": {"gStatsEnabled": 1}},
    {"name": "window-console", "cvars": {"gConsoleEnabled": 1}},
    {"name": "window-gfx-debugger", "cvars": {"gGfxDebuggerEnabled": 1}},
    {"name": "window-controller-config", "cvars": {"gControllerConfigurationEnabled": 1}},
    {"name": "menu-popout", "cvars": {"gSettings.Menu.Popout": 1}},
    {"name": "menu-controller-nav", "cvars": {"gControlNav": 1}},
    {"name": "menu-opaque-bg", "cvars": {"gSettings.Menu.BackgroundOpacity": 1.0}},
    # Small-window run exercises the compact layout (header overflow +
    # horizontal section strip). Cheats has many sidebar entries.
    {"name": "menu-compact", "cvars": {}, "sidebar": "Cheats", "size": (320, 240)},
    {"name": "toasts", "cvars": {}},  # needs SSB64_TOAST_DEMO=1 — see EXTRA_ENV
]

# Uniform window size for every capture so the gallery is visually comparable.
CAPTURE_SIZE = (960, 540)

# --matrix runs this reduced section set across the responsive matrix below;
# the full gallery would multiply into hundreds of launches.
MATRIX_SECTIONS = {"settings-general", "settings-input-mappings", "online-netplay", "support-actions"}
MATRIX_SIZES = [(1280, 720), (1920, 1080), (320, 240), (559, 360), (561, 360)]
MATRIX_SCALES = [0.75, 1.0, 1.5, 2.0]

# Extra env vars per capture name (on top of SSB64_SCREENSHOT_*/MAX_FRAMES).
EXTRA_ENV = {
    "toasts": {"SSB64_TOAST_DEMO": "1"},
}

CAPTURE_FRAME = 360   # ~6s at 60fps — past boot/intro with the menu overlaid
EXIT_FRAME = 400


def set_cvar(cfg: dict, dotted: str, value) -> None:
    """cfg['CVars'] uses nested objects mirroring the CVar's dot path."""
    node = cfg.setdefault("CVars", {})
    parts = dotted.split(".")
    for part in parts[:-1]:
        node = node.setdefault(part, {})
    node[parts[-1]] = value


def del_cvar(cfg: dict, dotted: str) -> None:
    node = cfg.get("CVars", {})
    parts = dotted.split(".")
    for part in parts[:-1]:
        node = node.get(part, {}) if isinstance(node, dict) else {}
    if isinstance(node, dict):
        node.pop(parts[-1], None)


# Every cvar any WINDOWS entry may set — cleared before each shot so extras
# from a previous capture don't leak into the next one.
EXTRA_KEYS = [k for entry in WINDOWS for k in entry["cvars"]]
EXTRA_KEYS.append("gOnlineStatus.Visible")  # removed from auto-capture; clear stale state
EXTRA_KEYS.append("gSettings.UIScale")      # --ui-scale must not leak between shots
EXTRA_KEYS.append("gSettings.Language")     # a prior non-EN run must not leak into the canonical gallery


def write_cfg(cfg_path: Path, header: str, sidebar_cvar: str, sidebar: str,
              extra: dict | None = None, size: tuple = CAPTURE_SIZE,
              ui_scale: float | None = None) -> None:
    cfg = {}
    if cfg_path.is_file():
        try:
            cfg = json.loads(cfg_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            cfg = {}
    for k in EXTRA_KEYS:
        del_cvar(cfg, k)
    set_cvar(cfg, "gOpenPortMenu", 1)
    set_cvar(cfg, "gSettings.Menu.ActiveHeader", header)
    set_cvar(cfg, f"gSettings.Menu.{sidebar_cvar}", sidebar)
    for k, v in (extra or {}).items():
        set_cvar(cfg, k, v)
    if ui_scale is not None:
        set_cvar(cfg, "gSettings.UIScale", ui_scale)
    # Uniform windowed size for comparable shots (Window.* lives outside CVars).
    win = cfg.setdefault("Window", {})
    win["Width"], win["Height"] = size
    win.setdefault("Fullscreen", {})["Enabled"] = False
    cfg_path.write_text(json.dumps(cfg, indent=2), encoding="utf-8")


def run_shot(exe: Path, cwd: Path, env: dict, tmp: Path, dest: Path) -> bool:
    shot = tmp / f"frame_{CAPTURE_FRAME}.png"
    shot.unlink(missing_ok=True)
    proc = subprocess.run([str(exe)], cwd=str(cwd), env=env,
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                          timeout=180, check=False)
    if shot.is_file() and shot.stat().st_size > 10_000:
        shutil.move(str(shot), str(dest))
        return True
    print(f"    (rc={proc.returncode}, shot exists={shot.exists()})")
    return False


def app_cfg_paths(exe_dir: Path) -> list:
    """Every location the runtime may resolve its config from. The platform
    app dir (%APPDATA%\BattleShip) is authoritative for this build, but a
    BattleShip.cfg.json next to the exe is used by portable runs — seed
    both so the menu selection lands regardless of which one wins."""
    paths = []
    appdata = os.environ.get("APPDATA")
    if appdata:
        paths.append(Path(appdata) / "BattleShip" / "BattleShip.cfg.json")
    else:
        paths.append(Path.home() / ".config" / "BattleShip" / "BattleShip.cfg.json")
    paths.append(exe_dir / "BattleShip.cfg.json")
    return paths


def parse_size(text: str) -> tuple:
    w, sep, h = text.lower().partition("x")
    if not sep:
        raise argparse.ArgumentTypeError(f"size must be WxH, got {text!r}")
    return (int(w), int(h))


def diff_pngs(current: Path, baseline: Path, tolerance: int) -> tuple:
    """Per-pixel diff. Returns (changed_ratio, mean_delta) or (None, reason)."""
    try:
        import numpy as np
        from PIL import Image
    except ImportError:
        return (None, "PIL/numpy not installed")
    if not baseline.is_file():
        return (None, "no baseline")
    a = np.asarray(Image.open(current).convert("RGB"), dtype=np.int16)
    b = np.asarray(Image.open(baseline).convert("RGB"), dtype=np.int16)
    if a.shape != b.shape:
        return (None, f"size mismatch {b.shape[:2]} -> {a.shape[:2]}")
    delta = np.abs(a - b).max(axis=2)
    changed = float((delta > tolerance).mean())
    return (changed, float(delta.mean()))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", type=Path, default=REPO_ROOT / "out" / "build" / "verify" / "Debug",
                    help="Directory containing BattleShip.exe")
    ap.add_argument("--out", type=Path, default=REPO_ROOT / "docs" / "screenshots" / "menu",
                    help="Output directory for PNGs")
    ap.add_argument("--only", nargs="*", default=None,
                    help="Capture only these output names (e.g. settings-general)")
    ap.add_argument("--size", type=parse_size, default=None,
                    help="Override window size for every capture, e.g. 1280x720")
    ap.add_argument("--ui-scale", type=float, default=None,
                    help="Seed gSettings.UIScale (0.75-2.5) for every capture")
    ap.add_argument("--matrix", action="store_true",
                    help="Run MATRIX_SECTIONS across MATRIX_SIZES x MATRIX_SCALES")
    ap.add_argument("--baseline", type=Path, default=None,
                    help="Copy each captured PNG into this dir as the accepted baseline")
    ap.add_argument("--diff", type=Path, default=None,
                    help="Compare each captured PNG against baselines in this dir")
    ap.add_argument("--diff-tolerance", type=int, default=32,
                    help="Per-channel delta above which a pixel counts as changed (0-255)")
    ap.add_argument("--max-diff-ratio", type=float, default=0.02,
                    help="Fail --diff when more than this fraction of pixels changed")
    args = ap.parse_args()

    # Resolve now: SSB64_SCREENSHOT_DIR is consumed by the game, which
    # interprets a relative path against ITS cwd (the exe dir), not ours.
    args.bin = args.bin.resolve()
    args.out = args.out.resolve()

    exe = args.bin / "BattleShip.exe"
    if not exe.is_file():
        print(f"error: {exe} not found", file=sys.stderr)
        return 2

    cfg_paths = app_cfg_paths(args.bin)
    tmp = args.bin / ".menu_shots_tmp"
    args.out.mkdir(parents=True, exist_ok=True)
    tmp.mkdir(exist_ok=True)
    if args.baseline:
        args.baseline.mkdir(parents=True, exist_ok=True)

    env = dict(os.environ)
    env.update({
        "SSB64_SCREENSHOT_FRAMES": str(CAPTURE_FRAME),
        "SSB64_SCREENSHOT_DIR": str(tmp),
        "SSB64_MAX_FRAMES": str(EXIT_FRAME),
        "SSB64_MUTE": "1",
        "SSB64_NO_UPDATE_CHECK": "1",
    })

    # --matrix expands (section x size x scale) into the same name-keyed loop.
    sections = [(h, s, sb, n) for h, s, sb, n in SECTIONS]
    windows = list(WINDOWS)
    if args.matrix:
        sections = [e for e in sections if e[3] in MATRIX_SECTIONS]
        windows = [e for e in windows if e["name"] == "menu-compact"]
    if args.size:
        for e in windows:
            if "size" not in e or e["name"] != "menu-compact":
                e["size"] = args.size
    capture_size = args.size or CAPTURE_SIZE

    produced = []  # (name, dest_path)
    old_cfgs = {p: p.read_bytes() if p.is_file() else None for p in cfg_paths}
    results = []

    seed_fails = []

    def capture(name: str, dest: Path, shot_env: dict, expect_header: str | None = None) -> bool:
        ok = run_shot(exe, args.bin, shot_env, tmp, dest)
        results.append((name, ok))
        print(f"{'OK  ' if ok else 'FAIL'} {name}")
        if ok:
            produced.append((name, dest))
        # The runtime writes the last-rendered header back to the cfg on
        # exit — with no input it must equal the seed. A mismatch means the
        # seed was ignored (e.g. a shadowing cfg took precedence).
        if expect_header:
            rendered = None
            for p in cfg_paths:
                if p.is_file():
                    try:
                        rendered = json.loads(p.read_text(encoding="utf-8")) \
                            .get("CVars", {}).get("gSettings", {}).get("Menu", {}) \
                            .get("ActiveHeader")
                        if rendered is not None:
                            break
                    except (json.JSONDecodeError, OSError):
                        continue
            if rendered is not None and rendered != expect_header:
                seed_fails.append(name)
                print(f"SEED? {name}: rendered '{rendered}', seeded '{expect_header}'")
        return ok

    try:
        if args.matrix:
            for size in MATRIX_SIZES:
                for scale in MATRIX_SCALES:
                    tag = f"{size[0]}x{size[1]}-s{scale:g}"
                    for header, scvar, sidebar, name in sections:
                        if args.only and name not in args.only:
                            continue
                        out_name = f"{name}-{tag}"
                        for p in cfg_paths:
                            write_cfg(p, header, scvar, sidebar, size=size, ui_scale=scale)
                        capture(out_name, args.out / f"{out_name}.png", env,
                                expect_header=header)
        else:
            for header, scvar, sidebar, name in sections:
                if args.only and name not in args.only:
                    continue
                for p in cfg_paths:
                    write_cfg(p, header, scvar, sidebar,
                              size=capture_size, ui_scale=args.ui_scale)
                capture(name, args.out / f"{name}.png", env, expect_header=header)

            # Floating windows & alt states — default context is Settings>General
            for entry in windows:
                name = entry["name"]
                if args.only and name not in args.only:
                    continue
                for p in cfg_paths:
                    write_cfg(p, "Settings", "SettingsSidebarSection",
                              entry.get("sidebar", "General"), entry["cvars"],
                              entry.get("size", capture_size), args.ui_scale)
                shot_env = dict(env)
                shot_env.update(EXTRA_ENV.get(name, {}))
                capture(name, args.out / f"{name}.png", shot_env)
    finally:
        for p, old in old_cfgs.items():
            if old is not None:
                p.write_bytes(old)
            elif p.is_file():
                p.unlink()
        shutil.rmtree(tmp, ignore_errors=True)

    if args.baseline:
        for name, dest in produced:
            shutil.copy2(dest, args.baseline / dest.name)
        print(f"baseline updated: {len(produced)} -> {args.baseline}")

    diff_fails = []
    if args.diff:
        for name, dest in produced:
            ratio_or_none, extra = diff_pngs(dest, args.diff / dest.name, args.diff_tolerance)
            if ratio_or_none is None:
                print(f"SKIP {name}: {extra}")
            elif ratio_or_none > args.max_diff_ratio:
                diff_fails.append(name)
                print(f"DIFF {name}: {ratio_or_none:.2%} pixels changed "
                      f"(mean delta {extra:.1f})")
            else:
                print(f"SAME {name}: {ratio_or_none:.3%} changed")

    fails = [n for n, ok in results if not ok]
    print(f"\n{len(results) - len(fails)}/{len(results)} captured -> {args.out}")
    if fails:
        print("failed:", ", ".join(fails))
    if seed_fails:
        print("seed ignored:", ", ".join(seed_fails))
    if diff_fails:
        print("diff exceeded:", ", ".join(diff_fails))
    return 1 if (fails or seed_fails or diff_fails) else 0


if __name__ == "__main__":
    sys.exit(main())
