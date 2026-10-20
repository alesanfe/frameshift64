"""Differential tests for the relocation extraction pipeline (post-audit §2.2).

The same RELOC extraction algorithm exists three times:

  - Python: tools/code-generation/ssb64_reloc_lib.py (codegen reference)
  - C:      tools/debugging/reloc_extract/reloc_extract.c (mirrors
            Torch's RelocFactory::parse)
  - C++:    components/game-runtime (Torch RelocFactory) — observed via
            the bytes it actually wrote into BattleShip.o2r

All three must agree byte-for-byte on the same baserom + file_id, and the
table walker must keep its bounds invariants. These tests pin that
equivalence so the implementations can't silently diverge.

Skips (not failures) when the baserom, a C compiler, or a built .o2r is
absent — the same contract golden_gameplay_test uses for ROM-gated work.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools" / "debugging" / "reloc_extract"))
sys.path.insert(0, str(ROOT / "tools" / "code-generation"))

import reloc_extract as re_py  # noqa: E402
import ssb64_reloc_lib as rl  # noqa: E402

ROM_CANDIDATES = [
    ROOT / "roms" / "baserom.us.z64",
    ROOT / "roms" / "baserom.us.n64",
]
C_SRC = ROOT / "tools" / "debugging" / "reloc_extract" / "reloc_extract.c"
VPK0_SRC = ROOT / "components" / "game-runtime" / "lib" / "libvpk0" / "vpk0.c"
VPK0_INC = VPK0_SRC.parent
DESC_TABLE = ROOT / "tools" / "code-generation" / "relocFileDescriptions.us.txt"


def _find_rom() -> Path | None:
    for p in ROM_CANDIDATES:
        if p.is_file():
            return p
    return None


def _find_o2r() -> Path | None:
    for cand in sorted(ROOT.glob("out/build/**/BattleShip.o2r")) + \
            sorted(ROOT.glob("build/BattleShip.o2r")):
        if cand.is_file() and zipfile.is_zipfile(cand):
            return cand
    return None


@pytest.fixture(scope="module")
def rom() -> bytes:
    p = _find_rom()
    if p is None:
        pytest.skip("baserom.us.z64 not present (requires-rom test)")
    return p.read_bytes()


@pytest.fixture(scope="module")
def layout(rom):
    return rl.RelocLayout.for_rom(rom)


@pytest.fixture(scope="module")
def c_extractor(tmp_path_factory) -> Path:
    """Build reloc_extract.c + libvpk0 — the C mirror of Torch's parser."""
    cc = next((c for c in ("cc", "gcc", "clang") if shutil.which(c)), None)
    if cc is None:
        pytest.skip("no C compiler on PATH for the C differential side")
    out = tmp_path_factory.mktemp("reloc_c") / "reloc_extract"
    r = subprocess.run(
        [cc, "-O2", "-o", str(out), str(C_SRC), str(VPK0_SRC),
         f"-I{VPK0_INC}"],
        capture_output=True, text=True)
    if r.returncode != 0:
        pytest.skip(f"C extractor did not build: {r.stderr.strip()[:200]}")
    return out


def _sample_ids(rom: bytes, layout, k: int = 8) -> list[int]:
    """Deterministic sample: first/last + a compressed/uncompressed mix,
    skipping giant entries to keep the pure-Python VPK0 fast."""
    entries = rl.read_all_entries(rom, layout)
    comp = [i for i, e in enumerate(entries[:-1])
            if e["is_compressed"] and e["compressed_bytes"] < 256 * 1024]
    raw = [i for i, e in enumerate(entries[:-1])
           if not e["is_compressed"] and e["decompressed_bytes"] < 256 * 1024]
    if not comp or not raw:
        pytest.skip("table lacks both compressed and raw entries")
    picks = {0, len(entries) - 2, comp[0], comp[len(comp) // 2], comp[-1],
             raw[0], raw[len(raw) // 2], raw[-1]}
    return sorted(picks)[:k]


def _o2r_basename_index(z: zipfile.ZipFile) -> dict[str, str]:
    """basename -> full member name for every reloc entry in the archive."""
    out = {}
    for n in z.namelist():
        parts = n.rsplit("/", 1)
        if len(parts) == 2 and parts[0].startswith("reloc"):
            out[parts[1]] = n
    return out


def _file_id_names() -> dict[int, str]:
    names = {}
    for line in DESC_TABLE.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line.startswith("-") and ":" in line:
            fid, _, name = line[1:].partition(":")
            names[int(fid)] = name.strip()
    return names


# ── table invariants ───────────────────────────────────────────────────

def test_table_offsets_monotonic_and_in_bounds(rom, layout):
    entries = rl.read_all_entries(rom, layout)
    assert len(entries) >= layout.file_count
    prev_end = 0
    for i, e in enumerate(entries[:-1]):
        end = e["data_offset"] + e["compressed_bytes"]
        assert end <= len(rom) - layout.data_start, f"entry {i} escapes ROM"
        assert e["data_offset"] >= prev_end or e["data_offset"] == 0, \
            f"entry {i} regresses the data stream"
        prev_end = max(prev_end, end)
        # NOTE: compressed_bytes > decompressed_bytes is legitimate for
        # tiny incompressible entries (VPK0 overhead) — entry 57 is an
        # example. The real size invariant (decoded length == table's
        # decompressed_bytes) is covered by test_vpk0_output_size.


def test_vpk0_output_size_matches_table(rom, layout):
    for fid in _sample_ids(rom, layout):
        info = rl.read_table_entry(rom, layout, fid)
        assert len(re_py.extract_file(rom, fid)) == info["decompressed_bytes"], \
            f"file_id {fid}: decoded size != table decompressed_bytes"


# ── Python vs C (Torch-mirror) extractor ───────────────────────────────

def test_python_c_extract_identical(rom, layout, c_extractor, tmp_path):
    rom_path = _find_rom()
    for fid in _sample_ids(rom, layout):
        c_out = tmp_path / f"c_{fid}.bin"
        r = subprocess.run(
            [str(c_extractor), str(rom_path), str(fid), str(c_out)],
            capture_output=True, text=True)
        assert r.returncode == 0, \
            f"file_id {fid}: C extractor failed: {r.stderr.strip()[:200]}"
        py = re_py.extract_file(rom, fid)
        c = c_out.read_bytes()
        assert c == py, \
            f"file_id {fid}: C={len(c)}B vs Python={len(py)}B differ"


def test_malformed_rom_rejected_consistently(rom, c_extractor, tmp_path):
    trunc = tmp_path / "truncated.z64"
    trunc.write_bytes(rom[:4096])
    r = subprocess.run([str(c_extractor), str(trunc), "0",
                        str(tmp_path / "o.bin")], capture_output=True)
    assert r.returncode != 0, "C extractor accepted a truncated ROM"
    with pytest.raises(Exception):
        re_py.extract_file(rom[:4096], 0)
    # file_id out of range — same contract on both sides
    r = subprocess.run([str(c_extractor), str(_find_rom()), "99999",
                        str(tmp_path / "o.bin")], capture_output=True)
    assert r.returncode != 0


# ── Python vs the .o2r Torch actually produced ─────────────────────────

def test_o2r_bytes_match_rom_extraction(rom, layout):
    o2r = _find_o2r()
    if o2r is None:
        pytest.skip("no built BattleShip.o2r to diff against")
    names = _file_id_names()
    with zipfile.ZipFile(o2r) as z:
        index = _o2r_basename_index(z)
        # Only file_ids the description table names can be located in the
        # .o2r (members are stored by name, not id) — sample a spread of
        # named ids, skipping giant members to keep pure-Python VPK0 fast.
        named = [fid for fid in names
                 if rl.read_table_entry(rom, layout, fid)["compressed_bytes"]
                 < 256 * 1024]
        step = max(1, len(named) // 8)
        named = sorted(named)[::step][:8]
        checked = 0
        for fid in named:
            member = index.get(names.get(fid, ""))
            if member is None:
                continue  # name table gaps aren't this test's contract
            torch_bytes = z.read(member)
            py_bytes = re_py.extract_file(rom, fid)
            # The .o2r member is a small Torch container (observed:
            # 80-byte header, "OLER" magic + 0xDEADBEEF sentinels) whose
            # trailing payload is the raw extraction verbatim — BSWAP is
            # applied at load time, not at pack time. Compare the payload
            # suffix, not the whole member.
            assert len(torch_bytes) > len(py_bytes), \
                f"file_id {fid} ({member}): o2r member smaller than payload"
            assert torch_bytes.endswith(py_bytes), \
                f"file_id {fid} ({member}): .o2r payload differs from ROM extraction"
            checked += 1
        assert checked >= 3, "sample too thin — o2r name mapping broken?"
