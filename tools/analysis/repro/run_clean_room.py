#!/usr/bin/env python3
"""run_clean_room — verify the project builds from a pristine worktree.

Reproduces a fresh-checkout experience without touching the main tree:

  1. git worktree add <tmp> --detach <commit> (no local hacks ride along)
  2. submodule update --init --recursive inside it
  3. cmake --preset <preset> + cmake --build + ctest (unless --skip-build)
  4. verify the worktree ends clean (a build must not dirty tracked files)
  5. remove the worktree even when a step fails

Flags:
  --commit REF      commit to check out (default HEAD)
  --preset NAME     configure preset (default <host>-debug equivalent —
                    pass the real repo preset, e.g. windows-debug)
  --skip-build      only verify checkout + configure (fast smoke)
  --report PATH     write a JSON report
  --keep            keep the worktree afterwards (debugging)

Exit 0 verified, 1 on any step failure.
"""
import argparse
import json
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def run(cmd: list[str], cwd: Path, log: list[str],
        timeout: int = 3600) -> int:
    log.append(f"$ {' '.join(cmd)}")
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True,
                       timeout=timeout)
    log.append((r.stdout + r.stderr)[-4000:])
    return r.returncode


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--commit", default="HEAD")
    ap.add_argument("--preset", required=True)
    ap.add_argument("--skip-build", action="store_true")
    ap.add_argument("--report")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    log: list[str] = []
    tmp = Path(tempfile.mkdtemp(prefix="cleanroom-"))
    wt = tmp / "tree"
    result = {"commit": args.commit, "preset": args.preset,
              "steps": [], "ok": False}
    t0 = time.time()

    def step(name, rc):
        result["steps"].append({"name": name, "rc": rc})
        return rc == 0

    try:
        if not step("worktree-add",
                    run(["git", "worktree", "add", "--detach",
                         str(wt), args.commit], ROOT, log)):
            raise SystemExit(1)
        if not step("submodules",
                    run(["git", "submodule", "update", "--init",
                         "--recursive"], wt, log)):
            raise SystemExit(1)
        if not step("configure",
                    run(["cmake", "--preset", args.preset], wt, log)):
            raise SystemExit(1)
        if not args.skip_build:
            if not step("build",
                        run(["cmake", "--build", "--preset", args.preset,
                             "--parallel", "4"], wt, log)):
                raise SystemExit(1)
            if not step("ctest",
                        run(["ctest", "--test-dir",
                             str(wt / "out/build" / args.preset),
                             "--output-on-failure"], wt, log)):
                raise SystemExit(1)
        # the build must not modify tracked files
        dirty = subprocess.run(["git", "status", "--porcelain"], cwd=wt,
                               capture_output=True, text=True).stdout.strip()
        step("clean-after-build", 0 if not dirty else 1)
        if dirty:
            log.append("worktree dirty after build:\n" + dirty)
            raise SystemExit(1)
        result["ok"] = True
    except SystemExit as e:
        pass
    except subprocess.TimeoutExpired:
        result["steps"].append({"name": "timeout", "rc": 1})
    finally:
        result["seconds"] = round(time.time() - t0, 1)
        if wt.exists():
            subprocess.run(["git", "worktree", "remove", "--force",
                            str(wt)], cwd=ROOT, capture_output=True)
        if tmp.exists() and not args.keep:
            shutil.rmtree(tmp, ignore_errors=True)
        if args.report:
            rp = Path(args.report)
            rp.parent.mkdir(parents=True, exist_ok=True)
            rp.write_text(json.dumps(result, indent=2) + "\n",
                          encoding="utf-8")

    status = "clean-room OK" if result["ok"] else "clean-room FAILED"
    print(f"{status} ({result['seconds']}s)")
    for s in result["steps"]:
        print(f"  {s['name']}: {'ok' if s['rc'] == 0 else 'FAIL'}")
    if not result["ok"]:
        print("\n".join(log[-6:]), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
