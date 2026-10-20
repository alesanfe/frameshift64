#!/usr/bin/env python3
"""generate_report — human-readable report of the public port_* ABI surface.

Combines:
  * PORT_ABI version constants (port_api_version.h)
  * declared public headers (config/architecture/public-api.yml)
  * baseline exports (config/abi/exports.json) or live-dumped symbols when
    --binary is given

Output: Markdown to --output (or stdout).

Exit 0 on success, 2 on missing required inputs.
"""
import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
VERSION_H = ROOT / "src/game/engine/port_api/port_api_version.h"
MANIFEST = ROOT / "config/architecture/public-api.yml"
EXPORTS = ROOT / "config/abi/exports.json"

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_exports import dump_symbols, filter_prefix  # noqa: E402


def version() -> str:
    text = VERSION_H.read_text(encoding="utf-8")
    major = re.search(r"PORT_ABI_VERSION_MAJOR\s+(\d+)", text).group(1)
    minor = re.search(r"PORT_ABI_VERSION_MINOR\s+(\d+)", text).group(1)
    return f"{major}.{minor} (0x{(int(major) << 8) | int(minor):04x})"


def public_headers() -> list[str]:
    headers = []
    if not MANIFEST.exists():
        return headers
    in_public = False
    for raw in MANIFEST.read_text(encoding="utf-8").splitlines():
        line = raw.split("#", 1)[0].rstrip()
        if re.match(r"^public:", line):
            in_public = True
            continue
        if re.match(r"^\w+:", line):
            in_public = False
        if in_public:
            m = re.match(r"^\s*-\s*(\S+)", line)
            if m:
                headers.append(m.group(1))
    return headers


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--output")
    ap.add_argument("--binary")
    args = ap.parse_args()

    symbols = None
    source = "baseline"
    if args.binary:
        dumped, tool = dump_symbols(Path(args.binary))
        if dumped is None:
            print("error: no symbol dumper available", file=sys.stderr)
            return 2
        symbols = filter_prefix(dumped, ("port_", "Port"))
        source = f"live binary ({tool})"
    elif EXPORTS.exists():
        symbols = json.loads(EXPORTS.read_text(encoding="utf-8"))["symbols"]

    lines = [
        "# port_* ABI report",
        "",
        f"- **Version:** {version()}",
        f"- **Symbol source:** {source}",
        "",
        "## Public headers",
        "",
    ]
    for h in public_headers():
        lines.append(f"- `{h}`")
    if symbols is not None:
        lines += ["", "## Exported `port_*` symbols", ""]
        for s in symbols:
            lines.append(f"- `{s}`")
    else:
        lines += ["", "_No exports baseline yet — run "
                  "`ALLOW_UPDATE=1 make abi-baseline` after a build._"]
    text = "\n".join(lines) + "\n"

    if args.output:
        out = Path(args.output)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(text, encoding="utf-8")
        print(f"wrote {out}")
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
