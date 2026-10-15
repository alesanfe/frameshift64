"""Process helpers: run standalone test scripts and game binaries."""

from __future__ import annotations

import os
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


@dataclass
class ScriptResult:
    returncode: int
    stdout: str


def run_script(script: Path, *args: str, timeout: int = 300,
               env_extra: dict[str, str] | None = None,
               cwd: Path | None = None) -> ScriptResult:
    """Run a standalone `*_test.py` driver as a subprocess.

    Returns the exit code plus captured output so the pytest wrapper can
    assert on the same contract the .ps1 wrappers used (0 pass / 1 fail
    / 2 harness error).
    """
    env = os.environ.copy()
    if env_extra:
        env.update(env_extra)
    proc = subprocess.run(
        [sys.executable, str(script), *args],
        capture_output=True, text=True, timeout=timeout,
        env=env, cwd=cwd or script.parent, check=False)
    return ScriptResult(proc.returncode, proc.stdout + proc.stderr)


def spawn(binary: Path, args: list[str], env_extra: dict[str, str],
          log_file: Path, cwd: Path | None = None) -> subprocess.Popen:
    """Spawn a game instance with its log redirected to a file."""
    env = os.environ.copy()
    env.update(env_extra)
    out = log_file.open("w", encoding="utf-8", errors="replace")
    return subprocess.Popen(
        [str(binary), *args], cwd=cwd or binary.parent,
        env=env, stdout=out, stderr=subprocess.STDOUT,
    )
