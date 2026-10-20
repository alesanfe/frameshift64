#!/usr/bin/env python3
"""generate_graph — DOT/SVG graph of the allowed module-dependency map.

Source of truth: the FORBIDDEN map in
tools/analysis/lint/check_module_deps.py — the same rulebook the
`module-deps` gate enforces, so the picture can't drift from the check.

Modes:
  --format dot    emit Graphviz DOT (default)
  --format svg    DOT rendered through `dot -Tsvg` (needs graphviz on PATH)
  --format json   {module: [allowed deps]} adjacency
  --output PATH   write result

A module A may depend on B when B is not in FORBIDDEN[A] and both live in
the product set. 'decomp' is drawn as an external leaf.
"""
import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools/analysis/lint"))
import check_module_deps as cmd  # noqa: E402


def adjacency() -> dict[str, set[str]]:
    modules = set(cmd.FORBIDDEN) | set(cmd.ALL_PRODUCT)
    edges = {m: set() for m in modules}
    for m in modules:
        forbidden = cmd.FORBIDDEN.get(m, set())
        for dep in modules:
            if dep != m and dep not in forbidden:
                edges[m].add(dep)
    return edges


def short(module: str) -> str:
    return module.replace("src/", "").replace("/", "->")


def to_dot(edges: dict[str, set[str]]) -> str:
    lines = ["digraph modules {", "  rankdir=LR;",
             '  node [shape=box, fontname="Consolas", fontsize=10];',
             '  decomp [style=dashed];']
    for m in sorted(edges):
        lines.append(f'  "{short(m)}";')
    for m, deps in sorted(edges.items()):
        for d in sorted(deps):
            lines.append(f'  "{short(m)}" -> "{short(d)}";')
    lines.append("}")
    return "\n".join(lines) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--format", choices=["dot", "svg", "json"],
                    default="dot")
    ap.add_argument("--output")
    args = ap.parse_args()

    edges = adjacency()
    if args.format == "json":
        payload = json.dumps({m: sorted(d) for m, d in edges.items()},
                             indent=2) + "\n"
        if args.output:
            Path(args.output).parent.mkdir(parents=True, exist_ok=True)
            Path(args.output).write_text(payload, encoding="utf-8")
            print(f"wrote {args.output}")
        else:
            sys.stdout.write(payload)
        return 0

    dot = to_dot(edges)
    if args.format == "svg":
        if not shutil.which("dot"):
            print("error: graphviz 'dot' not on PATH — use --format dot",
                  file=sys.stderr)
            return 2
        r = subprocess.run(["dot", "-Tsvg"], input=dot, capture_output=True,
                           text=True)
        if r.returncode != 0:
            print(f"error: dot failed: {r.stderr}", file=sys.stderr)
            return 2
        payload = r.stdout
    else:
        payload = dot

    if args.output:
        Path(args.output).parent.mkdir(parents=True, exist_ok=True)
        Path(args.output).write_text(payload, encoding="utf-8")
        print(f"wrote {args.output}")
    else:
        sys.stdout.write(payload)
    return 0


if __name__ == "__main__":
    sys.exit(main())
