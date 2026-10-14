"""pytest wrapper for gliden_convert_test.py."""

from __future__ import annotations

from pathlib import Path

import pytest

from smash_test.processes import run_script  # noqa: E402


@pytest.mark.integration
@pytest.mark.modding
def test_gliden_convert(artifacts_dir):
    script = Path(__file__).resolve().parent / "gliden_convert_test.py"
    result = run_script(script, timeout=120)
    (artifacts_dir / "gliden_convert.log").write_text(result.stdout)
    assert result.returncode == 0, result.stdout[-3000:]
