#!/usr/bin/env python3
"""
reloc_extract — Standalone SSB64:RELOC file extractor and verifier.

Reads a US or JP baserom directly using the same logic as Torch's RelocFactory,
decompresses VPK0 if needed, and writes the raw decompressed bytes.

Use cases:
  - Extract a single file by file_id
  - Compare against the bytes loaded by the new port (after pass1 BSWAP32)
    to verify Torch's extraction is lossless

Usage:
  reloc_extract.py extract <baserom.z64> <file_id> <output.bin>
  reloc_extract.py diff <baserom.z64> <file_id> <port_dump.bin>
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "code-generation"))
import ssb64_reloc_lib as rl


def extract_file(rom: bytes, file_id: int) -> bytes:
    layout = rl.RelocLayout.for_rom(rom)
    info = rl.read_table_entry(rom, layout, file_id)
    print(
        f"file_id={info['file_id']}  compressed={info['is_compressed']}  "
        f"data_offset=0x{info['data_offset']:X}  "
        f"reloc_intern=0x{info['reloc_intern_offset']:04X}  "
        f"reloc_extern=0x{info['reloc_extern_offset']:04X}  "
        f"comp_bytes={info['compressed_bytes']}  "
        f"decomp_bytes={info['decompressed_bytes']}",
        file=sys.stderr,
    )

    data_rom_addr = layout.data_start + info["data_offset"]
    print(
        f"data_rom_addr=0x{data_rom_addr:X} "
        f"(RELOC_DATA_START=0x{layout.data_start:X} + offset=0x{info['data_offset']:X})",
        file=sys.stderr,
    )

    if data_rom_addr + info["compressed_bytes"] > len(rom):
        raise ValueError("ROM too small for file data")

    file_data = rom[data_rom_addr:data_rom_addr + info["compressed_bytes"]]

    if info["is_compressed"]:
        decompressed = rl.vpk0_decode(file_data)
        if len(decompressed) != info["decompressed_bytes"]:
            print(
                f"WARNING: vpk0 produced {len(decompressed)} bytes, "
                f"table says {info['decompressed_bytes']}",
                file=sys.stderr,
            )
    else:
        decompressed = file_data[:info["decompressed_bytes"]]

    return decompressed


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def cmd_extract(argv):
    if len(argv) != 3:
        print("usage: reloc_extract.py extract <baserom.z64> <file_id> <output.bin>",
              file=sys.stderr)
        return 2

    rom_path = Path(argv[0])
    file_id  = int(argv[1], 0)
    out_path = Path(argv[2])

    rom = rom_path.read_bytes()
    if rom[:4] != bytes([0x80, 0x37, 0x12, 0x40]):
        print(f"warning: ROM header is not z64 ({rom[:4].hex()})", file=sys.stderr)

    data = extract_file(rom, file_id)
    out_path.write_bytes(data)
    print(f"wrote {len(data)} bytes to {out_path}", file=sys.stderr)
    return 0


def cmd_diff(argv):
    if len(argv) != 3:
        print("usage: reloc_extract.py diff <baserom.z64> <file_id> <port_dump.bin>",
              file=sys.stderr)
        return 2

    rom_path  = Path(argv[0])
    file_id   = int(argv[1], 0)
    port_path = Path(argv[2])

    rom = rom_path.read_bytes()
    rom_data = extract_file(rom, file_id)
    port_data = port_path.read_bytes()

    print()
    print(f"ROM-extracted (truth):  {len(rom_data)} bytes")
    print(f"port dump:              {len(port_data)} bytes")

    if len(rom_data) != len(port_data):
        print(f"SIZE MISMATCH: rom={len(rom_data)} port={len(port_data)}")

    cmp_len = min(len(rom_data), len(port_data))
    diffs = []
    for i in range(cmp_len):
        if rom_data[i] != port_data[i]:
            diffs.append(i)

    if not diffs and len(rom_data) == len(port_data):
        print("\nIDENTICAL — no differences found")
        return 0

    print(f"\n{len(diffs)} byte differences in first {cmp_len} bytes")

    # Print first 30 diffs with context
    print("\nFirst 30 differing bytes (offset: rom -> port):")
    for off in diffs[:30]:
        print(f"  0x{off:06X}: 0x{rom_data[off]:02X} -> 0x{port_data[off]:02X}")

    # Find runs (regions of contiguous diffs) for higher-level analysis
    runs = []
    if diffs:
        run_start = diffs[0]
        run_end = diffs[0]
        for d in diffs[1:]:
            if d == run_end + 1:
                run_end = d
            else:
                runs.append((run_start, run_end))
                run_start = d
                run_end = d
        runs.append((run_start, run_end))

    print(f"\n{len(runs)} contiguous diff runs:")
    for start, end in runs[:20]:
        size = end - start + 1
        print(f"  0x{start:06X} .. 0x{end:06X}  ({size} bytes)")
        # Hex dump of the run, both sides
        hex_rom  = " ".join(f"{b:02X}" for b in rom_data[start:end + 1])
        hex_port = " ".join(f"{b:02X}" for b in port_data[start:end + 1])
        print(f"    rom : {hex_rom}")
        print(f"    port: {hex_port}")

    return 1


def main(argv):
    if len(argv) < 1:
        print(__doc__)
        return 2
    cmd = argv[0]
    if cmd == "extract":
        return cmd_extract(argv[1:])
    if cmd == "diff":
        return cmd_diff(argv[1:])
    print(f"unknown command: {cmd}", file=sys.stderr)
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
