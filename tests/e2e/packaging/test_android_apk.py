"""Android deliverable E2E: APK structure + install + real boot on an
emulator/device over adb.

Structure checks always run when the APK exists. The device tests skip when
no adb is available; if a device isn't online but an AVD is configured, the
test boots the first listed AVD headless and uses it.
"""

from __future__ import annotations

import json
import re
import time
import zipfile
from pathlib import Path

import pytest
from smash_test.artifacts import (
    REPO_ROOT,
    adb,
    adb_devices,
    adb_out,
    emulator_bin,
    png_is_rendered,
    start_avd,
    wait_for_boot,
)

pytestmark = [pytest.mark.e2e, pytest.mark.serial]

APK_GLOB = "platforms/android/app/build/outputs/apk/*/app-*.apk"
PKG = "com.jrickey.battleship.debug"
# applicationIdSuffix=".debug" renames the package id but NOT the class
# namespace — activities stay under com.jrickey.battleship.*.
BOOT_ACTIVITY = f"{PKG}/com.jrickey.battleship.BootActivity"
def _find_o2r() -> Path | None:
    """BattleShip.o2r is arch-independent — any build tree works."""
    cands = sorted(REPO_ROOT.glob("out/build/*/BattleShip.o2r"),
                   key=lambda p: p.stat().st_mtime, reverse=True)
    return cands[0] if cands else None


O2R_SRC = _find_o2r()


def _find_apk() -> Path | None:
    cands = sorted(REPO_ROOT.glob(APK_GLOB),
                   key=lambda p: p.stat().st_mtime, reverse=True)
    return cands[0] if cands else None


@pytest.fixture(scope="module")
def apk() -> Path:
    path = _find_apk()
    if path is None:
        pytest.skip("no built APK under platforms/android/app/build/outputs/")
    return path


# ── structure (no device needed) ───────────────────────────────────────

def test_apk_contains_engine_libs(apk: Path):
    with zipfile.ZipFile(apk) as z:
        names = set(z.namelist())
    for so in ("lib/arm64-v8a/libmain.so", "lib/arm64-v8a/libSDL2.so",
               "lib/arm64-v8a/libtorch_runner.so"):
        assert so in names, f"{so} missing from APK"


def test_apk_contains_asset_pipeline(apk: Path):
    """Extraction recipes + f3d.o2r must ship so BootActivity can build
    BattleShip.o2r on-device."""
    with zipfile.ZipFile(apk) as z:
        names = set(z.namelist())
    assert "assets/f3d.o2r" in names
    assert "assets/config.yml" in names
    assert any(n.startswith("assets/yamls/us/") for n in names), \
        "US extraction recipes missing"


def test_apk_manifest_package(apk: Path):
    """Binary manifest strings still contain the applicationId.

    AXML string pools may be UTF-8 or UTF-16LE depending on the aapt2
    version — check both encodings.
    """
    with zipfile.ZipFile(apk) as z:
        manifest = z.read("AndroidManifest.xml")
    haystacks = (manifest.decode("utf-8", errors="replace").lower(),
                 manifest.decode("utf-16-le", errors="replace").lower())
    assert any("battleship" in h for h in haystacks), \
        "package id not in manifest"


def test_apk_dex_and_signing(apk: Path):
    with zipfile.ZipFile(apk) as z:
        names = set(z.namelist())
    assert "classes.dex" in names
    # Debug builds sign with the debug key — META-INF must be present.
    assert any(n.startswith("META-INF/") for n in names), "unsigned APK"


# ── device boot ────────────────────────────────────────────────────────

@pytest.fixture(scope="module")
def device(pytestconfig) -> str:
    adb_bin = adb()
    if adb_bin is None:
        pytest.skip("adb not found (no platform-tools)")
    dev_artifacts = pytestconfig._smash_artifacts / "android-device"
    dev_artifacts.mkdir(parents=True, exist_ok=True)
    serials = adb_devices(adb_bin)
    if not serials:
        emu = emulator_bin()
        if not emu:
            pytest.skip("no device and no emulator binary")
        import subprocess
        avds = [ln.strip() for ln in
                subprocess.run([emu, "-list-avds"],
                               capture_output=True, text=True,
                               timeout=15, check=False).stdout.splitlines()
                if ln.strip()]
        if not avds:
            pytest.skip("no device online and no AVDs configured")
        proc = start_avd(avds[0], dev_artifacts / "emulator.log")
        if proc is None:
            pytest.skip("could not start emulator")
        deadline = time.time() + 200
        serial = None
        while time.time() < deadline and serial is None:
            serials = adb_devices(adb_bin)
            serial = serials[0] if serials else None
            time.sleep(3)
        if serial is None or not wait_for_boot(adb_bin, serial):
            proc.kill()
            pytest.skip(f"AVD {avds[0]} did not boot in time")
        # Settle a few extra seconds — surfaceflinger/zygote still warming.
        time.sleep(10)
    else:
        serial = serials[0]
    yield serial


@pytest.mark.timeout(400)   # module-scoped AVD boot (≤210s) counts here
def test_apk_installs_on_device(apk: Path, device: str, artifacts_dir: Path):
    adb_bin = adb()
    abilist = adb_out(adb_bin, "-s", device, "shell",
                      "getprop", "ro.product.cpu.abilist").stdout
    if "arm64-v8a" not in abilist:
        pytest.skip(f"device ABI list {abilist.strip()!r} lacks arm64-v8a "
                    "(no ARM translation)")
    # A leftover install (older APK, abandoned staged session, residue in
    # /data/app) makes `adb install` die with
    # INSTALL_FAILED_INSUFFICIENT_STORAGE / "Failed to override installation
    # location" on emulator images — wipe first; uninstall of a missing
    # package just returns Failure on stderr.
    adb_out(adb_bin, "-s", device, "uninstall", PKG, timeout=120)
    out = adb_out(adb_bin, "-s", device, "install", "-r", "-g", str(apk),
                  timeout=300)
    assert "Success" in out.stdout, f"adb install failed: {out.stdout[-400:]}"


@pytest.mark.timeout(400)
def test_app_boots_and_renders(apk: Path, device: str, artifacts_dir: Path):
    adb_bin = adb()
    # Push a real BattleShip.o2r so BootActivity can jump straight into the
    # game — otherwise it parks on the ROM picker (still a valid boot, but
    # the full loop is worth more).
    if O2R_SRC is not None and O2R_SRC.is_file():
        files_dir = f"/sdcard/Android/data/{PKG}/files"
        adb_out(adb_bin, "-s", device, "shell", "mkdir", "-p", files_dir)
        adb_out(adb_bin, "-s", device, "push", str(O2R_SRC),
                f"{files_dir}/BattleShip.o2r", timeout=300)
    adb_out(adb_bin, "-s", device, "shell", "am", "force-stop", PKG)
    adb_out(adb_bin, "-s", device, "shell", "am", "start", "-n",
            BOOT_ACTIVITY, timeout=30)
    # Give the splash/extract/boot chain time, then inspect.
    time.sleep(25)
    log = adb_out(adb_bin, "-s", device, "logcat", "-d", "-t", "400",
                  timeout=30).stdout
    (artifacts_dir / "logcat.txt").write_text(log, encoding="utf-8",
                                              errors="replace")
    fatal = re.search(r"FATAL EXCEPTION|ANR in .*battleship|SIGSEGV|SIGABRT",
                      log)
    assert fatal is None, f"crash in logcat: {fatal.group(0)}"
    shot = artifacts_dir / "device.png"
    _screencap(adb_bin, device, shot)
    assert png_is_rendered(shot, min_bytes=20_000), \
        "screen capture is blank — app did not render"
    top = adb_out(adb_bin, "-s", device, "shell",
                  "dumpsys activity activities | grep -i battleship",
                  timeout=30).stdout
    assert "battleship" in top.lower() or "battleship" in log.lower(), \
        "BattleShip activity not in foreground"
    adb_out(adb_bin, "-s", device, "shell", "am", "force-stop", PKG)


# ── on-device UI captures ──────────────────────────────────────────────
# These run after test_app_boots_and_renders in the same module (serial),
# reusing the module-scoped `device` fixture so the AVD boots at most once.

_FILES_DIR = f"/sdcard/Android/data/{PKG}/files"


def _screencap(adb_bin: str, device: str, out_png: Path) -> Path:
    remote = "/sdcard/_battleship_e2e.png"
    adb_out(adb_bin, "-s", device, "shell", "screencap", "-p", remote,
            timeout=30)
    pull = adb_out(adb_bin, "-s", device, "pull", remote, str(out_png),
                   timeout=30)
    adb_out(adb_bin, "-s", device, "shell", "rm", "-f", remote)
    assert out_png.is_file(), f"screencap pull failed: {pull.stdout}"
    return out_png


def _push_game_cfg(adb_bin: str, device: str, tmp_dir: Path,
                   cvars: dict) -> None:
    """Seed BattleShip.cfg.json in the app's external files dir — the same
    location Context::GetPathRelativeToAppDirectory resolves on Android and
    where test_app_boots_and_renders already pushes the .o2r."""
    cfg = {"CVars": cvars,
           "Window": {"Width": 960, "Height": 540,
                      "Fullscreen": {"Enabled": False}}}
    local = tmp_dir / "BattleShip.cfg.json"
    local.write_text(json.dumps(cfg, indent=2), encoding="utf-8")
    adb_out(adb_bin, "-s", device, "shell", "mkdir", "-p", _FILES_DIR)
    out = adb_out(adb_bin, "-s", device, "push", str(local),
                  f"{_FILES_DIR}/BattleShip.cfg.json", timeout=60)
    assert out.returncode == 0, f"cfg push failed: {out.stdout} {out.stderr}"


def _launch_and_settle(adb_bin: str, device: str, settle_s: int = 30):
    adb_out(adb_bin, "-s", device, "shell", "am", "force-stop", PKG)
    adb_out(adb_bin, "-s", device, "shell", "am", "start", "-n",
            BOOT_ACTIVITY, timeout=30)
    time.sleep(settle_s)


def _ensure_o2r(adb_bin: str, device: str) -> None:
    """Stage BattleShip.o2r if absent — tests must not rely on
    test_app_boots_and_renders having pushed it (pytest-randomly reorders,
    and -k filters can deselect it entirely)."""
    if O2R_SRC is None or not O2R_SRC.is_file():
        return
    have = adb_out(adb_bin, "-s", device, "shell",
                   f"ls {_FILES_DIR}/BattleShip.o2r 2>/dev/null && echo Exists",
                   timeout=15).stdout
    if "Exists" in have:
        return
    adb_out(adb_bin, "-s", device, "shell", "mkdir", "-p", _FILES_DIR)
    out = adb_out(adb_bin, "-s", device, "push", str(O2R_SRC),
                  f"{_FILES_DIR}/BattleShip.o2r", timeout=300)
    assert out.returncode == 0, f"o2r push failed: {out.stdout} {out.stderr}"


@pytest.mark.timeout(600)
def test_android_ui_menu_overlay(apk: Path, device: str,
                                 artifacts_dir: Path):
    """Boot once with a clean cfg (baseline gameplay/attract frame) and once
    with gOpenPortMenu=1 seeded — the menu capture must render AND differ
    meaningfully from the baseline, proving the overlay is on screen."""
    adb_bin = adb()
    if O2R_SRC is None or not O2R_SRC.is_file():
        pytest.skip("no BattleShip.o2r to boot into the game with")
    _ensure_o2r(adb_bin, device)

    # Baseline: no cfg seed — whatever the game normally shows.
    adb_out(adb_bin, "-s", device, "shell", "rm", "-f",
            f"{_FILES_DIR}/BattleShip.cfg.json")
    _launch_and_settle(adb_bin, device)
    baseline = _screencap(adb_bin, device,
                          artifacts_dir / "ui-baseline.png")

    _push_game_cfg(adb_bin, device, artifacts_dir, {"gOpenPortMenu": 1})
    _launch_and_settle(adb_bin, device)
    try:
        shot = _screencap(adb_bin, device, artifacts_dir / "ui-menu.png")
        assert png_is_rendered(shot, min_bytes=20_000), \
            "menu capture is blank — ImGui overlay did not render"
        try:
            from PIL import Image, ImageChops
            with Image.open(baseline) as a, Image.open(shot) as b:
                a = a.convert("L").resize((160, 90))
                b = b.convert("L").resize((160, 90))
                diff = ImageChops.difference(a, b)
                changed = sum(1 for v in diff.getdata() if v > 24)
            ratio = changed / (160 * 90)
            assert ratio > 0.02, \
                f"menu frame identical to baseline ({ratio:.3%} px differ) " \
                "— overlay never appeared"
        except ImportError:
            pass  # pixel-diff needs PIL; artifacts are still saved either way
        log = adb_out(adb_bin, "-s", device, "logcat", "-d", "-t", "300",
                      timeout=30).stdout
        fatal = re.search(r"FATAL EXCEPTION|SIGSEGV|SIGABRT", log)
        assert fatal is None, f"crash while showing menu: {fatal.group(0)}"
    finally:
        adb_out(adb_bin, "-s", device, "shell", "rm", "-f",
                f"{_FILES_DIR}/BattleShip.cfg.json")
        adb_out(adb_bin, "-s", device, "shell", "am", "force-stop", PKG)


@pytest.mark.timeout(400)
def test_android_ui_rom_picker(apk: Path, device: str,
                               artifacts_dir: Path):
    """Boot without BattleShip.o2r → BootActivity parks on the ROM-picker UI.
    That first-run surface is part of the shipped UX — capture it."""
    adb_bin = adb()
    files_dir = _FILES_DIR
    had_o2r = "Exists" in adb_out(
        adb_bin, "-s", device, "shell",
        f"ls {files_dir}/BattleShip.o2r 2>/dev/null && echo Exists",
        timeout=15).stdout
    if had_o2r:
        adb_out(adb_bin, "-s", device, "shell", "mv", "-f",
                f"{files_dir}/BattleShip.o2r",
                f"{files_dir}/BattleShip.o2r.e2e_bak")
    _launch_and_settle(adb_bin, device, settle_s=20)
    try:
        shot = _screencap(adb_bin, device,
                          artifacts_dir / "ui-rom-picker.png")
        assert png_is_rendered(shot, min_bytes=8_000), \
            "ROM-picker capture is blank — first-run UI did not render"
        top = adb_out(adb_bin, "-s", device, "shell",
                      "dumpsys activity activities | grep -i battleship",
                      timeout=30).stdout
        assert "battleship" in top.lower(), \
            "BattleShip not in foreground for ROM-picker capture"
    finally:
        adb_out(adb_bin, "-s", device, "shell", "am", "force-stop", PKG)
        if had_o2r:
            adb_out(adb_bin, "-s", device, "shell", "mv", "-f",
                    f"{files_dir}/BattleShip.o2r.e2e_bak",
                    f"{files_dir}/BattleShip.o2r")
        if had_o2r:
            adb_out(adb_bin, "-s", device, "shell", "mv", "-f",
                    f"{files_dir}/BattleShip.o2r.e2e_bak",
                    f"{files_dir}/BattleShip.o2r")
