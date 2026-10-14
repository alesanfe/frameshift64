#!/usr/bin/env python3
"""check_docs.py — verify documentation integrity.

Checks, per markdown file under docs/ and the repo root:
  1. Relative links (](./x.md) resolve to existing files.
  2. Backtick-quoted repo paths (`docs/...`, `src/...`, `tools/...`,
     `include/...`, `assets/...`) that look like paths actually exist.
     Anchored at the repo root, not the file's dir — matches project
     convention of citing root-relative paths.

Exit 0 = clean, 1 = findings. Intended for pre-commit and CI.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

MD_LINK = re.compile(r"\]\(([^)\s#]+)(#[^)]*)?\)")
PATH_REF = re.compile(
    r"`((?:docs|src|include|tools|assets|services|scripts|mods|tests|"
    r"platforms|config|cmake|decomp|libultraship|torch)/[^`\s]+)`")

SCAN_DIRS = ["docs"]
SCAN_FILES = ["README.md", "BUILDING.md", "CONTRIBUTING.md", "CLAUDE.md",
              "SECURITY.md"]

SKIP_SCHEMES = ("http://", "https://", "mailto:", "ftp://")

# Paths docs legitimately reference without the file being committed:
# generated-on-demand artifacts (release pubkey is produced by the
# signing maintainer, embedded only if present — CMake guards with
# if(EXISTS)) and user-supplied inputs.
OPTIONAL_REFS = {
    "config/release_pubkey.hex",
}


def _iter_markdown():
    for d in SCAN_DIRS:
        yield from sorted((ROOT / d).rglob("*.md"))
    for f in SCAN_FILES:
        p = ROOT / f
        if p.exists():
            yield p


def check_file(path: Path):
    findings = []
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as e:
        return [(path, f"unreadable: {e}")]

    for m in MD_LINK.finditer(text):
        link = m.group(1)
        if link.startswith(SKIP_SCHEMES) or link.startswith("#"):
            continue
        if not (path.parent / link).resolve().exists():
            findings.append((path, f"dead link: {link}"))

    for m in PATH_REF.finditer(text):
        ref = m.group(1).rstrip(".,);:")
        # strip trailing refs: :N, :N-N, :N:N, :~N, :N,N, ::symbol,
        # :Symbol, and the U+FFFD mojibake left by mangled en-dashes
        ref = re.sub(r"::[A-Za-z_].*$", "", ref)
        ref = re.sub(r"[:~][\d,\-+/~\xa0-\uffff]*\d[\d,\-+/~\xa0-\uffff]*$", "", ref)
        ref = re.sub(r":[A-Za-z_][A-Za-z0-9_]*$", "", ref)
        # 'a/.../b' is doc shorthand for 'somewhere under a/'; ${var}
        # placeholders behave like globs
        ref = ref.replace("/...", "/**")
        ref = re.sub(r"\$\{[^}]*\}", "*", ref)
        # 'file.h/.cpp' shorthand -> the first variant is authoritative
        ref = re.sub(r"(\.[A-Za-z0-9]+)/\.[A-Za-z0-9]+$", r"\1", ref)
        if "<" in ref or not ref or ref in OPTIONAL_REFS:
            continue

        def _resolves(r):
            if "*" in r:
                return bool(list(ROOT.glob(r)))
            return (ROOT / r).exists()

        if "{" in ref and "}" in ref:
            # brace expansion, recursive: {a,b}.{c,d} -> all combos
            variants = [ref]
            while any("{" in v for v in variants):
                expanded = []
                for v in variants:
                    inner = re.search(r"\{([^}]*)\}", v)
                    if not inner:
                        expanded.append(v)
                        continue
                    expanded += [v[: inner.start()] + opt + v[inner.end():]
                                 for opt in inner.group(1).split(",")]
                variants = expanded
            if any(_resolves(v) for v in variants):
                continue
        elif _resolves(ref):
            continue
        findings.append((path, f"missing path ref: `{m.group(1)}`"))
    return findings


def main():
    all_findings = []
    for p in _iter_markdown():
        all_findings.extend(check_file(p))

    for path, msg in all_findings:
        print(f"{path.relative_to(ROOT)}: {msg}")
    print(f"\ncheck_docs: {len(all_findings)} finding(s)")
    return 1 if all_findings else 0


if __name__ == "__main__":
    sys.exit(main())
