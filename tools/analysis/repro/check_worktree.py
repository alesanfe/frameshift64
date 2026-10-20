#!/usr/bin/env python3
"""check_worktree — fail if the git worktree (or submodules) are dirty.

Reproducibility requires a known input state. A build produced from a dirty
tree cannot be reproduced from the recorded commit.

Flags:
  --recursive-submodules   also require clean submodule worktrees
  --allow-untracked        tolerate `??` untracked files (only tracked
                           modifications fail)

Exit 0 clean, 1 dirty, 2 not a git repo.
"""
import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def git(*args, cwd=ROOT) -> str:
    r = subprocess.run(["git", *args], cwd=cwd, capture_output=True,
                       text=True)
    if r.returncode != 0:
        raise SystemExit(f"error: git {' '.join(args)}: {r.stderr.strip()}")
    return r.stdout


def dirty_entries(status: str, allow_untracked: bool) -> list[str]:
    bad = []
    for line in status.splitlines():
        if not line:
            continue
        if allow_untracked and line.startswith("??"):
            continue
        bad.append(line)
    return bad


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--recursive-submodules", action="store_true")
    ap.add_argument("--allow-untracked", action="store_true")
    args = ap.parse_args()

    try:
        git("rev-parse", "--git-dir")
    except SystemExit:
        print("error: not a git repository", file=sys.stderr)
        return 2

    problems = []
    bad = dirty_entries(git("status", "--porcelain"), args.allow_untracked)
    problems += [f"worktree: {l}" for l in bad]

    if args.recursive_submodules:
        # flag column: ' ' = at recorded sha, '-' = not initialized,
        # '+' = not at recorded sha, 'U' = merge conflict
        flags = {}
        for line in git("submodule", "status", "--recursive").splitlines():
            if not line.strip():
                continue
            parts = line[1:].split()
            if len(parts) >= 2:
                flags[parts[1]] = line[:1]
        # authoritative path list (status output paths are hard to split
        # from the trailing describe text)
        paths = []
        for line in git("config", "--file", ".gitmodules", "--get-regexp",
                        r"\.path$").splitlines():
            paths.append(line.split(None, 1)[1].strip())
        for path in paths:
            flag = flags.get(path, " ")
            if flag == "-":
                problems.append(f"{path}: submodule not initialized")
            elif flag == "+":
                problems.append(f"{path}: submodule not at recorded SHA")
            elif flag == "U":
                problems.append(f"{path}: submodule has merge conflicts")
            elif (ROOT / path / ".git").exists() or \
                    (ROOT / path).is_dir():
                r = subprocess.run(
                    ["git", "status", "--porcelain"], cwd=ROOT / path,
                    capture_output=True, text=True)
                if r.returncode == 0 and r.stdout.strip():
                    for l in dirty_entries(r.stdout, args.allow_untracked):
                        problems.append(f"{path}: {l}")

    if problems:
        print("Dirty worktree:")
        for p in problems:
            print(f"  {p}")
        return 1
    print("worktree: clean"
          + (" (submodules clean)" if args.recursive_submodules else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
