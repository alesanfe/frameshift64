"""Deliverable integrity for the non-Windows platforms: UWP MSIX bundle,
Switch NRO + SD bundle, macOS Mach-O, Linux ELF.

These are structural checks — they prove each artifact is well-formed,
targets the right architecture, and carries the runtime payloads the game
needs. Runtime e2e for each OS lives in the platform's own test path or CI.
Each test skips when the artifact hasn't been built locally.
"""

from __future__ import annotations

import io
import zipfile
from pathlib import Path

import pytest
from smash_test.artifacts import (
    REPO_ROOT,
    elf_machine,
    macho_cputype,
    nro_header_ok,
    zip_names,
)

pytestmark = pytest.mark.e2e

OUT = REPO_ROOT / "out" / "build"


# ── UWP ────────────────────────────────────────────────────────────────

def _find_msixbundle() -> Path | None:
    cands = sorted(
        OUT.glob("uwp-pkg/AppPackages/**/*.msixbundle"),
        key=lambda p: p.stat().st_mtime, reverse=True)
    return cands[0] if cands else None


def test_uwp_msixbundle_structure():
    bundle = _find_msixbundle()
    if bundle is None:
        pytest.skip("no UWP .msixbundle under out/build/uwp-pkg/AppPackages/")
    names = zip_names(bundle)
    inner = [n for n in names if n.endswith(".msix")]
    assert inner, "bundle carries no inner .msix"
    assert "AppxMetadata/AppxBundleManifest.xml" in names
    with zipfile.ZipFile(bundle) as zb:
        msix = zipfile.ZipFile(io.BytesIO(zb.read(inner[0])))
        contents = set(msix.namelist())
        manifest = msix.read("AppxManifest.xml").decode("utf-8",
                                                        errors="replace")
    assert "BattleShip-UWP.exe" in contents
    assert "BattleShip.dll" in contents
    assert "f3d.o2r" in contents
    assert "<Identity" in manifest and "x64" in manifest


# ── Switch ─────────────────────────────────────────────────────────────

def test_switch_nro_and_bundle():
    nro = OUT / "switch" / "BattleShip.nro"
    if not nro.is_file():
        pytest.skip("no Switch build at out/build/switch/")
    assert nro_header_ok(nro), "BattleShip.nro lacks NRO0 magic"
    bundle = OUT / "switch" / "switch_sd" / "switch" / "BattleShip"
    for f in ("BattleShip.nro", "BattleShip.o2r", "f3d.o2r",
              "gamecontrollerdb.txt"):
        assert (bundle / f).is_file(), f"{f} missing from switch_sd bundle"


# ── macOS ──────────────────────────────────────────────────────────────

def test_macos_macho_arm64():
    exe = OUT / "macos" / "BattleShip"
    if not exe.is_file():
        pytest.skip("no macOS build at out/build/macos/")
    cpu = macho_cputype(exe)
    assert cpu == 0x0100000C, f"expected arm64 Mach-O, got cputype {cpu:#x}"
    # Runtime payloads must have been staged next to the binary.
    for f in ("BattleShip.o2r", "f3d.o2r"):
        assert (exe.parent / f).is_file(), f"{f} not staged"


# ── Linux ──────────────────────────────────────────────────────────────

def test_linux_elf_x86_64():
    exe = OUT / "linux" / "BattleShip"
    if not exe.is_file():
        pytest.skip("no Linux build at out/build/linux/")
    assert elf_machine(exe) == 62, "not an x86-64 ELF"
    for f in ("BattleShip.o2r", "f3d.o2r"):
        assert (exe.parent / f).is_file(), f"{f} not staged"
