"""pytest wrapper for mod_package_test.py."""

from __future__ import annotations

from pathlib import Path

import pytest
from smash_test.processes import run_script


@pytest.mark.integration
@pytest.mark.modding
def test_mod_package(artifacts_dir):
    script = Path(__file__).resolve().parent / "mod_package_test.py"
    result = run_script(script, timeout=120)
    (artifacts_dir / "mod_package.log").write_text(result.stdout)
    assert result.returncode == 0, result.stdout[-3000:]
