#!/usr/bin/env python3
"""check_remotes — audit git remote + submodule governance.

Validates the remote layout documented in docs/development/remotes.md:

1. Canonical remote `frameshift` exists and points at alesanfe/frameshift64.
2. `.gitmodules` component URLs all resolve through the canonical remote —
   the SHAs recorded in the parent's gitlinks only exist there.
3. `git submodule status` is clean: no `-` (uninitialized), `+` (checkout
   moved past the gitlink), or `U` (merge conflict) prefixes.
4. The checked-out branch tracks `frameshift/<branch>`.

`--probe` additionally runs `git ls-remote` against each declared remote
and verifies every recorded submodule SHA is advertised by the canonical
remote (fresh clones fail `submodule update` without it).

Exits 0 when clean, 1 listing every violation. Read-only — never rewrites
remote configuration.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
CANONICAL = "github.com/alesanfe/frameshift64"

EXPECTED_REMOTES = {  # name -> url fragment that must appear
    "frameshift": "alesanfe/frameshift64",
    "upstream": "JRickey/BattleShip",
}
# Legacy remotes whose drift is noted but not failed.
LEGACY_REMOTES = {"origin", "fork"}

SUBMODULE_BRANCHES = {  # path suffix -> expected canonical branch
    "components/decomp": "decomp",
    "components/libultraship": "libultraship",
    "components/game-runtime": "ssb64",
}


def git(*args: str, check: bool = True) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["git", "-C", str(ROOT), *args],
        capture_output=True, text=True, check=check, timeout=60)


def remote_urls() -> dict[str, str]:
    out = git("remote", "-v").stdout
    urls = {}
    for line in out.splitlines():
        m = re.match(r"(\S+)\s+(\S+)\s+\((fetch|push)\)", line)
        if m and m.group(3) == "fetch":
            urls[m.group(1)] = m.group(2)
    return urls


def gitmodule_urls() -> dict[str, str]:
    """submodule path -> url from .gitmodules."""
    cfg = ROOT / ".gitmodules"
    urls, path = {}, None
    for line in cfg.read_text(encoding="utf-8").splitlines():
        if m := re.match(r"\s*path\s*=\s*(\S+)", line):
            path = m.group(1)
        elif m := re.match(r"\s*url\s*=\s*(\S+)", line):
            if path:
                urls[path] = m.group(1)
    return urls


def recorded_gitlinks() -> dict[str, str]:
    """submodule path -> SHA recorded in the index."""
    out = git("ls-files", "-s", *SUBMODULE_BRANCHES.keys()).stdout
    links = {}
    for line in out.splitlines():
        m = re.match(r"160000 ([0-9a-f]{40}) \d+\t(.+)", line)
        if m:
            links[m.group(2)] = m.group(1)
    return links


def ls_remote_shas(url: str) -> set[str]:
    """All SHAs advertised by a remote (any ref)."""
    out = subprocess.run(["git", "ls-remote", url],
                         capture_output=True, text=True, timeout=60).stdout
    return {line.split()[0] for line in out.splitlines() if line.strip()}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--probe", action="store_true",
                    help="network ls-remote: verify gitlink SHAs are advertised")
    args = ap.parse_args()

    problems: list[str] = []
    urls = remote_urls()

    for name, frag in EXPECTED_REMOTES.items():
        if name not in urls:
            problems.append(f"remote '{name}' missing (expected *{frag}*) — "
                            "see docs/development/remotes.md")
        elif frag not in urls[name]:
            problems.append(f"remote '{name}' is {urls[name]} — expected *{frag}*")

    for name in LEGACY_REMOTES & urls.keys():
        if "fatty8me2" not in urls[name]:
            problems.append(f"legacy remote '{name}' is {urls[name]} — "
                            "expected the fatty8me2 fork or removal")

    for path, url in gitmodule_urls().items():
        if CANONICAL not in url:
            problems.append(f".gitmodules {path} -> {url} — recorded SHAs "
                            f"are only reachable via {CANONICAL}")

    status = git("submodule", "status", "--recursive").stdout
    for line in status.splitlines():
        if line and line[0] in "-+U":
            tag = {"-": "uninitialized", "+": "moved past gitlink",
                   "U": "merge conflict"}[line[0]]
            problems.append(f"submodule {line[1:].strip()}: {tag}")

    head_branch = git("rev-parse", "--abbrev-ref", "HEAD",
                      check=False).stdout.strip()
    if head_branch and head_branch != "HEAD":
        upstream = git("rev-parse", "--abbrev-ref",
                       f"{head_branch}@{{upstream}}",
                       check=False).stdout.strip()
        expected_up = f"frameshift/{head_branch}"
        if upstream != expected_up:
            problems.append(f"branch '{head_branch}' tracks "
                            f"'{upstream or 'none'}' — expected '{expected_up}'")

    if args.probe:
        canonical_url = urls.get("frameshift")
        if canonical_url:
            advertised = ls_remote_shas(canonical_url)
            for path, sha in recorded_gitlinks().items():
                if sha not in advertised:
                    problems.append(
                        f"gitlink {path}@{sha[:9]} not advertised on "
                        f"{canonical_url} — fresh clones cannot fetch it")
        for name, url in urls.items():
            try:
                ls_remote_shas(url)
            except subprocess.TimeoutExpired:
                problems.append(f"remote '{name}' ls-remote timed out: {url}")

    for p in problems:
        print(f"[remote] {p}")
    if problems:
        print(f"{len(problems)} remote/submodule violation(s) — "
              "see docs/development/remotes.md")
        return 1
    print("remotes + submodules clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
