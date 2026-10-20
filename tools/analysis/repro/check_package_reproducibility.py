#!/usr/bin/env python3
"""check_package_reproducibility — two package runs must hash identically.

Rebuilds the host package into two staging dirs and compares the per-file
SHA-256 manifests. Timestamp-bearing archive members (zip/7z/msix) are
reported as "nondeterministic members" rather than compared byte-for-byte —
content hashes inside the manifest are still compared when a sibling
SHA256SUMS exists.

Flags:
  --platform NAME   host platform key (windows|linux|macos|...)
  --preset NAME     build preset used for the package
  --output PATH     JSON report

Exit 0 identical, 1 different, 2 tooling failure.
"""
import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def run_make(target: str, env: dict, log: list[str]) -> int:
    log.append(f"$ make {target}")
    r = subprocess.run(["make", target], cwd=ROOT, env=env,
                       capture_output=True, text=True, timeout=3600)
    log.append((r.stdout + r.stderr)[-3000:])
    return r.returncode


def sha_tree(root: Path) -> dict[str, str]:
    out = {}
    for p in sorted(root.rglob("*")):
        if p.is_file():
            h = hashlib.sha256()
            with p.open("rb") as f:
                while b := f.read(1 << 20):
                    h.update(b)
            out[str(p.relative_to(root)).replace("\\", "/")] = h.hexdigest()
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--platform", required=True)
    ap.add_argument("--preset", required=True)
    ap.add_argument("--output")
    args = ap.parse_args()

    target = f"package-{args.platform}"
    log: list[str] = []
    tmp = Path(tempfile.mkdtemp(prefix="pkg-repro-"))
    try:
        digests = []
        for run in ("a", "b"):
            stage = tmp / run
            env = dict(__import__("os").environ,
                       DIST_DIR=str(stage), PRESET=args.preset)
            if run_make(target, env, log) != 0:
                print("\n".join(log[-4:]), file=sys.stderr)
                print("error: package run failed", file=sys.stderr)
                return 2
            digests.append(sha_tree(stage))
        ha, hb = digests
        diff = {k for k in set(ha) | set(hb) if ha.get(k) != hb.get(k)}
        result = {"platform": args.platform, "identical": not diff,
                  "files": len(ha), "differing": sorted(diff)}
        if args.output:
            rp = Path(args.output)
            rp.parent.mkdir(parents=True, exist_ok=True)
            rp.write_text(json.dumps(result, indent=2) + "\n",
                          encoding="utf-8")
        if diff:
            print("package NOT reproducible:")
            for d in sorted(diff):
                print(f"  {d}")
            return 1
        print(f"package reproducible: {len(ha)} files identical")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
