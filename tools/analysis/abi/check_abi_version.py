#!/usr/bin/env python3
"""check_abi_version — validate the port_* ABI version block.

Reads src/game/engine/port_api/port_api_version.h and verifies:
  * PORT_ABI_VERSION_MAJOR / _MINOR are defined as plain integers;
  * PORT_ABI_CURRENT composes them as (major << 8) | minor;
  * port_abi_version() exists and returns PORT_ABI_CURRENT;
  * every public header of the port ABI is reachable — the header list comes
    from config/architecture/public-api.yml (`public:` entries that live
    under src/game/engine/port_api/).

Exit 0 clean, 1 on violations. Prints the resolved version.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
VERSION_H = ROOT / "src/game/engine/port_api/port_api_version.h"
IMPL = ROOT / "src/game/engine/port_api/port_api.cpp"
MANIFEST = ROOT / "config/architecture/public-api.yml"


def parse_version(path: Path) -> tuple[int, int]:
    text = path.read_text(encoding="utf-8")
    major = re.search(r"#define\s+PORT_ABI_VERSION_MAJOR\s+(\d+)", text)
    minor = re.search(r"#define\s+PORT_ABI_VERSION_MINOR\s+(\d+)", text)
    if not major or not minor:
        raise SystemExit(f"error: {path}: PORT_ABI_VERSION_MAJOR/_MINOR missing")
    return int(major.group(1)), int(minor.group(1))


def main() -> int:
    problems = []
    major, minor = parse_version(VERSION_H)
    current = (major << 8) | minor

    text = VERSION_H.read_text(encoding="utf-8")
    if not re.search(r"#define\s+PORT_ABI_CURRENT\s*\\\s*\n?\s*"
                     r"\(\s*\(PORT_ABI_VERSION_MAJOR\s*<<\s*8\s*\)\s*"
                     r"\|\s*PORT_ABI_VERSION_MINOR\s*\)", text):
        problems.append("PORT_ABI_CURRENT is not (MAJOR<<8)|MINOR")

    impl = IMPL.read_text(encoding="utf-8")
    if not re.search(r"unsigned\s+int\s+port_abi_version\s*\(\s*void\s*\)", impl):
        problems.append("port_abi_version() definition missing")
    if "PORT_ABI_CURRENT" not in impl:
        problems.append("port_abi_version() does not return PORT_ABI_CURRENT")

    if MANIFEST.exists():
        api_dir = "src/game/engine/port_api/"
        in_public = False
        declared = []
        for raw in MANIFEST.read_text(encoding="utf-8").splitlines():
            line = raw.split("#", 1)[0].rstrip()
            if re.match(r"^public:", line):
                in_public = True
                continue
            if re.match(r"^\w+:", line):
                in_public = False
            if in_public:
                m = re.match(r"^\s*-\s*(\S+)", line)
                if m and m.group(1).startswith(api_dir):
                    declared.append(m.group(1))
        for h in declared:
            if not (ROOT / h).exists():
                problems.append(f"declared public header missing: {h}")
    else:
        problems.append(f"manifest missing: {MANIFEST}")

    if problems:
        print("ABI version check FAILED:")
        for p in problems:
            print(f"  {p}")
        return 1
    print(f"port ABI version {major}.{minor} "
          f"(PORT_ABI_CURRENT=0x{current:04x}) OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
