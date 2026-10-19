"""
Script 4: Follow internal reloc pointer targets to classify data regions.

Each internal reloc pointer points FROM a struct field TO another location
in the same file. By examining what's at each target, we can classify
the regions the pointers reference (display lists, vertex buffers, textures,
sub-structs, etc.).

Also examines the spatial layout: pointer-containing structs typically live
at the start of the blob, referenced data (DLs, textures) at the end.

Usage: python tools/examine_pointer_targets.py [path-to-baserom.us.z64]
"""

import math
import os
import struct
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "code-generation"))
import ssb64_reloc_lib as rl

# US reloc-table geometry via the shared lib (JP: rl.RelocLayout("jp"))
LAYOUT = rl.RelocLayout("us")

# GFX opcodes
GFX_OPCODES = {
    0x01, 0x03, 0x05, 0x06, 0x07, 0xBF, 0xD7, 0xD9, 0xDA, 0xDB, 0xDC,
    0xDD, 0xDE, 0xDF, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8,
    0xE9, 0xED, 0xEE, 0xEF, 0xF0, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7,
    0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF, 0xBB, 0xB6, 0xB7,
    0xB8, 0xBC, 0xBD, 0xC0, 0xCB, 0xD8,
}


def walk_reloc_chain_with_targets(words, start):
    """Walk reloc chain, return list of (slot_word_idx, target_word_idx)."""
    results = []
    cur = start
    while cur != 0xFFFF and cur < len(words):
        w = words[cur]
        nxt = (w >> 16) & 0xFFFF
        target = w & 0xFFFF
        results.append((cur, target))
        cur = nxt
    return results


def classify_target(words, target_word, num_words):
    """Classify what data starts at a given word offset in the file."""
    if target_word >= num_words:
        return "out_of_bounds"

    # Look at the first few words at the target
    remaining = num_words - target_word
    look = min(remaining, 8)
    sample = [words[target_word + k] for k in range(look)]

    # Check if it starts with a GFX opcode (display list)
    if look >= 2:
        opcode = (sample[0] >> 24) & 0xFF
        if opcode in GFX_OPCODES:
            return "display_list"

    # Check if it looks like vertex data (4 words per Vtx, packed s16 values)
    if look >= 4:
        vtx_like = 0
        for k in range(min(look, 4)):
            w = sample[k]
            hi = (w >> 16) & 0xFFFF
            lo = w & 0xFFFF
            s_hi = hi if hi < 0x8000 else hi - 0x10000
            s_lo = lo if lo < 0x8000 else lo - 0x10000
            if abs(s_hi) < 10000 and abs(s_lo) < 10000:
                vtx_like += 1
        if vtx_like >= 3:
            return "vertex_data"

    # Check if it looks like float data (animation, position data)
    if look >= 2:
        float_count = 0
        for w in sample[:4]:
            if w == 0:
                float_count += 1
            else:
                exp = (w >> 23) & 0xFF
                if 1 <= exp <= 254:
                    try:
                        f = struct.unpack(">f", struct.pack(">I", w))[0]
                        if not math.isnan(f) and 1e-10 < abs(f) < 1e10:
                            float_count += 1
                    except Exception:
                        pass
        if float_count >= 2:
            return "float_struct"

    # Check for a pointer table (words that point to other locations in the file)
    # These would be reloc descriptors before patching, but since we read pre-reloc data,
    # they look like {u16 next, u16 target} packed in u32
    if look >= 2:
        ptr_table_like = 0
        for w in sample[:4]:
            target_part = w & 0xFFFF
            if target_part < num_words:
                ptr_table_like += 1
        if ptr_table_like >= 3:
            return "pointer_table"

    # Check for zero-heavy region (padding or BSS-like)
    if look >= 4:
        zero_count = sum(1 for w in sample[:4] if w == 0)
        if zero_count >= 3:
            return "zero_region"

    # Check for small integer values (enum tables, ID lists, etc.)
    if look >= 2:
        small_count = sum(1 for w in sample[:4] if w <= 0xFFFF)
        if small_count >= 3:
            return "small_int_table"

    # Default: opaque binary data (likely texture/palette pixels)
    return "opaque_binary"


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    rom_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(repo_root, "baserom.us.z64")

    if not os.path.exists(rom_path):
        print(f"Error: ROM not found at {rom_path}")
        return 1

    rom = rl.load_rom(rom_path)


    # Track target classifications
    target_types = Counter()
    total_targets = 0

    # Track the spatial distribution:
    # What fraction of the file is "header" (before first target) vs "body" (targets onward)
    header_fracs = []

    # Track how many distinct target offsets each file has
    target_counts = Counter()

    print(f"Following reloc pointer targets in {LAYOUT.file_count} files...\n")

    for fid, e, data, words in rl.iter_file_words(rom, LAYOUT):
        num_words = len(words)

        # Get intern reloc targets
        targets = []
        if e["reloc_intern_offset"] != 0xFFFF:
            pairs = walk_reloc_chain_with_targets(words, e["reloc_intern_offset"])
            targets = [t for _, t in pairs]

        if not targets:
            continue

        # Classify each unique target
        unique_targets = sorted(set(targets))
        target_counts[len(unique_targets)] += 1

        for t in unique_targets:
            cls = classify_target(words, t, num_words)
            target_types[cls] += 1
            total_targets += 1

        # Spatial: what's the earliest target (= start of "body" data)
        if unique_targets:
            earliest_target = min(unique_targets)
            header_frac = earliest_target / num_words * 100 if num_words > 0 else 0
            header_fracs.append(header_frac)

    # Results
    print("=" * 65)
    print("POINTER TARGET ANALYSIS")
    print("=" * 65)
    print(f"Total unique pointer targets examined: {total_targets:,}\n")

    print("What do internal reloc pointers point to?")
    for cls, count in target_types.most_common():
        pct = count / total_targets * 100
        print(f"  {cls:>20s}: {count:>6,}  ({pct:5.1f}%)")

    print("\nDistinct target count per file:")
    for count in sorted(target_counts.keys()):
        num_files = target_counts[count]
        if num_files > 5 or count <= 5:
            print(f"  {count:>4d} targets: {num_files:>4d} files")

    if header_fracs:
        avg_header = sum(header_fracs) / len(header_fracs)
        print("\nSpatial layout (header = struct data before first pointer target):")
        print(f"  Average header fraction: {avg_header:.1f}% of file")

        # Distribution
        buckets = Counter()
        for frac in header_fracs:
            if frac < 5:
                buckets["<5%"] += 1
            elif frac < 10:
                buckets["5-10%"] += 1
            elif frac < 20:
                buckets["10-20%"] += 1
            elif frac < 50:
                buckets["20-50%"] += 1
            else:
                buckets["50%+"] += 1

        for bucket in ["<5%", "5-10%", "10-20%", "20-50%", "50%+"]:
            count = buckets.get(bucket, 0)
            print(f"    {bucket:>8s}: {count:>4d} files")

    print(f"\n{'=' * 65}")
    print("KEY INSIGHT")
    print(f"{'=' * 65}")

    dl_count = target_types.get("display_list", 0)
    vtx_count = target_types.get("vertex_data", 0)
    float_count = target_types.get("float_struct", 0)
    opaque_count = target_types.get("opaque_binary", 0)
    ptr_table_count = target_types.get("pointer_table", 0)

    print(f"""
  The internal reloc pointers primarily target:
  - Display lists:  {dl_count} targets ({dl_count/total_targets*100:.1f}%)
  - Vertex data:    {vtx_count} targets ({vtx_count/total_targets*100:.1f}%)
  - Float structs:  {float_count} targets ({float_count/total_targets*100:.1f}%)
  - Opaque binary:  {opaque_count} targets ({opaque_count/total_targets*100:.1f}%)
  - Pointer tables: {ptr_table_count} targets ({ptr_table_count/total_targets*100:.1f}%)

  This means the file layout is typically:
    [struct header with pointer fields] -> [DL commands] [vertices] [textures] [more structs]
    ^-- small, needs u32/u16 swap         ^-- bulk data with known swap rules
""")

    return 0


if __name__ == "__main__":
    sys.exit(main())
