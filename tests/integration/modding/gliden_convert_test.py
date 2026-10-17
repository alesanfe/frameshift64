"""
Unit test for tools/developer/modding/convert_gliden64_pack.py — the CLI twin of the
in-game GLideN64 converter (src/application/update/downloads/ModPackDownloader.cpp). Both
re-key "<prefix>#<crc8>#<fmt>#<siz>[#<palCRC8>]_<tag>.png" files into the
port's "#<ourHash8>#<fmt>#<siz>.png" grammar using the vendored hash map
(tools/developer/modding/vendor/battleship.json). This test keeps the CLI honest on
the same inputs the in-game path accepts.

Run:  python tests/integration/modding/gliden_convert_test.py
"""

import json
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
CONVERT = REPO_ROOT / "tools" / "developer" / "modding" / "convert_gliden64_pack.py"
MAPPING = REPO_ROOT / "tools" / "developer" / "modding" / "vendor" / "battleship.json"


def run_convert(pack: Path, out: Path) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(CONVERT), "--pack", str(pack), "--out", str(out)],
        capture_output=True,
        text=True,
        check=False,
    )


def main() -> int:
    failures = 0
    mapping = json.loads(MAPPING.read_text(encoding="utf-8"))

    # One 3-field key (no palette CRC) and one 4-field key from the real map.
    k3 = next(k for k in mapping if k.count("#") == 2)
    k4 = next(k for k in mapping if k.count("#") == 3)
    c3, f3, s3 = k3.split("#")
    c4, f4, s4, p4 = k4.split("#")

    name3 = f"SMASH BROTHERS#{c3}#{f3}#{s3}_all.png"
    name4 = f"OTHER PREFIX#{c4}#{f4}#{s4}#{p4}_ciByRGBA.png"
    want3 = f"#{mapping[k3].upper()}#{f3}#{s3}.png"
    want4 = f"#{mapping[k4].upper()}#{f4}#{s4}.png"

    with tempfile.TemporaryDirectory() as tmp:
        pack = Path(tmp) / "pack"
        pack.mkdir()
        (pack / name3).write_bytes(b"png3")
        (pack / name4).write_bytes(b"png4")
        (pack / "random.png").write_bytes(b"x")                       # unparseable
        (pack / "SMASH BROTHERS#DEADBEEF#4#0_all.png").write_bytes(b"x")  # unmapped

        out = Path(tmp) / "out"
        result = run_convert(pack, out)
        checks = [
            ("dir pack converts", result.returncode == 0),
            ("3-field key renamed", (out / want3).is_file()),
            ("4-field palette key renamed", (out / want4).is_file()),
            ("unmapped file skipped", not (out / "SMASH BROTHERS#DEADBEEF#4#0_all.png").exists()),
            ("unparseable file skipped", not (out / "random.png").exists()),
            ("content preserved", (out / want3).read_bytes() == b"png3"),
        ]
        if result.returncode != 0:
            print(f"pack output: {result.stdout}{result.stderr}")
        for label, ok in checks:
            print(f"{'PASS' if ok else 'FAIL'}: {label}")
            if not ok:
                failures += 1

    with tempfile.TemporaryDirectory() as tmp:
        zpath = Path(tmp) / "pack.zip"
        with zipfile.ZipFile(zpath, "w") as z:
            z.writestr("subdir/" + name3, b"png3")
            z.writestr("readme.txt", "hi")
        out = Path(tmp) / "outz"
        result = run_convert(zpath, out)
        checks = [
            ("zip pack converts", result.returncode == 0),
            ("subdir preserved", (out / "subdir" / want3).is_file()),
            ("non-png ignored", not (out / "readme.txt").exists()),
        ]
        if result.returncode != 0:
            print(f"zip output: {result.stdout}{result.stderr}")
        for label, ok in checks:
            print(f"{'PASS' if ok else 'FAIL'}: {label}")
            if not ok:
                failures += 1

    # A pack with zero convertible PNGs must fail, not silently succeed.
    with tempfile.TemporaryDirectory() as tmp:
        pack = Path(tmp) / "empty"
        pack.mkdir()
        (pack / "random.png").write_bytes(b"x")
        result = run_convert(pack, Path(tmp) / "out")
        ok = result.returncode != 0
        print(f"{'PASS' if ok else 'FAIL'}: nothing-converted fails loudly")
        if not ok:
            failures += 1

    print(f"\n{'ALL PASS' if failures == 0 else f'{failures} FAILURE(S)'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
