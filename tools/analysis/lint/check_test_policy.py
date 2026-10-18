#!/usr/bin/env python3
"""check_test_policy — validate suite-policies.yml against the tree.

tests/support/config/suite-policies.yml declares one policy block per
tests/<level>/ directory. Checks:

1. every policy `type` is a known level;
2. every top-level dir under tests/ that looks like a level (contains
   test files) has a policy — and vice versa, no orphan policies;
3. declared resource needs (requires_rom / requires_network / requires_hw)
   have corresponding pytest markers in pyproject.toml;
4. `support/`, `config/`, `fixtures/`, `golden/`, `data/`, `harness/`,
   `__pycache__` are exempt — they are infrastructure, not levels.

Exits 0 clean, 1 on violations.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
POLICY = ROOT / "tests" / "support" / "config" / "suite-policies.yml"
TESTS = ROOT / "tests"
PYPROJECT = ROOT / "pyproject.toml"

INFRA_DIRS = {"support", "config", "fixtures", "golden", "data", "harness",
              "__pycache__", "corpora"}

LEVEL_TYPES = {"unit", "integration", "contract", "e2e", "fuzz",
               "security", "smoke", "regression", "performance"}

RESOURCE_MARKERS = {
    "requires_rom": "requires_rom",
    "requires_network": "requires_network",
    "requires_hw": "hardware",
}


def parse_policy() -> dict[str, dict[str, str]]:
    suites: dict[str, dict[str, str]] = {}
    name = None
    for raw in POLICY.read_text(encoding="utf-8").splitlines():
        line = raw.split("#", 1)[0].rstrip()
        if not line.strip() or line.strip() == "suites:":
            continue
        if m := re.match(r"^  (\w[\w-]*):\s*$", line):
            name = m.group(1)
            suites[name] = {}
            continue
        if name and (m := re.match(r"^    (\w+):\s*(.*)$", line)):
            suites[name][m.group(1)] = m.group(2).strip()
    return suites


def level_dirs() -> set[str]:
    out = set()
    for d in TESTS.iterdir():
        if not d.is_dir() or d.name in INFRA_DIRS or d.name.startswith("."):
            continue
        if any(d.rglob("test_*.py")) or any(d.rglob("*_test.*")):
            out.add(d.name)
    return out


def declared_markers() -> set[str]:
    text = PYPROJECT.read_text(encoding="utf-8")
    return set(re.findall(r'"(\w[\w-]*):', text))


def main() -> int:
    policies = parse_policy()
    dirs = level_dirs()
    markers = declared_markers()
    problems: list[str] = []

    for name, pol in policies.items():
        if pol.get("type") not in LEVEL_TYPES:
            problems.append(f"policy '{name}': unknown type "
                            f"'{pol.get('type')}'")
        for field, marker in RESOURCE_MARKERS.items():
            if pol.get(field) == "true" and marker not in markers:
                problems.append(
                    f"policy '{name}': {field}=true but marker '{marker}' "
                    "not declared in pyproject.toml")
        for field in ("owner", "timeout_s", "ci_blocking", "parallel_ok"):
            if field not in pol:
                problems.append(f"policy '{name}': missing field '{field}'")

    for name in sorted(dirs - policies.keys()):
        problems.append(f"tests/{name}/ has tests but no policy entry")
    for name in sorted(policies.keys() - dirs - {"performance"}):
        # performance/ is allowed to exist as a policy before the dir does
        if not (TESTS / name).is_dir():
            problems.append(f"policy '{name}' has no tests/{name}/ dir")

    for p in problems:
        print(f"[test-policy] {p}")
    if problems:
        print(f"{len(problems)} violation(s) — see tests/support/config/"
              "suite-policies.yml")
        return 1
    print(f"test policy clean ({len(policies)} levels, {len(dirs)} dirs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
