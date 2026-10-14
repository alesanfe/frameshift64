"""pytest wrappers for the standalone e2e netplay drivers.

The drivers in this directory are the real implementation (shared
harness: tests/harness/netplay/session_runner.py). These wrappers give
them pytest collection, markers, JUnit reporting and artifact capture —
the same coverage CI and local runs had through the .ps1 wrappers.

Each driver needs a built BattleShip binary (autodetected or
--build-dir) and keeps the historical exit-code contract:
0 pass / 1 desync or unexpected / 2 harness error.
"""

from __future__ import annotations

import shutil
import sys
from pathlib import Path

import pytest

from smash_test.processes import run_script  # noqa: E402

HERE = Path(__file__).resolve().parent
FIXTURE_REPLAYS = Path(__file__).resolve().parents[2] / "fixtures" / "replays"
_WINDOWS = sys.platform == "win32"

# name -> (script, extra args, needs_powershell)
DRIVERS = {
    "localtest": ("localtest_test.py", ["--seconds", "30"], False),
    "lan": ("lan_test.py", ["--players", "2", "--seconds", "30"], False),
    "lobbytest": ("lobbytest_test.py", ["--seconds", "40"], False),
    "lobbytest_multi": ("lobbytest_multi_test.py",
                        ["--players", "3", "--seconds", "40"], False),
    "compat": ("compat_test.py", [], False),
    "spectate": ("spectate_test.py",
                 ["--players", "2", "--seconds", "40"], False),
    # soak + mutation_suite shell out to rollback_synctest.ps1 → Windows only
    "soak": ("soak_test.py",
             ["--rematch-rounds", "2", "--seconds", "30",
              "--synctest-minutes", "0"], True),
    "mutation_suite": ("mutation_suite_test.py",
                       ["--minutes", "1", "--seconds", "30"], True),
}


@pytest.mark.e2e
@pytest.mark.netplay
@pytest.mark.serial
@pytest.mark.slow
@pytest.mark.timeout(900)
@pytest.mark.parametrize("name", sorted(DRIVERS))
def test_netplay_driver(name, build_dir, artifacts_dir):
    script, extra, needs_ps = DRIVERS[name]
    if needs_ps and not _WINDOWS:
        pytest.skip(f"{name} drives rollback_synctest.ps1 (Windows only)")
    result = run_script(
        HERE / script,
        *extra,
        "--build-dir", str(build_dir),
        "--out-dir", str(artifacts_dir / name),
        timeout=800,
    )
    (artifacts_dir / f"{name}.log").write_text(result.stdout)
    assert result.returncode == 0, result.stdout[-3000:]


@pytest.mark.e2e
@pytest.mark.netplay
@pytest.mark.serial
@pytest.mark.slow
@pytest.mark.timeout(300)
def test_replay_takeover(build_dir, artifacts_dir):
    replays = sorted(FIXTURE_REPLAYS.glob("*.ssr")) + \
        sorted(FIXTURE_REPLAYS.glob("*.ssbreplay"))
    if not replays:
        pytest.skip("no replay fixture under tests/fixtures/replays/")
    result = run_script(
        HERE / "replay_takeover_test.py",
        "--replay", str(replays[0]),
        "--takeover", "1:100",
        "--seconds", "20",
        "--build-dir", str(build_dir),
        "--out-dir", str(artifacts_dir / "takeover"),
        timeout=240,
    )
    (artifacts_dir / "replay_takeover.log").write_text(result.stdout)
    assert result.returncode == 0, result.stdout[-3000:]
