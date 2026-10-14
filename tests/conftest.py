"""Shared pytest fixtures for the smash-classic suite.

pytest is the unified orchestrator: it runs the Python tests directly,
drives the standalone e2e/netplay drivers under tests/e2e/, and (when a
native build exists) shells out to ctest for the GTest binaries. Coverage
of the Python side comes from pytest-cov; C/C++ coverage comes from an
LLVM-instrumented build (-DSSB64_ENABLE_COVERAGE=ON) merged by
tools/testing/coverage.py.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "out"

# Make the smash_test support package importable from every test module.
sys.path.insert(0, str(ROOT / "tests" / "support" / "python"))

# Search order for the game executable: explicit --build-dir / --executable
# wins; otherwise probe the conventional build trees newest-first.
_DEFAULT_BUILD_GLOBS = (
    "out/build/*/Release",
    "out/build/*/Debug",
    "out/build/*",
    "build",
)


def pytest_addoption(parser: pytest.Parser) -> None:
    group = parser.getgroup("smash-classic")
    group.addoption("--build-dir", type=Path, default=None,
                    help="Directory containing the built BattleShip binary "
                         "(default: autodetect under out/build/)")
    group.addoption("--region", choices=("us", "jp"), default="us",
                    help="Game region used by tests")
    group.addoption("--rom-us", type=Path, default=None,
                    help="Path to a legally supplied US ROM")
    group.addoption("--rom-jp", type=Path, default=None,
                    help="Path to a legally supplied JP ROM")
    group.addoption("--executable", type=Path, default=None,
                    help="Explicit path to the native executable")
    group.addoption("--keep-artifacts", action="store_true",
                    help="Keep artifacts for successful tests")


def pytest_configure(config: pytest.Config) -> None:
    config._smash_artifacts = OUT / "test-results" / "artifacts"
    config._smash_artifacts.mkdir(parents=True, exist_ok=True)


@dataclass(frozen=True)
class TestEnvironment:
    root: Path
    build_dir: Path | None
    region: str
    executable: Path | None
    artifacts: Path
    rom: Path | None


def _find_executable(build_dir: Path) -> Path | None:
    for name in ("BattleShip.exe", "BattleShip",
                 "bin/BattleShip.exe", "bin/BattleShip",
                 "Debug/BattleShip.exe", "Release/BattleShip.exe"):
        candidate = build_dir / name
        if candidate.is_file():
            return candidate
    return None


def _autodetect_build() -> tuple[Path | None, Path | None]:
    """Return (build_dir, executable) from the conventional trees."""
    best: tuple[float, Path, Path] | None = None
    for pattern in _DEFAULT_BUILD_GLOBS:
        for cand in sorted(ROOT.glob(pattern)):
            exe = _find_executable(cand)
            if exe is None:
                continue
            mtime = exe.stat().st_mtime
            if best is None or mtime > best[0]:
                best = (mtime, cand, exe)
    if best is None:
        return None, None
    return best[1], best[2]


@pytest.fixture(scope="session")
def build_dir(pytestconfig: pytest.Config) -> Path:
    explicit = pytestconfig.getoption("--build-dir")
    if explicit is not None:
        path = explicit.resolve()
        if not path.is_dir():
            pytest.fail(f"Build directory does not exist: {path}",
                        pytrace=False)
        return path
    detected, _ = _autodetect_build()
    if detected is None:
        pytest.fail("No --build-dir given and no built BattleShip binary "
                    "found under out/build/", pytrace=False)
    return detected


@pytest.fixture(scope="session")
def game_executable(pytestconfig: pytest.Config,
                    build_dir: Path) -> Path:
    explicit = pytestconfig.getoption("--executable")
    if explicit is not None:
        explicit = explicit.resolve()
        if not explicit.is_file():
            pytest.fail(f"Executable does not exist: {explicit}",
                        pytrace=False)
        return explicit
    exe = _find_executable(build_dir)
    if exe is None:
        pytest.fail(f"BattleShip executable not found under {build_dir}",
                    pytrace=False)
    return exe


@pytest.fixture
def artifacts_dir(request: pytest.FixtureRequest,
                  pytestconfig: pytest.Config):
    safe = request.node.nodeid.replace("/", "_").replace("::", "__")
    path = pytestconfig._smash_artifacts / safe
    if path.exists():
        shutil.rmtree(path)
    path.mkdir(parents=True)
    yield path
    keep = pytestconfig.getoption("--keep-artifacts")
    failed = getattr(request.node, "_smash_failed", False)
    if not keep and not failed:
        shutil.rmtree(path, ignore_errors=True)


@pytest.fixture(scope="session")
def test_environment(pytestconfig: pytest.Config) -> TestEnvironment:
    region = pytestconfig.getoption("--region")
    rom = pytestconfig.getoption(f"--rom-{region}")
    build, exe = _autodetect_build()
    explicit_build = pytestconfig.getoption("--build-dir")
    explicit_exe = pytestconfig.getoption("--executable")
    if explicit_build is not None:
        build = explicit_build.resolve()
        exe = _find_executable(build) or exe
    if explicit_exe is not None:
        exe = explicit_exe.resolve()
    return TestEnvironment(
        root=ROOT,
        build_dir=build,
        region=region,
        executable=exe,
        artifacts=pytestconfig._smash_artifacts,
        rom=rom.resolve() if rom else None,
    )


@pytest.fixture
def require_rom(test_environment: TestEnvironment) -> Path:
    rom = test_environment.rom
    if rom is None:
        # Fall back to the conventional repo-root name before skipping.
        cand = test_environment.root / f"baserom.{test_environment.region}.z64"
        if cand.is_file():
            return cand
        pytest.skip(f"No legal {test_environment.region.upper()} ROM was "
                    "supplied (--rom-us/--rom-jp or baserom.<region>.z64)")
    if not rom.is_file():
        pytest.fail(f"Configured ROM does not exist: {rom}", pytrace=False)
    return rom


@pytest.fixture(scope="session")
def ctest() -> str:
    exe = shutil.which("ctest")
    if exe is None:
        # ctest ships next to cmake; cover shells that only have the latter.
        cmake = shutil.which("cmake")
        if cmake:
            sibling = Path(cmake).with_name(
                "ctest.exe" if sys.platform == "win32" else "ctest")
            if sibling.is_file():
                return str(sibling)
        pytest.skip("ctest not found (no CMake toolchain in PATH)")
    return exe


@pytest.fixture(scope="session")
def native_coverage_env() -> dict[str, str]:
    """Env overlay for spawning instrumented binaries under pytest."""
    raw = OUT / "coverage" / "native" / "raw"
    raw.mkdir(parents=True, exist_ok=True)
    return {"LLVM_PROFILE_FILE": str(raw / "%p-%m.profraw")}


@pytest.hookimpl(wrapper=True)
def pytest_runtest_makereport(item: pytest.Item, call: pytest.CallInfo):
    report = yield
    if report.when in {"setup", "call", "teardown"} and report.failed:
        item._smash_failed = True
    return report
