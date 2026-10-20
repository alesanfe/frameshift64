#!/usr/bin/env python3
"""check_exports — compare the binary's exported port_* surface to a baseline.

Dumps the defined global symbols of the built executable (or shared lib) and
diffs the `port_*` subset against config/abi/exports.json.

Dumper selection (first available on PATH):
  Windows  : dumpbin /exports, else llvm-nm, else nm --defined-only
  Linux    : nm -D --defined-only (dynamic) then nm --defined-only
  macOS    : nm -gU

Flags:
  --binary PATH       executable/lib to inspect (required)
  --baseline PATH     JSON baseline (default config/abi/exports.json)
  --report PATH       write a JSON diff report
  --update            overwrite the baseline with current symbols
                      (the Makefile gates this behind ALLOW_UPDATE=1)
  --include PREFIX    additional symbol prefixes to track (repeatable)

Exit 0 clean, 1 on unexpected additions/removals, 2 on tooling failure.
"""
import argparse
import json
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
DEFAULT_BASELINE = ROOT / "config/abi/exports.json"
DEFAULT_PREFIXES = ("port_", "Port", "port")


def run(cmd: list[str]) -> str | None:
    try:
        return subprocess.run(cmd, capture_output=True, text=True,
                              check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return None


def dump_symbols(binary: Path) -> tuple[list[str], str] | tuple[None, None]:
    """Return (symbols, tool). Try several dumpers until one parses."""
    b = str(binary)
    attempts = []

    if shutil.which("dumpbin"):
        attempts.append(("dumpbin", ["dumpbin", "/exports", b],
                         _parse_dumpbin))
    for tool, args, parser in (
        ("llvm-nm", ["llvm-nm", "--defined-only", "--extern-only", b],
         _parse_nm),
        ("nm", ["nm", "--defined-only", "--extern-only", b], _parse_nm),
        ("objdump", ["objdump", "-T", b], _parse_objdump_t),
    ):
        if shutil.which(tool):
            attempts.append((tool, args, parser))

    for name, cmd, parser in attempts:
        out = run(cmd)
        if out:
            syms = parser(out)
            if syms:
                return sorted(set(syms)), name
    # Last resort: parse the PE export table directly — needs no external
    # tools and covers every Windows build (the port_* surface is exported
    # via __declspec(dllexport)).
    try:
        syms = pe_exports(binary)
        if syms:
            return sorted(set(syms)), "pe-export-table"
    except Exception:
        pass
    return None, None


def pe_exports(binary: Path) -> list[str]:
    """Read the PE export directory and return exported symbol names."""
    data = binary.read_bytes()
    if data[:2] != b"MZ":
        return []
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe_off:pe_off + 4] != b"PE\0\0":
        return []
    coff = pe_off + 4
    num_sections = struct.unpack_from("<H", data, coff + 2)[0]
    opt_size = struct.unpack_from("<H", data, coff + 16)[0]
    opt = coff + 20
    magic = struct.unpack_from("<H", data, opt)[0]
    if magic == 0x10B:      # PE32
        export_rva, export_sz = struct.unpack_from("<II", data, opt + 96)
        sec_base = opt + opt_size
    elif magic == 0x20B:    # PE32+
        export_rva, export_sz = struct.unpack_from("<II", data, opt + 112)
        sec_base = opt + opt_size
    else:
        return []
    if not export_rva:
        return []

    def rva2off(rva: int) -> int | None:
        for i in range(num_sections):
            s = sec_base + i * 40
            va, vsz, raw_off = struct.unpack_from("<III", data, s + 12)[0], \
                struct.unpack_from("<I", data, s + 8)[0], \
                struct.unpack_from("<I", data, s + 20)[0]
            if va <= rva < va + max(vsz, 1):
                return raw_off + (rva - va)
        return None

    exp_off = rva2off(export_rva)
    if exp_off is None:
        return []
    (names_rva,) = struct.unpack_from("<I", data, exp_off + 32)
    (num_names,) = struct.unpack_from("<I", data, exp_off + 24)
    names_off = rva2off(names_rva)
    if names_off is None:
        return []
    syms = []
    for i in range(num_names):
        nrva = struct.unpack_from("<I", data, names_off + i * 4)[0]
        noff = rva2off(nrva)
        if noff is None:
            continue
        end = data.index(b"\0", noff)
        syms.append(data[noff:end].decode("ascii", errors="replace"))
    return syms


def _parse_dumpbin(text: str) -> list[str]:
    syms = []
    for line in text.splitlines():
        m = re.match(r"\s*\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)", line)
        if m:
            name = m.group(1).lstrip("_").split("@")[0]
            syms.append(name)
    return syms


def _parse_nm(text: str) -> list[str]:
    syms = []
    for line in text.splitlines():
        m = re.match(r"^(?:[0-9A-Fa-f]+\s+)?[A-Za-z]?\s*?([A-Za-z_]\w*)$", line.strip())
        if m:
            name = m.group(1)
            if not name.startswith("__"):
                syms.append(name)
    return syms


def _parse_objdump_t(text: str) -> list[str]:
    syms = []
    for line in text.splitlines():
        m = re.match(r"^[0-9A-Fa-f]+\s+\S+\s+\S+\s+[0-9A-Fa-f]+\s+(\S+)", line)
        if m:
            syms.append(m.group(1).lstrip("_"))
    return syms


def filter_prefix(symbols: list[str], prefixes: tuple[str, ...]) -> list[str]:
    return sorted(s for s in symbols
                  if any(s.startswith(p) or s.startswith("_" + p)
                         for p in prefixes))


def load_baseline(path: Path) -> list[str] | None:
    if not path.exists():
        return None
    data = json.loads(path.read_text(encoding="utf-8"))
    return sorted(data.get("symbols", []))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--baseline", default=str(DEFAULT_BASELINE))
    ap.add_argument("--report")
    ap.add_argument("--update", action="store_true")
    ap.add_argument("--include", action="append", default=[])
    args = ap.parse_args()

    binary = Path(args.binary)
    if not binary.exists():
        print(f"error: binary not found: {binary}", file=sys.stderr)
        return 2

    dumped, tool = dump_symbols(binary)
    if dumped is None:
        print("error: no symbol dumper available "
              "(need dumpbin, llvm-nm, nm or objdump)", file=sys.stderr)
        return 2

    prefixes = tuple(DEFAULT_PREFIXES) + tuple(args.include)
    current = filter_prefix(dumped, prefixes)
    baseline = load_baseline(Path(args.baseline))

    if args.update:
        path = Path(args.baseline)
        path.parent.mkdir(parents=True, exist_ok=True)
        payload = {
            "symbols": current,
            "prefixes": list(prefixes),
            "tool": tool,
            "note": "Regenerate via: ALLOW_UPDATE=1 make abi-baseline",
        }
        path.write_text(json.dumps(payload, indent=2) + "\n",
                        encoding="utf-8")
        print(f"baseline updated: {path} ({len(current)} symbols via {tool})")
        return 0

    if baseline is None:
        print(f"error: baseline missing: {args.baseline}", file=sys.stderr)
        print("create it intentionally: ALLOW_UPDATE=1 make abi-baseline",
              file=sys.stderr)
        return 2

    added = sorted(set(current) - set(baseline))
    removed = sorted(set(baseline) - set(current))

    diff = {"tool": tool, "binary": str(binary), "added": added,
            "removed": removed}
    if args.report:
        rp = Path(args.report)
        rp.parent.mkdir(parents=True, exist_ok=True)
        rp.write_text(json.dumps(diff, indent=2) + "\n", encoding="utf-8")

    if added or removed:
        print("ABI surface changed:")
        for s in removed:
            print(f"  - {s}")
        for s in added:
            print(f"  + {s}")
        print("if intentional: ALLOW_UPDATE=1 make abi-baseline")
        return 1

    print(f"abi-exports: {len(current)} port_* symbols match baseline "
          f"(via {tool})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
