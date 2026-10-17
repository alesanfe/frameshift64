"""
Unit test for tools/developer/modding/package_mod.py — the driver that turns a mod
source folder (manifest.json + src/*.c + include/) into the packaged
layout the runtime loader mounts (manifest + build.gen + dist/*.c).

Run:  python tests/integration/modding/mod_package_test.py
"""

import json
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
PACKAGE_MOD = REPO_ROOT / "tools" / "developer" / "modding" / "package_mod.py"


def make_fixture_mod(mod_dir: Path) -> None:
    """Minimal mod: manifest + one .c that includes a local header."""
    (mod_dir / "src").mkdir(parents=True)
    (mod_dir / "include").mkdir()
    (mod_dir / "manifest.json").write_text(
        json.dumps(
            {
                "name": "Fixture Mod",
                "main": "build.gen",
                "version": "0.0.1",
                "author": "test",
                "license": "MIT",
                "code-version": 1,
            }
        ),
        encoding="utf-8",
    )
    (mod_dir / "include" / "fixture.h").write_text(
        "#define FIXTURE_VALUE 42\n", encoding="utf-8"
    )
    (mod_dir / "src" / "fixture.c").write_text(
        '#include "fixture.h"\nint fixture_value(void) { return FIXTURE_VALUE; }\n',
        encoding="utf-8",
    )
    # .inc.c files are excluded from packaging (generated blobs).
    (mod_dir / "src" / "fixture.inc.c").write_text(
        "unsigned char blob[] = {0};\n", encoding="utf-8"
    )


def run_packager(mod_dir: Path, out_dir: Path) -> subprocess.CompletedProcess:
    return subprocess.run(
        [
            sys.executable, str(PACKAGE_MOD),
            "--mod-dir", str(mod_dir),
            "--out-dir", str(out_dir),
            "--repo-root", str(REPO_ROOT),
        ],
        capture_output=True,
        text=True,
        check=False,
    )


def main() -> int:
    failures = 0

    with tempfile.TemporaryDirectory() as tmp:
        mod_dir = Path(tmp) / "fixture"
        out_dir = Path(tmp) / "out" / "fixture"
        make_fixture_mod(mod_dir)

        result = run_packager(mod_dir, out_dir)
        if result.returncode != 0:
            print(f"FAIL: packager exited {result.returncode}: {result.stderr}")
            return 1

        checks = [
            ("manifest.json copied", (out_dir / "manifest.json").is_file()),
            ("build.gen written", (out_dir / "build.gen").is_file()),
            ("dist source produced", (out_dir / "dist" / "fixture.c").is_file()),
            ("include copied", (out_dir / "include" / "fixture.h").is_file()),
            (
                ".inc.c excluded",
                not (out_dir / "dist" / "fixture.inc.c").exists(),
            ),
        ]

        build_gen = (out_dir / "build.gen").read_text(encoding="utf-8")
        checks.append(("build.gen lists dist/fixture.c", "dist/fixture.c" in build_gen))

        dist_src = (out_dir / "dist" / "fixture.c").read_text(encoding="utf-8")
        checks.append(("header inlined", "FIXTURE_VALUE 42" in dist_src))

        manifest_out = json.loads((out_dir / "manifest.json").read_text(encoding="utf-8"))
        checks.append(("manifest preserves name", manifest_out["name"] == "Fixture Mod"))

        for label, ok in checks:
            print(f"{'PASS' if ok else 'FAIL'}: {label}")
            if not ok:
                failures += 1

    # Missing manifest must fail loudly, not produce a half package.
    with tempfile.TemporaryDirectory() as tmp:
        bad = Path(tmp) / "bad"
        bad.mkdir()
        result = run_packager(bad, Path(tmp) / "out")
        ok = result.returncode != 0 and "manifest.json" in result.stderr
        print(f"{'PASS' if ok else 'FAIL'}: missing manifest fails loudly")
        if not ok:
            failures += 1

    print(f"\n{'ALL PASS' if failures == 0 else f'{failures} FAILURE(S)'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
