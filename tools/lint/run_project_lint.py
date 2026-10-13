"""One-shot static-analysis runner for the Smash Classic tree.

Runs every configured linter, writes one report per tool under
lint-reports/, and exits non-zero if any *required* check failed.

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
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence

ROOT = Path(__file__).resolve().parent
REPORTS = ROOT / "lint-reports"
BUILD = ROOT / "build-lint"

OWN_CODE_DIRS = ["port", "include", "android", "uwp", "tools", "debug_tools", "scripts"]

EXCLUDED_DIR_NAMES = {
    ".git", ".venv", ".venv-lint", ".claude",
    "build", "build-lint", "out", "dist", "x64",
    "decomp", "libultraship", "torch", "workspace",
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
        if not path.is_file():
            continue
        rel = path.relative_to(ROOT)
        if any(part in EXCLUDED_DIR_NAMES for part in rel.parts):
            continue
        if path.name == "CMakeLists.txt" or path.suffix == ".cmake":
            files.append(str(rel))
    return sorted(files)


def own_dirs() -> list:
    return [d for d in OWN_CODE_DIRS if (ROOT / d).exists()]


def configure_cmake(args) -> int:
    if args.skip_configure:
        return 0
    cmd = ["cmake", "-S", ".", "-B", BUILD.name,
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
                   else r"smash-classic.(port|include|android|uwp|tools|debug_tools).")
        checks.append(Check("clang-tidy",
            [resolve("run-clang-tidy"), "-p", str(BUILD),
             "-config-file", str(ROOT / ".clang-tidy"),
             "-j", str(args.jobs), pattern],
            "clang-tidy.txt", required=False))
        cpp = [resolve("cppcheck"), f"--project={BUILD / 'compile_commands.json'}",
               "--enable=warning,performance,portability,style",
               "--inconclusive", "--inline-suppr", "--std=c++20"]
        supp = ROOT / "cppcheck-suppressions.txt"
        if supp.exists():
            cpp.append(f"--suppressions-list={supp}")
        checks.append(Check("cppcheck", cpp, "cppcheck.txt", required=False))
    else:
        print("NOTE: no build-lint/compile_commands.json — clang-tidy and "
              "cppcheck skipped")

    cpplint_cmd = [resolve("cpplint"), "--recursive",
                   "--extensions=c,cc,cpp,cxx,h,hh,hpp,hxx",
                   "--filter=-whitespace/line_length,-build/include_subdir",
                   "--exclude=build", "--exclude=build-lint",
                   "--exclude=.venv-lint"]
    if not args.include_vendor:
        cpplint_cmd += ["--exclude=decomp", "--exclude=libultraship",
                        "--exclude=torch", "--exclude=workspace"]
    cpplint_cmd += dirs if not args.include_vendor else ["."]
    checks.append(Check("cpplint", cpplint_cmd, "cpplint.txt", required=False))

    checks += [
        Check("lizard", [resolve("lizard"), "-l", "cpp", "-C", "15",
                         "-L", "120", "-a", "6", "-w"] + dirs,
              "lizard.txt", required=False),
        Check("cmake-lint", [resolve("cmake-lint")] + find_cmake_files(),
              "cmake-lint.txt", required=False),
        Check("codespell", [resolve("codespell"), ".", "--config", ".codespellrc"],
              "codespell.txt", required=False),
        Check("yamllint", [resolve("yamllint"), "-c", ".yamllint.yml", "."],
              "yamllint.txt", required=False),
        Check("ruff", [resolve("ruff"), "check", ".",
                       "--exclude", ".venv-lint", "--exclude", "build",
                       "--exclude", "build-lint", "--exclude", "decomp",
                       "--exclude", "libultraship", "--exclude", "torch",
                       "--output-format", "full"],
              "ruff.txt", required=False),
        Check("bandit", [resolve("bandit"), "-r"] +
              [p for p in ("scripts", "tools", "debug_tools")
               if (ROOT / p).exists()] + ["-x", "*/tests/*,*/build/*"],
              "bandit.txt", required=False),
    ]

    sem = ROOT / "semgrep-smash.yml"
    if sem.exists():
        checks.append(Check("semgrep-smash",
            [resolve("semgrep"), "scan", "--config", str(sem)] + dirs,
            "semgrep-smash.txt", required=False))
    return checks


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--skip-configure", action="store_true")
    ap.add_argument("--include-vendor", action="store_true")
    ap.add_argument("--build-type", default="Debug")
    ap.add_argument("--generator", default="Ninja" if shutil.which("ninja") else None)
    ap.add_argument("--jobs", type=int, default=4)
    args = ap.parse_args()

    REPORTS.mkdir(exist_ok=True)
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
