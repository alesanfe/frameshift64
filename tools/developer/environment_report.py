#!/usr/bin/env python3
"""environment_report — machine-readable snapshot of the build environment.

Captures tool versions, host facts, detected presets, git state, and ROM
presence into a JSON report (default out/reports/environment.json). Used by
`make environment` / `make report`; the JSON is what a bug report should
attach.
"""
import argparse
import json
import platform
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

TOOLS = {
    "cmake": ["cmake", "--version"],
    "ctest": ["ctest", "--version"],
    "python": [sys.executable, "--version"],
    "git": ["git", "--version"],
    "node": ["node", "--version"],
    "npm": ["npm", "--version"],
    "ninja": ["ninja", "--version"],
    "clang-format": ["clang-format", "--version"],
    "semgrep": ["semgrep", "--version"],
    "checkmake": ["checkmake", "--version"],
}


def probe(cmd: list[str]) -> str | None:
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=15)
        return r.stdout.splitlines()[0].strip() if r.stdout else None
    except (OSError, subprocess.TimeoutExpired):
        return None


def git(*args) -> str:
    r = subprocess.run(["git", *args], cwd=ROOT, capture_output=True,
                       text=True)
    return r.stdout.strip() if r.returncode == 0 else ""


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--output",
                    default=str(ROOT / "out/reports/environment.json"))
    args = ap.parse_args()

    presets = []
    pj = ROOT / "CMakePresets.json"
    if pj.exists():
        try:
            data = json.loads(pj.read_text(encoding="utf-8"))
            presets = [p["name"] for p in data.get("configurePresets", [])]
        except (json.JSONDecodeError, KeyError):
            pass

    report = {
        "host": {
            "os": platform.system(),
            "release": platform.release(),
            "machine": platform.machine(),
            "python": platform.python_version(),
        },
        "tools": {name: probe(cmd) for name, cmd in TOOLS.items()},
        "presets": presets,
        "git": {
            "head": git("rev-parse", "--short", "HEAD"),
            "branch": git("branch", "--show-current"),
            "dirty": bool(git("status", "--porcelain")),
            "submodules": git("submodule", "status", "--recursive")
                          .splitlines(),
        },
        "roms": {
            p.name: p.exists()
            for p in (ROOT / "roms").glob("baserom.*")
        } if (ROOT / "roms").exists() else {},
        "emsdk": (ROOT / "out/emsdk").exists(),
    }

    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {out}")
    missing = [k for k, v in report["tools"].items() if v is None]
    if missing:
        print("tools not found: " + ", ".join(missing))
    return 0


if __name__ == "__main__":
    sys.exit(main())
