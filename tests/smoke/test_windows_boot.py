"""Windows deliverable smoke: the built BattleShip.exe boots, renders real
frames, shuts down cleanly via SSB64_MAX_FRAMES, and honours the runtime
switches shipped in the release build (headless, safe mode, screenshots,
menu overlay).

These are process-level E2E checks on the artifact itself — no mocks.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

from smash_test.artifacts import (
    GameRun, assert_clean_boot, launch_game, png_is_rendered)

pytestmark = [
    pytest.mark.smoke,
    pytest.mark.e2e,
    pytest.mark.serial,          # one GPU window at a time
    pytest.mark.skipif(sys.platform != "win32", reason="Windows binary test"),
]


def _cfg_seed(artifacts: Path, **cvars) -> Path:
    """Pre-seed the isolated APPDATA config (portable-exe layout also read)."""
    import json
    cfg_dir = artifacts / "appdata" / "BattleShip"
    cfg_dir.mkdir(parents=True, exist_ok=True)
    cfg = {"CVars": {k: v for k, v in cvars.items()},
           "Window": {"Width": 960, "Height": 540,
                      "Fullscreen": {"Enabled": False}}}
    path = cfg_dir / "BattleShip.cfg.json"
    path.write_text(json.dumps(cfg, indent=2), encoding="utf-8")
    return path


def test_boot_clean_exit(game_executable: Path, artifacts_dir: Path):
    run = launch_game(game_executable, artifacts_dir, frames=240,
                      name="boot")
    assert_clean_boot(run)


def test_headless_mode(game_executable: Path, artifacts_dir: Path):
    """SSB64_HEADLESS exercises the sim path with no video submission."""
    run = launch_game(game_executable, artifacts_dir, frames=180,
                      env_extra={"SSB64_HEADLESS": "1"}, name="headless")
    assert_clean_boot(run)
    assert "headless mode" in run.log


def test_safe_mode(game_executable: Path, artifacts_dir: Path):
    run = launch_game(game_executable, artifacts_dir, frames=180,
                      env_extra={"SSB64_SAFE_MODE": "1"}, name="safemode")
    assert_clean_boot(run)


@pytest.mark.timeout(300)
def test_screenshot_capture(game_executable: Path, artifacts_dir: Path):
    """A rendered frame lands on disk and is not a flat colour."""
    shot_dir = artifacts_dir / "shots"
    run = launch_game(game_executable, artifacts_dir, frames=400,
                      env_extra={
                          "SSB64_SCREENSHOT_FRAMES": "300",
                          "SSB64_SCREENSHOT_DIR": str(shot_dir),
                      }, name="screenshot", timeout=300)
    assert_clean_boot(run)
    shots = list(shot_dir.glob("frame_*.png"))
    assert shots, "no screenshot written"
    assert png_is_rendered(shots[0]), f"{shots[0]} looks like a blank frame"


@pytest.mark.timeout(300)
def test_menu_overlay(game_executable: Path, artifacts_dir: Path):
    """Seeded gOpenPortMenu=1 must produce a rendered menu frame."""
    _cfg_seed(artifacts_dir, gOpenPortMenu=1)
    shot_dir = artifacts_dir / "menu"
    run = launch_game(game_executable, artifacts_dir, frames=400,
                      env_extra={
                          "SSB64_SCREENSHOT_FRAMES": "300",
                          "SSB64_SCREENSHOT_DIR": str(shot_dir),
                      }, name="menu", timeout=300)
    assert_clean_boot(run)
    shots = list(shot_dir.glob("frame_*.png"))
    assert shots and png_is_rendered(shots[0]), "menu frame missing/blank"


def test_log_jsonl_mirror(game_executable: Path, artifacts_dir: Path):
    """SSB64_LOG_JSON emits parseable structured lines for tooling."""
    import json
    # SSB64_LOG_JSON=1 mirrors to <SSB64_LOG_PATH>.jsonl
    jsonl = artifacts_dir / "jsonlog.log.jsonl"
    run = launch_game(game_executable, artifacts_dir, frames=120,
                      env_extra={"SSB64_LOG_JSON": "1"},
                      name="jsonlog")
    assert_clean_boot(run)
    assert jsonl.is_file(), "jsonl mirror file not produced"
    lines = [ln for ln in
             jsonl.read_text(encoding="utf-8", errors="replace").splitlines()
             if ln.strip()]
    assert lines, "jsonl mirror is empty"
    for ln in lines[:20]:
        rec = json.loads(ln)
        assert {"t", "lvl", "msg"} <= set(rec), f"bad record: {ln[:120]}"
