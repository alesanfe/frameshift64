#!/usr/bin/env python3
"""check_o2r_reproducibility — same ROM + recipes must give identical .o2r.

Runs the Torch extractor twice into temp dirs and compares SHA-256 of the
produced archive(s). The build pins SOURCE_DATE_EPOCH inside CMake so zip
timestamps can't drift; this tool reproduces that pin itself so the check
measures content determinism, not timestamp noise.

Flags:
  --rom PATH        baserom (required — guard-rom passes ROM_PATH)
  --region us|jp    recipe set under assets/game/ (default us)
  --torch PATH      torch executable (default: search build dirs)
  --output PATH     JSON report
  --keep            keep the two output trees for diffing

Torch reads a *staged* recipe dir (config.yml + yamls/<region>/ + lang/),
so this tool stages one via the project's own cmake/StageAssetRecipes.cmake
in script mode — the same code path the build uses.

Exit 0 identical, 1 different, 2 missing inputs.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
EPOCH = "315532800"  # DOS timestamp floor — mirrors the CMake pin


def find_torch() -> Path | None:
    for cand in sorted(ROOT.glob("out/build/*/tools/**/torch*")) + \
                  sorted(ROOT.glob("out/build/*/**/torch*")):
        if cand.is_file() and cand.suffix in (".exe", ""):
            return cand
    exe = shutil.which("torch") or shutil.which("Torch")
    return Path(exe) if exe else None


def stage_recipes(dest: Path, region: str) -> int:
    """Stage config.yml + yamls/<region>/ + lang/ exactly like configure."""
    return subprocess.run(
        ["cmake", f"-DSOURCE_DIR={ROOT}", f"-DDESTINATION_DIR={dest}",
         f"-DVERSION={region}", "-P",
         str(ROOT / "cmake/StageAssetRecipes.cmake")],
        capture_output=True, text=True).returncode


def extract(torch: Path, rom: Path, recipes: Path, dest: Path) -> int:
    env = dict(os.environ, SOURCE_DATE_EPOCH=EPOCH)
    dest.mkdir(parents=True, exist_ok=True)
    r = subprocess.run(
        [str(torch), "o2r", str(rom), "-s", str(recipes), "-d", str(dest)],
        capture_output=True, text=True, env=env)
    if r.returncode != 0:
        print(r.stdout[-2000:] + r.stderr[-2000:], file=sys.stderr)
    return r.returncode


def sha_tree(root: Path) -> dict[str, str]:
    out = {}
    for p in sorted(root.rglob("*")):
        if p.is_file():
            h = hashlib.sha256()
            with p.open("rb") as f:
                while b := f.read(1 << 20):
                    h.update(b)
            out[str(p.relative_to(root)).replace("\\", "/")] = h.hexdigest()
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom", required=True)
    ap.add_argument("--region", default="us")
    ap.add_argument("--torch")
    ap.add_argument("--output")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    rom = Path(args.rom)
    torch = Path(args.torch) if args.torch else find_torch()
    if not rom.exists():
        print(f"error: ROM not found: {rom}", file=sys.stderr)
        return 2
    if not (ROOT / "assets" / "game" / args.region).is_dir():
        print(f"error: no recipes for region '{args.region}' under "
              "assets/game/", file=sys.stderr)
        return 2
    if torch is None or not Path(torch).exists():
        print("error: torch executable not found — pass --torch or build "
              "a preset that generates it", file=sys.stderr)
        return 2

    tmp = Path(tempfile.mkdtemp(prefix="o2r-repro-"))
    try:
        recipes = tmp / "recipes"
        if stage_recipes(recipes, args.region) != 0:
            print("error: recipe staging failed", file=sys.stderr)
            return 2
        a, b = tmp / "run-a", tmp / "run-b"
        if extract(Path(torch), rom, recipes, a) != 0:
            print("error: first extraction failed", file=sys.stderr)
            return 2
        if extract(Path(torch), rom, recipes, b) != 0:
            print("error: second extraction failed", file=sys.stderr)
            return 2
        ha, hb = sha_tree(a), sha_tree(b)
        diff = {k for k in set(ha) | set(hb) if ha.get(k) != hb.get(k)}
        result = {"identical": not diff, "files_a": len(ha),
                  "files_b": len(hb), "differing": sorted(diff),
                  "epoch": EPOCH}
        if args.output:
            rp = Path(args.output)
            rp.parent.mkdir(parents=True, exist_ok=True)
            rp.write_text(json.dumps(result, indent=2) + "\n",
                          encoding="utf-8")
        if diff:
            print("o2r NOT reproducible — differing members:")
            for d in sorted(diff):
                print(f"  {d}")
            return 1
        print(f"o2r reproducible: {len(ha)} files identical "
              f"(SOURCE_DATE_EPOCH={EPOCH})")
        return 0
    finally:
        if not args.keep:
            shutil.rmtree(tmp, ignore_errors=True)
        else:
            print(f"kept comparison trees at {tmp}")


if __name__ == "__main__":
    sys.exit(main())
