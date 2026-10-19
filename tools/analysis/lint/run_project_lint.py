"""One-shot static-analysis runner for the Smash Classic tree.

Runs every configured linter, writes one report per tool under
out/reports/lint/, and exits non-zero if any *required* check failed.

    python run_project_lint.py                 # own code only
    python run_project_lint.py --skip-configure
    python run_project_lint.py --include-vendor
    python run_project_lint.py --build-type Release
"""

from __future__ import annotations

import argparse
import os
import shlex
import shutil
import subprocess
import sys
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]  # tools/analysis/lint/ -> repo root
REPORTS = ROOT / "out" / "reports" / "lint"
BUILD = ROOT / "out" / "build" / "lint"

OWN_CODE_DIRS = ["src", "include", "platforms", "tools", "scripts", "services", "mods", "tests"]

EXCLUDED_DIR_NAMES = {
    ".git", ".venv", ".venv-lint", ".claude",
    "build", "build-lint", "out", "dist", "x64",
    "decomp", "libultraship", "torch", "components", "mods/examples",
    "node_modules", "assets", "expected",
}


@dataclass(frozen=True)
class Check:
    name: str
    command: list
    report: str
    required: bool = True


def command_exists(command: str) -> bool:
    if shutil.which(command):
        return True
    # tools installed into the lint venv only
    scripts_dir = ROOT / ".venv-lint" / ("Scripts" if os.name == "nt" else "bin")
    return (scripts_dir / command).exists() or any(
        p.name == command or p.stem == command
        for p in scripts_dir.glob(f"{command}*")
    ) if scripts_dir.exists() else False


def resolve(command: str) -> str:
    """Prefer the venv copy so callers don't need to activate it."""
    scripts_dir = ROOT / ".venv-lint" / ("Scripts" if os.name == "nt" else "bin")
    for ext in ("", ".exe", ".bat", ".cmd"):
        cand = scripts_dir / f"{command}{ext}"
        if cand.exists():
            return str(cand)
    return command


def render_command(command: Sequence) -> str:
    flat = [str(c) for c in command]
    return subprocess.list2cmdline(flat) if os.name == "nt" else shlex.join(flat)


def run_check(check: Check) -> int:
    report_path = REPORTS / check.report
    print(f"\n{'=' * 72}\n[{check.name}]\n{render_command(check.command)}\n"
          f"report: {report_path.relative_to(ROOT)}\n{'=' * 72}")
    executable = resolve(str(check.command[0]))
    if not command_exists(executable):
        msg = f"SKIP: executable not found: {executable}\n"
        report_path.write_text(msg, encoding="utf-8")
        print(msg.rstrip())
        return 0 if not check.required else 2
    cmd = [executable] + [str(c) for c in check.command[1:]]
    with report_path.open("w", encoding="utf-8", errors="replace") as report:
        proc = subprocess.run(cmd, cwd=ROOT, stdout=report,
                              stderr=subprocess.STDOUT, text=True, check=False)
    print(f"[{'OK' if proc.returncode == 0 else f'EXIT {proc.returncode}'}] {check.name}")
    return proc.returncode


def find_cmake_files() -> list:
    files = []
    for path in ROOT.rglob("*"):
        rel = path.relative_to(ROOT)
        if any(part in EXCLUDED_DIR_NAMES for part in rel.parts):
            continue
        try:
            if not path.is_file():
                continue
        except OSError:
            continue  # dangling symlink/junction (e.g. under _deps)
        if path.name == "CMakeLists.txt" or path.suffix == ".cmake":
            files.append(str(rel))
    return sorted(files)


def own_dirs() -> list:
    return [d for d in OWN_CODE_DIRS if (ROOT / d).exists()]


def find_shell_files() -> list:
    """Repo-owned POSIX shell scripts (vendor/submodule trees excluded)."""
    files = []
    for d in ("packaging", "scripts", "tools", "platforms"):
        base = ROOT / d
        if not base.exists():
            continue
        for path in base.rglob("*.sh"):
            rel = path.relative_to(ROOT).as_posix()
            if "/build/" in rel or "/out/" in rel:
                continue
            files.append(rel)
    return sorted(files)


def configure_cmake(args) -> int:
    if args.skip_configure:
        return 0
    cmd = ["cmake", "-S", ".", "-B", str(BUILD),
           "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
           f"-DCMAKE_BUILD_TYPE={args.build_type}"]
    if args.generator:
        cmd += ["-G", args.generator]
    print("\nConfiguring build-lint:\n" + render_command(cmd))
    return subprocess.run(cmd, cwd=ROOT, check=False).returncode


def build_checks(args) -> list:
    dirs = own_dirs()
    checks = []

    if (BUILD / "compile_commands.json").exists():
        # compile_commands.json uses absolute paths with backslashes on Windows.
        # '.' matches either separator without needing backslash escapes.
        pattern = (r".*" if args.include_vendor
                   else r"smash-classic.(src|include|platforms|tools|services|mods|tests).")
        checks.append(Check("clang-tidy",
            [resolve("run-clang-tidy"), "-p", str(BUILD),
             "-config-file", str(ROOT / ".clang-tidy"),
             "-j", str(args.jobs), pattern],
            "clang-tidy.txt", required=False))
        cpp = [resolve("cppcheck"), f"--project={BUILD / 'compile_commands.json'}",
               "--enable=warning,performance,portability,style",
               "--inconclusive", "--inline-suppr", "--std=c++20"]
        supp = ROOT / "config" / "static-analysis" / "cppcheck-suppressions.txt"
        if supp.exists():
            cpp.append(f"--suppressions-list={supp}")
        checks.append(Check("cppcheck", cpp, "cppcheck.txt", required=False))
    else:
        print("NOTE: no build-lint/compile_commands.json — clang-tidy and "
              "cppcheck skipped")

    cpplint_cmd = [resolve("cpplint"), "--recursive",
                   "--extensions=c,cc,cpp,cxx,h,hh,hpp,hxx",
                   # Filtered to repo conventions: tabs are the codebase's
                   # indent style, C casts + long/short are load-bearing in
                   # the N64 ABI layer, brace style is mixed deliberately.
                   "--filter=-whitespace/line_length,-build/include_subdir,-whitespace/tab,-readability/casting,-runtime/int,-whitespace/braces",
                   "--exclude=build", "--exclude=build-lint", "--exclude=out",
                   "--exclude=.venv-lint",
                   # cpplint globs excludes from CWD without recursion —
                   # "node_modules" alone matches nothing; name the dir.
                   "--exclude=services/netplay-lobby/node_modules",
                   "--exclude=generated", "--exclude=.claude"]
    if not args.include_vendor:
        cpplint_cmd += ["--exclude=components", "--exclude=mods/examples"]
    cpplint_cmd += dirs if not args.include_vendor else ["."]
    checks.append(Check("cpplint", cpplint_cmd, "cpplint.txt", required=False))

    checks += [
        Check("lizard", [resolve("lizard"), "-l", "cpp", "-C", "15", "-L", "120", "-a", "6", "-w", "-x", "*/node_modules/*", *dirs],
              "lizard.txt", required=False),
        # Complexity ratchet: per-file quota vs config/complexity-baseline.txt.
        # Blocks growth of CCN>=15 / NLOC>=100 functions; the lizard report
        # above stays informational.
        Check("complexity",
              [sys.executable, str(ROOT / "tools" / "analysis" / "lint" /
                                   "check_complexity.py")],
              "complexity.txt", required=True),
        # File-size ratchet: no new file over 1000 lines; existing giants
        # are pinned at their current size via config/file-size-baseline.txt.
        Check("file-size",
              [sys.executable, str(ROOT / "tools" / "analysis" / "lint" /
                                   "check_file_size.py")],
              "file-size.txt", required=True),
        Check("cmake-lint", [resolve("cmake-lint"), *find_cmake_files(), "-c", ".cmakelintrc"],
              "cmake-lint.txt", required=False),
        Check("codespell", [resolve("codespell"), ".", "--config", ".codespellrc"],
              "codespell.txt", required=False),
        Check("yamllint", [resolve("yamllint"), "-c", ".yamllint.yml", "."],
              "yamllint.txt", required=False),
        Check("ruff", [resolve("ruff"), "check", ".",
                       "--exclude", ".venv-lint", "--exclude", "build",
                       "--exclude", "build-lint", "--exclude", "out",
                       "--exclude", "components",
                       "--output-format", "full"],
              "ruff.txt", required=False),
        Check("bandit", [resolve("bandit"), "-r", "-ll"] +
              [p for p in ("scripts", "tools")
               if (ROOT / p).exists()] + ["-x", "*/tests/*,*/build/*"],
              "bandit.txt", required=False),
        Check("module-deps",
              [sys.executable, str(ROOT / "tools" / "analysis" / "lint" /
                                   "check_module_deps.py")],
              "module-deps.txt", required=True),
        Check("cvar-schema",
              [sys.executable, str(ROOT / "tools" / "analysis" / "lint" /
                                   "check_cvar_schema.py")],
              "cvar-schema.txt", required=True),
        Check("generated-headers",
              [sys.executable, str(ROOT / "tools" / "analysis" / "lint" /
                                   "check_generated_headers.py")],
              "generated-headers.txt", required=True),
        Check("retired-paths",
              [sys.executable, str(ROOT / "tools" / "analysis" / "lint" /
                                   "check_retired_paths.py")],
              "retired-paths.txt", required=True),
        Check("public-api",
              [sys.executable, str(ROOT / "tools" / "analysis" / "lint" /
                                   "check_public_api.py")],
              "public-api.txt", required=True),
        Check("test-policy",
              [sys.executable, str(ROOT / "tools" / "analysis" / "lint" /
                                   "check_test_policy.py")],
              "test-policy.txt", required=True),
        Check("remotes",
              [sys.executable, str(ROOT / "tools" / "analysis" / "lint" /
                                   "check_remotes.py")],
              "remotes.txt", required=True),
        # --- external linters beyond the classic suite -------------------
        Check("zizmor",
              [resolve("zizmor"), "--format", "plain", ".github/workflows"],
              "zizmor.txt", required=False),
        Check("shellcheck",
              [resolve("shellcheck"), "-S", "warning", *find_shell_files()],
              "shellcheck.txt", required=False),
        Check("vulture",
              [resolve("vulture"), "tools", "tests", "--min-confidence", "80"],
              "vulture.txt", required=False),
        Check("pip-audit",
              [resolve("pip-audit"), "-r",
               str(ROOT / "tools" / "analysis" / "lint" / "requirements.txt")],
              "pip-audit.txt", required=False),
    ]

    sem = ROOT / "config" / "analysis" / "semgrep" / "smash-classic.yml"
    if sem.exists():
        checks.append(Check("semgrep-smash",
            [resolve("semgrep"), "scan", "--config", str(sem), *dirs],
            "semgrep.txt", required=False))
    return checks


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--skip-configure", action="store_true")
    ap.add_argument("--include-vendor", action="store_true")
    ap.add_argument("--build-type", default="Debug")
    ap.add_argument("--generator", default="Ninja" if shutil.which("ninja") else None)
    ap.add_argument("--jobs", type=int, default=4)
    args = ap.parse_args()

    REPORTS.mkdir(parents=True, exist_ok=True)
    rc = configure_cmake(args)
    if rc != 0:
        print(f"cmake configure failed ({rc}); running tools that don't "
              "need compile_commands.json")

    worst = 0
    for check in build_checks(args):
        rc = run_check(check)
        if check.required:
            worst = max(worst, rc)
    print(f"\nReports in {REPORTS.relative_to(ROOT)}/")
    return worst


if __name__ == "__main__":
    sys.exit(main())
