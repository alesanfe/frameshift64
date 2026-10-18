"""Complexity ratchet for the modern port layer.

Runs lizard over the own-code dirs and compares per-file violation counts
against config/complexity-baseline.txt. A function is a violation when its
cyclomatic complexity >= 15 or its NLOC >= 100 (the Phase-0 budgets).

    python tools/analysis/lint/check_complexity.py           # check vs baseline
    python tools/analysis/lint/check_complexity.py --update  # regenerate baseline
    python tools/analysis/lint/check_complexity.py --report  # list violations

Baseline is per-file (path -> allowed violation count) rather than
per-function so renames and line shifts don't churn it: adding a new
over-budget function to a file already at its quota fails the check, and
reducing a file's count below quota is a signal to regenerate and lock in
the improvement. Files absent from the baseline must be clean.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
BASELINE = ROOT / "config" / "complexity-baseline.txt"

SCAN_DIRS = ["src", "include", "platforms", "tools", "services", "mods"]
CCN_LIMIT = 15
NLOC_LIMIT = 100


def lizard_exe() -> str:
    exe = shutil.which("lizard")
    if exe:
        return exe
    venv = ROOT / ".venv-lint" / ("Scripts" if sys.platform == "win32" else "bin")
    for cand in venv.glob("lizard*"):
        return str(cand)
    sys.exit("lizard not found — pip install lizard (or tools/analysis/lint/requirements.txt)")


def collect_violations() -> dict[str, list[tuple[str, int, int]]]:
    """relpath -> [(func, ccn, nloc)] for functions over budget."""
    with tempfile.NamedTemporaryFile(suffix=".xml", delete=False) as tmp:
        xml_path = tmp.name
    # -l cpp restricts to C/C++ (vendored Java/JS/Python stay out); relative
    # dirs keep the item names repo-relative for stable baseline keys.
    dirs = [d for d in SCAN_DIRS if (ROOT / d).exists()]
    subprocess.run([lizard_exe(), "-l", "cpp", "--xml", "-C", str(CCN_LIMIT),
                    "-L", str(NLOC_LIMIT), "-x", "*/node_modules/*",
                    "-o", xml_path] + dirs,
                   check=True, capture_output=True, cwd=ROOT)
    violations: dict[str, list[tuple[str, int, int]]] = {}
    for item in ET.parse(xml_path).getroot().iter("item"):  # nosec B314 - parses lizard's own -o output, not untrusted input
        # cppncss-style: <item name="func(...) at rel/path.cpp:line">
        # with <value> children (Nr, NCSS~NLOC, CCN).
        label = item.get("name", "")
        vals = [v.text or "0" for v in item.findall("value")]
        if " at " not in label or len(vals) < 3:
            continue
        func, _, loc = label.rpartition(" at ")
        path = loc.rsplit(":", 1)[0]
        nloc, ccn = int(float(vals[1])), int(float(vals[2]))
        if ccn < CCN_LIMIT and nloc < NLOC_LIMIT:
            continue
        violations.setdefault(Path(path).as_posix(), []).append((func, ccn, nloc))
    Path(xml_path).unlink(missing_ok=True)
    return violations


def load_baseline() -> dict[str, int]:
    out: dict[str, int] = {}
    if not BASELINE.exists():
        return out
    for line in BASELINE.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        path, _, count = line.rpartition(" ")
        out[path.strip()] = int(count)
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--update", action="store_true", help="write current counts to the baseline")
    ap.add_argument("--report", action="store_true", help="print every violation")
    args = ap.parse_args()

    violations = collect_violations()
    total = sum(len(v) for v in violations.values())

    if args.report:
        for path in sorted(violations):
            for func, ccn, nloc in violations[path]:
                print(f"{path}: {func}  CCN={ccn} NLOC={nloc}")
        print(f"{total} violation(s) in {len(violations)} file(s)")

    if args.update:
        lines = [f"{p} {len(v)}" for p, v in sorted(violations.items())]
        BASELINE.write_text(
            "# Per-file complexity violation quota (lizard -C 15 -L 100).\n"
            "# Regenerate with: python tools/analysis/lint/check_complexity.py --update\n"
            + "\n".join(lines) + "\n", encoding="utf-8")
        print(f"baseline -> {BASELINE.relative_to(ROOT)} "
              f"({total} violations across {len(lines)} files)")
        return 0

    baseline = load_baseline()
    failures = []
    regressions = []
    for path, found in sorted(violations.items()):
        allowed = baseline.get(path, 0)
        if len(found) > allowed:
            regressions.append((path, len(found), allowed))
    for path, allowed in sorted(baseline.items()):
        if path not in violations:
            failures.append((path, 0, allowed))  # improved to zero

    for path, found, allowed in regressions:
        names = ", ".join(f"{f}(CCN={c})" for f, c, _ in violations[path])
        print(f"FAIL {path}: {found} violation(s) > baseline {allowed}\n     {names}")
    for path, _, allowed in failures:
        print(f"NOTE {path}: clean now — drop its baseline entry ({allowed} allowed)")

    if regressions:
        print(f"\n{sum(f - a for _, f, a in regressions)} new over-budget function(s). "
              "Refactor below CCN<15 / NLOC<100, or if the growth is intentional "
              "run --update and justify it in review.")
        return 1
    print(f"complexity ratchet OK: {total} violation(s), all within baseline quotas")
    return 0


if __name__ == "__main__":
    sys.exit(main())
