"""pytest wrappers for the standalone script tests under tests/unit/.

Each script keeps its own CLI contract (0 pass / nonzero fail); the
wrapper gives them pytest collection, markers, JUnit output and
artifact capture without rewriting them.
"""

from __future__ import annotations

import shutil
from pathlib import Path

import pytest
from smash_test.processes import run_script

UNIT = Path(__file__).resolve().parent / "resources"


@pytest.mark.unit
@pytest.mark.requires_rom
def test_extern_ids(require_rom, artifacts_dir):
    """Every reloc file's extern refs must be valid file IDs."""
    result = run_script(UNIT / "extern_ids_test.py", str(require_rom),
                        timeout=120)
    (artifacts_dir / "extern_ids.log").write_text(result.stdout)
    assert result.returncode == 0, result.stdout[-3000:]


@pytest.mark.unit
@pytest.mark.requires_rom
def test_vpk0_decompressor(require_rom, artifacts_dir):
    """VPK0 decompressor round-trips every compressed file in the ROM."""
    if shutil.which("cl") is None and shutil.which("cl.exe") is None \
            and shutil.which("cc") is None and shutil.which("gcc") is None:
        pytest.skip("no C compiler available to build the vpk0 probe")
    result = run_script(UNIT / "vpk0_test.py", str(require_rom),
                        timeout=600)
    (artifacts_dir / "vpk0.log").write_text(result.stdout)
    assert result.returncode == 0, result.stdout[-3000:]


@pytest.mark.unit
def test_ctest_unit(ctest, build_dir, artifacts_dir):
    """Run the CTest-registered native tests labelled `unit`."""
    proc = __import__("subprocess").run(
        [ctest, "--test-dir", str(build_dir), "--label-regex", "unit",
         "--output-on-failure"],
        capture_output=True, text=True, timeout=300)
    (artifacts_dir / "ctest-unit.log").write_text(
        proc.stdout + proc.stderr)
    if proc.returncode != 0 and "No tests were found" in proc.stdout:
        pytest.skip("no native tests registered in this build "
                    "(configure with -DSSB64_BUILD_TESTING=ON)")
    assert proc.returncode == 0, proc.stdout[-3000:]
