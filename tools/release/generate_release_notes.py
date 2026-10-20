#!/usr/bin/env python3
"""generate_release_notes — draft release notes from git history.

Groups `git log <prev-tag>..HEAD` subjects by conventional-commit type
(feat/fix/refactor/build/docs/test/chore/perf/other) into Markdown. The
output is a draft — review before attaching to a release.

Flags:
  --version X.Y.Z   header version (required)
  --since TAG       override the auto-detected previous tag
  --output PATH     write Markdown (default stdout)
"""
import argparse
import re
import subprocess
import sys
from collections import OrderedDict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

TYPE_ORDER = ["feat", "fix", "perf", "refactor", "build", "docs",
              "test", "ci", "chore"]
TYPE_TITLES = {
    "feat": "Features", "fix": "Fixes", "perf": "Performance",
    "refactor": "Refactoring", "build": "Build & Tooling",
    "docs": "Documentation", "test": "Tests", "ci": "CI",
    "chore": "Maintenance", "other": "Other",
}


def git(*args) -> str:
    r = subprocess.run(["git", *args], cwd=ROOT, capture_output=True,
                       text=True)
    return r.stdout.strip() if r.returncode == 0 else ""


def prev_tag() -> str:
    return git("describe", "--tags", "--abbrev=0") or git(
        "rev-list", "--max-parents=0", "HEAD").splitlines()[0]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", required=True)
    ap.add_argument("--since")
    ap.add_argument("--output")
    args = ap.parse_args()

    since = args.since or prev_tag()
    log = git("log", f"{since}..HEAD", "--pretty=%s")
    if not log:
        print(f"error: no commits in range {since}..HEAD", file=sys.stderr)
        return 2

    groups: "OrderedDict[str, list[str]]" = OrderedDict(
        (t, []) for t in TYPE_ORDER + ["other"])
    conv = re.compile(r"^(\w+)(?:\([^)]*\))?[!]?:\s*(.+)$")
    for line in log.splitlines():
        m = conv.match(line)
        kind, desc = (m.group(1).lower(), m.group(2)) if m else ("other", line)
        if kind not in groups:
            kind = "other"
        groups[kind].append(desc.strip())

    out = [f"# v{args.version}", "",
           f"Changes since {since} ({len(log.splitlines())} commits).", ""]
    for kind, items in groups.items():
        if not items:
            continue
        out += [f"## {TYPE_TITLES[kind]}", ""]
        out += [f"- {d}" for d in items]
        out.append("")
    text = "\n".join(out)

    if args.output:
        p = Path(args.output)
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text, encoding="utf-8")
        print(f"wrote {p}")
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
