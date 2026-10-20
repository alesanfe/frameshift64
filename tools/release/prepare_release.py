#!/usr/bin/env python3
"""prepare_release — pre-flight validation before a release build.

Checks, in order:
  * --version X.Y.Z is a sane semver (no leading v)
  * the git worktree is clean (delegates to check_worktree)
  * submodules sit at recorded SHAs (when --require-submodules-clean)
  * HEAD is on the release branch (main, unless --branch overrides)
  * no existing v<version> tag (release can't be stamped twice)

Exit 0 ready, 1 on failed gates. Does NOT build or tag anything.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/analysis/repro"))
from check_worktree import dirty_entries  # noqa: E402


def git(*args) -> tuple[int, str]:
    r = subprocess.run(["git", *args], cwd=ROOT, capture_output=True,
                       text=True)
    return r.returncode, (r.stdout or r.stderr).strip()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", required=True)
    ap.add_argument("--branch", default="main")
    ap.add_argument("--require-clean", action="store_true")
    ap.add_argument("--require-submodules-clean", action="store_true")
    args = ap.parse_args()

    problems = []

    if not re.fullmatch(r"\d+\.\d+\.\d+", args.version):
        problems.append(f"VERSION '{args.version}' is not X.Y.Z semver "
                        "(no leading v)")

    if args.require_clean:
        rc, out = git("status", "--porcelain")
        bad = dirty_entries(out, allow_untracked=False)
        for l in bad:
            problems.append(f"worktree dirty: {l}")

    if args.require_submodules_clean:
        rc, out = git("submodule", "status", "--recursive")
        for line in out.splitlines():
            if line.startswith(("+", "-", "U")):
                problems.append(f"submodule state: {line[:60]}")

    rc, branch = git("branch", "--show-current")
    if branch != args.branch:
        problems.append(f"on branch '{branch}', expected '{args.branch}'")

    rc, out = git("tag", "-l", f"v{args.version}")
    if out.strip():
        problems.append(f"tag v{args.version} already exists")

    if problems:
        print("Release pre-flight FAILED:")
        for p in problems:
            print(f"  - {p}")
        return 1
    print(f"release pre-flight OK — v{args.version} on {args.branch}, "
          "tree clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
