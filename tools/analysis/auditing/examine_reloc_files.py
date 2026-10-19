"""
Examine all 2132 reloc files to understand their internal structure.

For each file, this script:
1. Decompresses if needed (using vpk0)
2. Walks the internal + external reloc chains (counting pointer slots)
3. Computes the fraction of the file that is pointer slots vs data
4. Identifies what word sizes appear in non-pointer regions

The output is a CSV + summary statistics to help understand what
byte-swapping strategy is needed for the PC port.

Usage: python tools/examine_reloc_files.py [path-to-baserom.us.z64]
"""

import os
import struct
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "code-generation"))

import ssb64_reloc_lib as rl

# US reloc-table geometry via the shared lib (JP: rl.RelocLayout("jp"))
LAYOUT = rl.RelocLayout("us")


def walk_reloc_chain(data_words, start_offset):
    """
    Walk a reloc chain in decompressed file data (as big-endian u32 array).
    Returns list of (word_index, target_word_index) for each pointer slot.
    """
    slots = []
    current = start_offset  # in u32 words

    while current != 0xFFFF and current < len(data_words):
        word = data_words[current]
        next_reloc = (word >> 16) & 0xFFFF
        target_words = word & 0xFFFF
        slots.append({
            "slot_word": current,
            "target_word": target_words,
        })
        current = next_reloc

    return slots


def analyze_file(data_bytes, reloc_intern, reloc_extern):
    """Analyze a decompressed file's structure."""
    if len(data_bytes) == 0:
        return None

    num_words = len(data_bytes) // 4
    # Parse as big-endian u32 words
    data_words = struct.unpack(f">{num_words}I", data_bytes[:num_words * 4])

    # Walk reloc chains
    intern_slots = walk_reloc_chain(data_words, reloc_intern) if reloc_intern != 0xFFFF else []
    extern_slots = walk_reloc_chain(data_words, reloc_extern) if reloc_extern != 0xFFFF else []

    # Mark which words are pointer slots
    pointer_word_indices = set()
    for s in intern_slots:
        pointer_word_indices.add(s["slot_word"])
    for s in extern_slots:
        pointer_word_indices.add(s["slot_word"])

    # Analyze non-pointer words
    # Check for patterns that suggest u16 data (values where high or low 16 bits are zero/small)
    non_ptr_words = []
    for i, w in enumerate(data_words):
        if i not in pointer_word_indices:
            non_ptr_words.append(w)

    return {
        "size_bytes": len(data_bytes),
        "num_words": num_words,
        "intern_ptr_count": len(intern_slots),
        "extern_ptr_count": len(extern_slots),
        "total_ptr_count": len(intern_slots) + len(extern_slots),
        "pointer_word_indices": pointer_word_indices,
        "non_ptr_words": non_ptr_words,
    }


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    rom_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(repo_root, "baserom.us.z64")

    if not os.path.exists(rom_path):
        print(f"Error: ROM not found at {rom_path}")
        return 1

    print(f"Loading ROM: {rom_path}")
    rom = rl.load_rom(rom_path)

    entries = rl.read_all_entries(rom, LAYOUT)
    print(f"Parsed {LAYOUT.file_count} reloc table entries")


    # Analyze all files
    print(f"\nAnalyzing {LAYOUT.file_count} files...\n")

    stats = {
        "total": 0,
        "compressed": 0,
        "uncompressed": 0,
        "skipped": 0,
        "empty": 0,
        "has_intern_reloc": 0,
        "has_extern_reloc": 0,
        "has_any_reloc": 0,
        "no_reloc": 0,
    }

    # Per-file results
    file_results = []

    # Size distribution
    size_buckets = Counter()  # bucket → count
    ptr_density_buckets = Counter()  # "X%" → count

    for fid, e in enumerate(entries):
        decompressed_bytes = e["decompressed_bytes"]

        if decompressed_bytes == 0:
            stats["empty"] += 1
            file_results.append({"id": fid, "status": "empty"})
            continue

        stats["total"] += 1

        try:
            _info, data = rl.extract_file(rom, LAYOUT, fid)
        except ValueError:
            stats["skipped"] += 1
            file_results.append({"id": fid, "status": "decompress_failed"})
            continue
        if e["is_compressed"]:
            stats["compressed"] += 1
        else:
            stats["uncompressed"] += 1

        analysis = analyze_file(data, e["reloc_intern_offset"], e["reloc_extern_offset"])
        if analysis is None:
            file_results.append({"id": fid, "status": "analyze_failed"})
            continue

        has_intern = e["reloc_intern_offset"] != 0xFFFF
        has_extern = e["reloc_extern_offset"] != 0xFFFF

        if has_intern:
            stats["has_intern_reloc"] += 1
        if has_extern:
            stats["has_extern_reloc"] += 1
        if has_intern or has_extern:
            stats["has_any_reloc"] += 1
        else:
            stats["no_reloc"] += 1

        # Size bucket
        if decompressed_bytes < 64:
            bucket = "<64B"
        elif decompressed_bytes < 256:
            bucket = "64-255B"
        elif decompressed_bytes < 1024:
            bucket = "256B-1K"
        elif decompressed_bytes < 4096:
            bucket = "1K-4K"
        elif decompressed_bytes < 16384:
            bucket = "4K-16K"
        elif decompressed_bytes < 65536:
            bucket = "16K-64K"
        else:
            bucket = ">64K"
        size_buckets[bucket] += 1

        # Pointer density
        if analysis["num_words"] > 0:
            density_pct = (analysis["total_ptr_count"] / analysis["num_words"]) * 100
        else:
            density_pct = 0
        density_bucket = f"{int(density_pct // 5) * 5}-{int(density_pct // 5) * 5 + 5}%"
        ptr_density_buckets[density_bucket] += 1

        file_results.append({
            "id": fid,
            "status": "ok",
            "size": decompressed_bytes,
            "compressed": e["is_compressed"],
            "intern_ptrs": analysis["intern_ptr_count"],
            "extern_ptrs": analysis["extern_ptr_count"],
            "total_ptrs": analysis["total_ptr_count"],
            "num_words": analysis["num_words"],
            "density_pct": density_pct,
        })

    # Print summary
    print("=" * 60)
    print("RELOC FILE ANALYSIS SUMMARY")
    print("=" * 60)
    print(f"  Total files:      {LAYOUT.file_count}")
    print(f"  Empty files:      {stats['empty']}")
    print(f"  Compressed:       {stats['compressed']}")
    print(f"  Uncompressed:     {stats['uncompressed']}")
    print(f"  Skipped:          {stats['skipped']}")
    print(f"  Has intern reloc: {stats['has_intern_reloc']}")
    print(f"  Has extern reloc: {stats['has_extern_reloc']}")
    print(f"  Has any reloc:    {stats['has_any_reloc']}")
    print(f"  No reloc at all:  {stats['no_reloc']}")

    print("\nFile size distribution:")
    for bucket in ["<64B", "64-255B", "256B-1K", "1K-4K", "4K-16K", "16K-64K", ">64K"]:
        count = size_buckets.get(bucket, 0)
        print(f"  {bucket:>10s}: {count:4d} files")

    print("\nPointer density distribution (% of words that are pointer slots):")
    for pct in range(0, 105, 5):
        bucket = f"{pct}-{pct+5}%"
        count = ptr_density_buckets.get(bucket, 0)
        if count > 0:
            print(f"  {bucket:>8s}: {count:4d} files")

    # Top 20 largest files
    ok_files = [r for r in file_results if r["status"] == "ok"]
    ok_files_sorted = sorted(ok_files, key=lambda x: x["size"], reverse=True)
    print("\nTop 20 largest files:")
    print(f"  {'ID':>6s}  {'Size':>8s}  {'Comp':>4s}  {'IntPtrs':>7s}  {'ExtPtrs':>7s}  {'Density':>7s}")
    for r in ok_files_sorted[:20]:
        comp = "Y" if r["compressed"] else "N"
        print(f"  {r['id']:>6d}  {r['size']:>8d}  {comp:>4s}  {r['intern_ptrs']:>7d}  {r['extern_ptrs']:>7d}  {r['density_pct']:>6.1f}%")

    # Files with NO reloc (pure data, no pointers)
    no_reloc_files = [r for r in ok_files if r["total_ptrs"] == 0]
    print(f"\nFiles with zero pointer slots: {len(no_reloc_files)}")
    if no_reloc_files:
        total_bytes = sum(r["size"] for r in no_reloc_files)
        print(f"  Total bytes in zero-pointer files: {total_bytes} ({total_bytes/1024:.1f} KB)")

    # Write CSV for further analysis
    csv_path = os.path.join(repo_root, "tools", "reloc_file_analysis.csv")
    with open(csv_path, "w") as f:
        f.write("file_id,status,size_bytes,compressed,intern_ptrs,extern_ptrs,total_ptrs,num_words,density_pct\n")
        for r in file_results:
            if r["status"] == "ok":
                f.write(f"{r['id']},{r['status']},{r['size']},{r['compressed']},"
                        f"{r['intern_ptrs']},{r['extern_ptrs']},{r['total_ptrs']},"
                        f"{r['num_words']},{r['density_pct']:.2f}\n")
            else:
                f.write(f"{r['id']},{r['status']},,,,,,\n")
    print(f"\nCSV written to: {csv_path}")

    # Cleanup

    return 0


if __name__ == "__main__":
    sys.exit(main())
