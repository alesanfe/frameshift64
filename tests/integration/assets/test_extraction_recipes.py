"""pytest wrapper for extraction_recipes_test.py (structure + golden)."""

from __future__ import annotations

from pathlib import Path

import pytest
from smash_test.processes import run_script


@pytest.mark.integration
@pytest.mark.assets
def test_extraction_recipes(artifacts_dir):
    script = Path(__file__).resolve().parent / "extraction_recipes_test.py"
    result = run_script(script, timeout=120)
    (artifacts_dir / "extraction_recipes.log").write_text(result.stdout)
    assert result.returncode == 0, result.stdout[-3000:]
