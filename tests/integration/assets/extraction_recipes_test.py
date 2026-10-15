#!/usr/bin/env python3
"""
test_extraction_recipes.py — Structural + golden-hash validation for the
checked-in Torch extraction recipes under assets/recipes/.

TD-006: the generated recipes are the contract between the ROM pipeline
(generate_yamls.py / generate_fighter_slices.py) and torch; a silent edit or
regeneration drift ships broken or corrupted assets. This test:

  1. Parses every .yml recipe and validates its structure:
       - `:config:` present and a mapping
       - every asset entry is a mapping with a string `type`, `symbol`
         equal to its key, and integer `offset`/`size` fields
       - `size` > 0, `offset` >= 0
       - ROM-relative recipes (no `ssb64_reloc_parent`): offset+size within
         a generous ROM bound (64 MiB)
       - slice-map recipes (`ssb64_reloc_parent` set): offsets stay inside a
         bundle-sized bound (8 MiB)
       - `file_id` (when present) unique per region and inside the reloc
         file-table size for that region
       - `symbol` unique across the region
       - overlapping (offset, size) ranges within one recipe → warning
  2. Golden manifest: `tests/support/golden/extraction_recipes.sha256` records the
     sha256 of every recipe file. Compared by default; `--update` rewrites it
     after an intentional recipe change. Missing/extra/changed files fail.

Usage:
    python tests/integration/assets/extraction_recipes_test.py            # validate + compare golden
    python tests/integration/assets/extraction_recipes_test.py --update   # regenerate golden manifest
    python tests/integration/assets/extraction_recipes_test.py --no-golden# structure only
"""

import argparse
import hashlib
import itertools
import sys
from pathlib import Path

try:
    import yaml
except ImportError:
    print("PyYAML required: pip install pyyaml", file=sys.stderr)
    sys.exit(2)

REPO_ROOT = Path(__file__).resolve().parents[3]
EXTRACTION_DIR = REPO_ROOT / "assets" / "recipes"
GOLDEN_PATH = REPO_ROOT / "tests" / "support" / "golden" / "extraction_recipes.sha256"

# Generous ceilings — SSB64 ROMs are 32 MiB; these bounds catch garbage
# offsets without constraining legitimate recipes.
ROM_OFFSET_BOUND = 0x04000000
BUNDLE_OFFSET_BOUND = 0x00800000

# file_id validity bound per region (reloc file-table entry count, matching
# generate_yamls.py VERSIONS).
FILE_ID_COUNT = {"us": 2132, "jp": 2107}


def recipe_files():
    if not EXTRACTION_DIR.is_dir():
        return []
    return sorted(EXTRACTION_DIR.rglob("*.yml"))


def validate_recipe(path, region, symbols, file_ids):
    """Returns (failures, warnings) counts for one recipe file."""
    failures = 0
    warnings = 0
    rel = path.relative_to(REPO_ROOT).as_posix()

    try:
        doc = yaml.safe_load(path.read_text(encoding="utf-8"))
    except yaml.YAMLError as e:
        print(f"  [FAIL] {rel}: YAML parse error: {e}")
        return 1, 0

    if not isinstance(doc, dict):
        print(f"  [FAIL] {rel}: top-level document is not a mapping")
        return 1, 0

    # The source convention writes ':config:' — YAML parses that as key
    # ':config' (trailing colon is the mapping separator).
    config = doc.get(":config", doc.get(":config:"))
    if not isinstance(config, dict):
        print(f"  [FAIL] {rel}: missing/non-mapping ':config:'")
        return 1, 0

    slice_map = "ssb64_reloc_parent" in config
    offset_bound = BUNDLE_OFFSET_BOUND if slice_map else ROM_OFFSET_BOUND

    ranges = []
    for key, entry in doc.items():
        if key in (":config", ":config:"):
            continue
        if not isinstance(entry, dict):
            print(f"  [FAIL] {rel}: entry '{key}' is not a mapping")
            failures += 1
            continue

        typ = entry.get("type")
        if not isinstance(typ, str) or not typ:
            print(f"  [FAIL] {rel}: '{key}' missing string 'type'")
            failures += 1

        if entry.get("symbol") != key:
            print(f"  [FAIL] {rel}: '{key}' symbol mismatch "
                  f"({entry.get('symbol')!r})")
            failures += 1

        offset = entry.get("offset")
        size = entry.get("size")
        if not isinstance(offset, int) or isinstance(offset, bool) or offset < 0:
            print(f"  [FAIL] {rel}: '{key}' bad offset {offset!r}")
            failures += 1
            continue
        if not isinstance(size, int) or isinstance(size, bool) or size <= 0:
            print(f"  [FAIL] {rel}: '{key}' bad size {size!r}")
            failures += 1
            continue
        if offset + size > offset_bound:
            bound_kind = "bundle" if slice_map else "ROM"
            print(f"  [FAIL] {rel}: '{key}' offset+size 0x{offset + size:X} "
                  f"exceeds {bound_kind} bound 0x{offset_bound:X}")
            failures += 1

        fid = entry.get("file_id")
        if fid is not None:
            if (not isinstance(fid, int) or isinstance(fid, bool) or fid < 0
                    or fid >= FILE_ID_COUNT.get(region, 1 << 30)):
                print(f"  [FAIL] {rel}: '{key}' bad file_id {fid!r}")
                failures += 1
            elif fid in file_ids[region]:
                print(f"  [FAIL] {rel}: '{key}' duplicate file_id {fid} "
                      f"(first in {file_ids[region][fid]})")
                failures += 1
            else:
                file_ids[region][fid] = rel

        # Symbol/file_id uniqueness is per-region — us and jp legitimately
        # name the same assets (same game, two ROMs).
        if key in symbols[region]:
            print(f"  [WARN] {rel}: symbol '{key}' also in {symbols[region][key]}")
            warnings += 1
        else:
            symbols[region][key] = rel

        ranges.append((offset, offset + size, key))

    # Overlap detection within a single recipe (slices should tile, not
    # overlap; ROM recipes shouldn't alias).
    ranges.sort()
    for (_, prev_end, prev_key), (start, end, key) in itertools.pairwise(ranges):
        if start < prev_end:
            print(f"  [WARN] {rel}: '{key}' [0x{start:X},0x{end:X}) overlaps "
                  f"'{prev_key}' (ends 0x{prev_end:X})")
            warnings += 1

    return failures, warnings


def sha256_of(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def golden_compare(files):
    """Compare recorded hashes. Returns failure count."""
    if not GOLDEN_PATH.is_file():
        print(f"  [WARN] golden manifest {GOLDEN_PATH} not found — run "
              f"with --update to create it")
        return 0

    recorded = {}
    for line in GOLDEN_PATH.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        digest, _, rel = line.partition("  ")
        recorded[rel] = digest

    failures = 0
    seen = set()
    for f in files:
        rel = f.relative_to(REPO_ROOT).as_posix()
        seen.add(rel)
        digest = sha256_of(f)
        if rel not in recorded:
            print(f"  [FAIL] golden: new recipe {rel} (update the manifest "
                  f"with --update)")
            failures += 1
        elif recorded[rel] != digest:
            print(f"  [FAIL] golden: {rel} content changed "
                  f"({recorded[rel][:12]}… -> {digest[:12]}…)")
            failures += 1

    for rel in sorted(set(recorded) - seen):
        print(f"  [FAIL] golden: recorded recipe {rel} no longer exists")
        failures += 1

    return failures


def golden_update(files):
    lines = [("# sha256 of every extraction recipe — regenerated by\n"
              "#   python tests/integration/assets/extraction_recipes_test.py --update\n"
              "# after an intentional recipe change. TD-006 golden test.\n")]
    for f in files:
        rel = f.relative_to(REPO_ROOT).as_posix()
        lines.append(f"{sha256_of(f)}  {rel}\n")
    GOLDEN_PATH.parent.mkdir(parents=True, exist_ok=True)
    GOLDEN_PATH.write_text("".join(lines), encoding="utf-8")
    print(f"wrote {GOLDEN_PATH} ({len(files)} recipes)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--update", action="store_true",
                    help="rewrite the golden manifest")
    ap.add_argument("--no-golden", action="store_true",
                    help="skip the golden-hash comparison")
    args = ap.parse_args()

    files = recipe_files()
    if not files:
        print(f"  [FAIL] no recipes found under {EXTRACTION_DIR}")
        return 1

    if args.update:
        golden_update(files)

    failures = 0
    warnings = 0
    symbols = {}
    file_ids = {}
    for f in files:
        region = f.relative_to(EXTRACTION_DIR).parts[0]
        symbols.setdefault(region, {})
        file_ids.setdefault(region, {})
        fa, wa = validate_recipe(f, region, symbols, file_ids)
        failures += fa
        warnings += wa

    if not args.no_golden:
        failures += golden_compare(files)

    print("\nExtraction recipe validation:")
    print(f"  Recipe files: {len(files)}")
    print(f"  Symbols per region: "
          f"{', '.join(f'{r}={len(s)}' for r, s in symbols.items())}")
    print(f"  File IDs per region: "
          f"{', '.join(f'{r}={len(s)}' for r, s in file_ids.items())}")
    print(f"  Warnings: {warnings}; failures: {failures}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
