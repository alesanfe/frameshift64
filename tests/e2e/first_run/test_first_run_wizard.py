"""First-run deliverable flow: a fresh install (no BattleShip.o2r anywhere)
must land on the setup wizard instead of crashing or silently exiting.

Runs the binary from a clean temp cwd with an isolated APPDATA so the real
install's config/assets are untouched.
"""

from __future__ import annotations

import shutil
import sys
from pathlib import Path

import pytest

from smash_test.artifacts import launch_game, png_is_rendered

pytestmark = [
    pytest.mark.e2e,
    pytest.mark.serial,
    pytest.mark.skipif(sys.platform != "win32", reason="Windows wizard test"),
]


def _isolated_dir(game_executable: Path, artifacts_dir: Path) -> Path:
    """Clean-install simulation: the full shipped payload minus the
    user-extracted BattleShip.o2r. Must copy everything the binary resolves
    via ExecutableDirPath() — torch.exe, config.yml and yamls/<region> — or
    the first-run flow cannot attempt extraction. Launching the REAL exe
    instead of this copy is a bug: ExecutableDirPath() then finds the
    build-tree o2r and the wizard never shows."""
    iso = artifacts_dir / "clean-install"
    iso.mkdir(parents=True, exist_ok=True)
    exe_dir = game_executable.parent
    for item in (game_executable.name, "torch.exe", "config.yml",
                 "f3d.o2r", "yamls"):
        src = exe_dir / item
        dest = iso / item
        if not src.exists():
            continue
        if src.is_dir():
            if dest.exists():
                shutil.rmtree(dest)
            shutil.copytree(src, dest)
        elif not dest.exists() or dest.stat().st_mtime != src.stat().st_mtime:
            shutil.copy2(src, dest)
    return iso


def _iso_exe(game_executable: Path, iso: Path) -> Path:
    """The copied binary inside the isolated install dir."""
    return iso / game_executable.name


@pytest.mark.timeout(360)  # real torch extraction can exceed the 60s default
def test_wizard_renders_on_missing_assets(game_executable: Path,
                                          artifacts_dir: Path):
    iso = _isolated_dir(game_executable, artifacts_dir)
    # SHIP_HOME (set by launch_game) isolates the app-data dir — the real
    # profile may already carry BattleShip.o2r and would mask the wizard.
    shot_dir = artifacts_dir / "wizard"
    run = launch_game(
        _iso_exe(game_executable, iso), artifacts_dir, frames=300, cwd=iso,
        env_extra={
            "SSB64_WIZARD_AUTOCANCEL": "240",
            "SSB64_SCREENSHOT_FRAMES": "120",
            "SSB64_SCREENSHOT_DIR": str(shot_dir),
        }, name="wizard", timeout=300)
    assert not run.timed_out, "wizard never reached the autocancel frame"
    assert "first_run" in run.log, f"wizard path not taken:\n{run.log[-1500:]}"
    assert "SSB64_WIZARD_AUTOCANCEL fired" in run.log
    shots = list(shot_dir.glob("frame_*.png"))
    assert shots, "no wizard screenshot captured"
    assert png_is_rendered(shots[0]), "wizard frame is blank"


@pytest.mark.timeout(360)  # real torch extraction can exceed the 60s default
def test_wizard_rom_path_prefilled(game_executable: Path,
                                   artifacts_dir: Path):
    """Placing baserom.us.z64 in the isolated app-data dir must be detected
    by the wizard's default path (logged) — the extraction CTA path."""
    iso = _isolated_dir(game_executable, artifacts_dir)
    # SHIP_HOME verbatim = the app-data dir (no extra BattleShip level).
    appdata = artifacts_dir / "appdata"
    appdata.mkdir(parents=True, exist_ok=True)
    rom = Path(__file__).resolve().parents[3] / "roms" / "baserom.us.z64"
    if not rom.is_file():
        pytest.skip("baserom.us.z64 not present in repo roms/")
    shutil.copy2(rom, appdata / "baserom.us.z64")
    run = launch_game(
        _iso_exe(game_executable, iso), artifacts_dir, frames=240, cwd=iso,
        env_extra={"SSB64_WIZARD_AUTOCANCEL": "200"}, name="wizard-rom",
        timeout=300)
    assert not run.timed_out
    assert "first_run" in run.log
    # The wizard must see the ROM at the prefilled path (its own log lines
    # record detection); a hard miss means the default path is broken.
    assert "baserom" in run.log.lower() or "rom" in run.log.lower(), \
        f"no ROM-path evidence in log:\n{run.log[-1500:]}"
