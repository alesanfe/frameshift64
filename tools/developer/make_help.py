#!/usr/bin/env python3
"""make_help — categorized target catalog for the root Makefile facade.

Parses `target: deps ## [Category] description` lines across the files given
on argv (make passes $(MAKEFILE_LIST)). Targets without a `##` comment are
internal (guards, helpers) and hidden; documented targets without an explicit
`[Category]` fall back to a module-derived category.

Modes:
  --mode common   curated workflow entry points only (the daily driver view)
  --mode all      every documented target, grouped by category
  --category X    only targets in category X
  --format json   machine-readable catalog (writes to --output or stdout)

Exit 0 always — this is a display tool, not a gate.
"""
import argparse
import json
import re
import sys
from collections import OrderedDict
from pathlib import Path

ENTRY_RE = re.compile(
    r"^([a-zA-Z0-9_.\-]+)\s*:[^=]*?##\s*(?:\[([^\]]+)\]\s*)?(.*)$"
)

# Module file → default category when a target has no explicit [Tag].
MODULE_CATEGORY = {
    "Makefile": "Workflow",
    "common.mk": "Diagnostics",
    "dependencies.mk": "Setup",
    "generation.mk": "Build",
    "build.mk": "Build",
    "test.mk": "Test",
    "quality.mk": "Quality",
    "security.mk": "Security",
    "packaging.mk": "Package",
    "release.mk": "Release",
    "netplay.mk": "Netplay",
    "abi.mk": "ABI",
    "decomp.mk": "Decomp",
    "platforms.mk": "Platform",
    "diagnostics.mk": "Diagnostics",
    "reproducibility.mk": "Reproducibility",
    "workflows.mk": "Workflow",
    "help.mk": "Help",
    "makefile.mk": "Makefile",
    "android.mk": "Platform",
    "uwp.mk": "Platform",
}

# Targets shown by `make help` (common mode) — the ~12 daily entry points.
COMMON = [
    "help", "setup", "dev", "build", "run", "test", "test-unit",
    "check", "verify", "verify-fast", "package", "release-dry-run",
    "pre-commit", "pre-push", "clean", "help-all",
]

CATEGORY_ORDER = [
    "Help", "Workflow", "Setup", "Build", "Test", "Netplay", "Sanitizer",
    "Quality", "Security", "ABI", "Decomp", "Platform", "Package",
    "Release", "Reproducibility", "Diagnostics", "Makefile", "Clean",
    "Other",
]


def collect(makefiles: list[str]) -> "OrderedDict[str, list[tuple[str, str]]]":
    """Return {category: [(target, desc)]} preserving declaration order."""
    catalog: "OrderedDict[str, list[tuple[str, str]]]" = OrderedDict()
    seen: set[str] = set()
    for path in makefiles:
        p = Path(path)
        module_cat = MODULE_CATEGORY.get(p.name, "Other")
        try:
            lines = p.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            continue
        # Join backslash-continuations: a rule's ## doc may sit on the last
        # physical line of a multi-line prerequisite list.
        logical = []
        buf = ""
        for raw in lines:
            buf += raw
            if buf.endswith("\\"):
                buf = buf[:-1]
                continue
            logical.append(buf)
            buf = ""
        if buf:
            logical.append(buf)
        for line in logical:
            m = ENTRY_RE.match(line)
            if not m:
                continue
            target, cat, desc = m.group(1), m.group(2) or module_cat, m.group(3).strip()
            if target in seen or target.startswith(".") or target == "help":
                pass
            if target in seen:
                continue
            seen.add(target)
            catalog.setdefault(cat, []).append((target, desc))
    return catalog


def ordered(catalog: "OrderedDict[str, list[tuple[str, str]]]"):
    """Yield categories in CATEGORY_ORDER, unknown ones appended sorted."""
    for cat in CATEGORY_ORDER:
        if cat in catalog:
            yield cat, catalog[cat]
    for cat in sorted(set(catalog) - set(CATEGORY_ORDER)):
        yield cat, catalog[cat]


def render_text(catalog, only_cat=None, common=False):
    width = 0
    rows = []
    for cat, entries in ordered(catalog):
        if only_cat and cat != only_cat:
            continue
        items = [(t, d) for t, d in entries
                 if not common or t in COMMON]
        if not items:
            continue
        width = max(width, *(len(t) for t, _ in items))
        rows.append((cat, items))
    out = []
    for cat, items in rows:
        out.append(f"{cat}:")
        for target, desc in sorted(items):
            out.append(f"  {target:<{width}}  {desc}")
        out.append("")
    return "\n".join(out).rstrip() + "\n"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", choices=["common", "all"], default="common")
    ap.add_argument("--category")
    ap.add_argument("--format", choices=["text", "json"], default="text")
    ap.add_argument("--output")
    ap.add_argument("makefiles", nargs="+")
    args = ap.parse_args()

    catalog = collect(args.makefiles)

    if args.format == "json":
        data = {
            cat: [{"target": t, "description": d} for t, d in items]
            for cat, items in ordered(catalog)
        }
        payload = json.dumps(data, indent=2) + "\n"
        if args.output:
            out = Path(args.output)
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_text(payload, encoding="utf-8")
            print(f"wrote {out}")
        else:
            sys.stdout.write(payload)
        return 0

    text = render_text(catalog, only_cat=args.category,
                       common=(args.mode == "common"))
    if args.output:
        out = Path(args.output)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
