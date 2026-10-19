#!/usr/bin/env python3
"""Aggregate netplay determinism metrics from a test run directory.

Parses the structured lines netrollback.c already emits into the per-peer
logs plus the confirmed-state traces the harness writes, and produces a
single metrics.json:

    {
      "peers": { "host": {"ticks": N, "rollbacks": N, "resim_ticks": N,
                          "max_rollback": N, "sync_failures": N, "desync": 0},
                 ... },
      "traces": { "ticks": N, "diverged": false, "first_divergence_line": null },
      "desync_rate": 0.0,          # diverged peers / compared peers
      "worst_max_rollback": N,     # deepest resim depth observed, any peer
      "rollback_frames_per_tick": 0.0,
      "input_delay": N,            # worst configured input-delay (auto-bump counts)
      "late_inputs": N,            # inputs that arrived too late to pace
      "dropped_datagrams": N,
      "stalls": N
    }

Usage:
    python tools/analysis/netplay_metrics.py --dir out/logs/netplay-test \
        --out out/logs/netplay-test/metrics.json

Exit code: 0 always (metrics, not a gate) unless --fail-on-desync or
--gate is passed. --gate compares this run's metrics against a committed
baseline JSON ({label: {metric: value}}): a clean baseline going red is a
hard fail; numeric counters fail when they exceed baseline * (1+tolerance).
Labels missing from the baseline produce a note, not a failure — the
nightly workflow's baseline-refresh step is what pins them.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

# "SSB64 Rollback: netplay tick=1300 rollbacks=4 resim_ticks=9 max_rollback=6
#  failed=0 confirmed=1300 sync_checks=22 desync=0"
NETPLAY_LINE = re.compile(
    r"netplay tick=(?P<tick>\d+) rollbacks=(?P<rollbacks>\d+) "
    r"resim_ticks=(?P<resim>\d+) max_rollback=(?P<maxrb>\d+) "
    r"failed=(?P<failed>\d+) confirmed=(?P<confirmed>\d+) "
    r"sync_checks=(?P<sync>\d+) desync=(?P<desync>-?\d+)"
)
# "SSB64 Rollback: netplay session rollbacks=7 resim_ticks=15 max_rollback=6
#  failed=0 traced_ticks=1800 sync_checks=30 desync=0"
SESSION_LINE = re.compile(
    r"netplay session rollbacks=(?P<rollbacks>\d+) resim_ticks=(?P<resim>\d+) "
    r"max_rollback=(?P<maxrb>\d+) failed=(?P<failed>\d+) "
    r"traced_ticks=(?P<traced>\d+) sync_checks=(?P<sync>\d+) desync=(?P<desync>-?\d+)"
)
DESYNC_LINE = re.compile(r"DESYNC detected at tick=(\d+)")

# Latency counters netpeer.c already emits per session:
#   "SSB64 NetPeer: VS session stop sent=%u recv=%u dropped=%u staged=%u
#    late=%u stalls=%u lockstep_stalls=%u timesync_stalls=%u checksum=..."
NETPEER_STOP_LINE = re.compile(
    r"VS session stop sent=(?P<sent>\d+) recv=(?P<recv>\d+) "
    r"dropped=(?P<dropped>\d+) staged=(?P<staged>\d+) late=(?P<late>\d+) "
    r"stalls=(?P<stalls>\d+)"
)
# "SSB64 NetPeer: input delay -> %u"   (configured input-delay frames)
DELAY_LINE = re.compile(r"input delay -> (?P<delay>\d+)")
# "SSB64 NetPeer: sustained late inputs (%u in %u frames) — bumping delay to %u"
AUTO_DELAY_LINE = re.compile(r"bumping delay to (?P<delay>\d+)")
# "SSB64 Perf: frames=600 window_s=10.0 frame_p50_us=1234 frame_p95_us=...
#  frame_p99_us=... frame_max_us=..."   (SSB64_PERF_LOG=1, gameloop.cpp)
PERF_LINE = re.compile(
    r"SSB64 Perf: frames=(?P<frames>\d+) window_s=(?P<win>[\d.]+) "
    r"frame_p50_us=(?P<p50>\d+) frame_p95_us=(?P<p95>\d+) "
    r"frame_p99_us=(?P<p99>\d+) frame_max_us=(?P<max>\d+)"
)


def peer_metrics(log_path: Path) -> dict:
    """Latest counters win (periodic lines overwrite; session line is final)."""
    m: dict[str, Any] = {"ticks": 0, "rollbacks": 0, "resim_ticks": 0, "max_rollback": 0,
                         "sync_failures": 0, "desync": 0, "desync_ticks": [],
                         "input_delay": 0, "late_inputs": 0, "dropped": 0, "stalls": 0,
                         "frame_p50_us": 0, "frame_p95_us": 0, "frame_p99_us": 0,
                         "frame_max_us": 0, "perf_windows": 0}
    for line in log_path.read_text(encoding="utf-8", errors="replace").splitlines():
        mm = SESSION_LINE.search(line) or NETPLAY_LINE.search(line)
        if mm:
            m["ticks"] = int(mm["traced"] if "traced" in mm.groupdict() and mm["traced"]
                             else mm["confirmed"] if "confirmed" in mm.groupdict() else mm["tick"])
            m["rollbacks"] = max(m["rollbacks"], int(mm["rollbacks"]))
            m["resim_ticks"] = max(m["resim_ticks"], int(mm["resim"]))
            m["max_rollback"] = max(m["max_rollback"], int(mm["maxrb"]))
            m["sync_failures"] = max(m["sync_failures"], int(mm["failed"]))
            m["desync"] = max(m["desync"], int(mm["desync"]))
            continue
        dm = DESYNC_LINE.search(line)
        if dm:
            m["desync"] = 1
            m["desync_ticks"].append(int(dm.group(1)))
            continue
        pm = PERF_LINE.search(line)
        if pm:
            # perf windows are periodic — keep the worst window (the tail
            # is what benchmarking gates on), plus a count so an absent
            # SSB64_PERF_LOG run is distinguishable from a clean one.
            m["perf_windows"] += 1
            for field, group in (("frame_p50_us", "p50"), ("frame_p95_us", "p95"),
                                 ("frame_p99_us", "p99"), ("frame_max_us", "max")):
                m[field] = max(m[field], int(pm[group]))
            continue
        if "SSB64 NetPeer:" in line:
            sm = NETPEER_STOP_LINE.search(line)
            if sm:
                # session-stop is terminal — overwrite (a log can hold
                # several sessions; the last one's totals are the match)
                m["late_inputs"] = int(sm["late"])
                m["dropped"] = int(sm["dropped"])
                m["stalls"] = int(sm["stalls"])
                continue
            am = AUTO_DELAY_LINE.search(line)
            if am:
                m["input_delay"] = max(m["input_delay"], int(am["delay"]))
                continue
            lm = DELAY_LINE.search(line)
            if lm:
                m["input_delay"] = max(m["input_delay"], int(lm["delay"]))
    return m


def trace_diff(traces: dict[str, list[str]]) -> dict:
    """First differing line between peers' confirmed-state traces."""
    names = sorted(traces)
    result: dict[str, Any] = {"ticks": 0, "diverged": False, "first_divergence_line": None}
    if len(names) < 2:
        return result
    ref = traces[names[0]]
    result["ticks"] = min(len(t) for t in traces.values())
    for other_name in names[1:]:
        for i, (a, b) in enumerate(zip(ref, traces[other_name], strict=False)):
            if a != b:
                result["diverged"] = True
                result["first_divergence_line"] = i
                result["diverged_peer"] = other_name
                return result
    return result


def _percentile(sorted_vals: list[float], p: float) -> float:
    """Linear-interpolation percentile; sorted_vals must be sorted."""
    if not sorted_vals:
        return 0.0
    k = (len(sorted_vals) - 1) * p / 100.0
    lo = int(k)
    hi = min(lo + 1, len(sorted_vals) - 1)
    return sorted_vals[lo] + (sorted_vals[hi] - sorted_vals[lo]) * (k - lo)


def summarize(history: Path, days: int) -> int:
    """Reduce the JSONL history to dashboard stats: per-key P50/P95/P99,
    run count, worst desync rate, 30-day window by default."""
    records = []
    cutoff = datetime.now(UTC).timestamp() - days * 86400
    for line in history.read_text(encoding="utf-8", errors="replace").splitlines():
        try:
            rec = json.loads(line)
            ts = datetime.fromisoformat(rec["ts"]).timestamp()
            if ts >= cutoff:
                records.append(rec)
        except (ValueError, KeyError):
            continue
    keys = ["desync_rate", "worst_max_rollback", "rollback_frames_per_tick",
            "input_delay", "late_inputs"]
    summary: dict[str, Any] = {
        "window_days": days,
        "runs": len(records),
        "desynced_runs": sum(1 for r in records if r.get("diverged")),
    }
    for k in keys:
        vals = sorted(float(r.get(k, 0.0)) for r in records)
        summary[k] = {"p50": _percentile(vals, 50), "p95": _percentile(vals, 95),
                      "p99": _percentile(vals, 99), "max": vals[-1] if vals else 0.0}
    print(json.dumps(summary, indent=2))
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", type=Path,
                    help="run directory holding *.log and *.trace files")
    ap.add_argument("--out", type=Path, default=None,
                    help="metrics.json destination (default: <dir>/metrics.json)")
    ap.add_argument("--fail-on-desync", action="store_true",
                    help="exit 1 if any peer desynced or traces diverged")
    ap.add_argument("--history", type=Path, default=None,
                    help="JSONL file to append this run's record to (keeps the "
                         "rolling 30-day window; pruning is left to the reader)")
    ap.add_argument("--label", type=str, default="",
                    help="run label stored in the history record (e.g. profile name)")
    ap.add_argument("--summarize", type=Path, default=None, metavar="JSONL",
                    help="reduce a history file to P50/P95/P99 per metric "
                         "and exit (dashboard feed)")
    ap.add_argument("--days", type=int, default=30,
                    help="--summarize window in days (default 30)")
    ap.add_argument("--gate", type=Path, default=None, metavar="JSON",
                    help="baseline file to gate this run against "
                         "({label: {metric: value}}); labels absent from "
                         "the baseline are noted, not failed")
    ap.add_argument("--tolerance", type=float, default=0.25,
                    help="numeric metric headroom over baseline before "
                         "--gate fails (default 0.25 = +25%%)")
    a = ap.parse_args()
    if a.summarize is not None:
        return summarize(a.summarize, a.days)
    if a.dir is None:
        ap.error("--dir is required unless --summarize is given")

    peers = {}
    for log in sorted(a.dir.glob("*.log")):
        peers[log.stem] = peer_metrics(log)

    traces = {t.stem: t.read_text(encoding="utf-8", errors="replace").splitlines()
              for t in sorted(a.dir.glob("*.trace"))}
    trace_result = trace_diff(traces)

    compared = max(0, len(traces) - 1)
    diverged_peers = 1 if trace_result["diverged"] else 0
    total_resim = sum(p["resim_ticks"] for p in peers.values())
    total_ticks = max((p["ticks"] for p in peers.values()), default=0)

    metrics = {
        "peers": peers,
        "traces": trace_result,
        "desync_rate": (diverged_peers / compared) if compared else 0.0,
        "worst_max_rollback": max((p["max_rollback"] for p in peers.values()), default=0),
        "rollback_frames_per_tick": (total_resim / total_ticks) if total_ticks else 0.0,
        # Latency surface: worst configured input-delay across peers +
        # inputs that arrived too late to pace (rollback absorbed them).
        "input_delay": max((p["input_delay"] for p in peers.values()), default=0),
        "late_inputs": max((p["late_inputs"] for p in peers.values()), default=0),
        "dropped_datagrams": max((p["dropped"] for p in peers.values()), default=0),
        "stalls": max((p["stalls"] for p in peers.values()), default=0),
        # Frame-cost surface (only nonzero when SSB64_PERF_LOG=1 ran).
        "frame_p50_us": max((p["frame_p50_us"] for p in peers.values()), default=0),
        "frame_p95_us": max((p["frame_p95_us"] for p in peers.values()), default=0),
        "frame_p99_us": max((p["frame_p99_us"] for p in peers.values()), default=0),
    }

    out = a.out or (a.dir / "metrics.json")
    out.write_text(json.dumps(metrics, indent=2) + "\n", encoding="utf-8")
    if a.history is not None:
        a.history.parent.mkdir(parents=True, exist_ok=True)
        record = {
            "ts": datetime.now(UTC).isoformat(timespec="seconds"),
            "label": a.label or a.dir.name,
            "ticks": trace_result["ticks"],
            "diverged": trace_result["diverged"],
            "desync_rate": metrics["desync_rate"],
            "worst_max_rollback": metrics["worst_max_rollback"],
            "rollback_frames_per_tick": metrics["rollback_frames_per_tick"],
            "input_delay": metrics["input_delay"],
            "late_inputs": metrics["late_inputs"],
            "stalls": metrics["stalls"],
            "dropped_datagrams": metrics["dropped_datagrams"],
            "frame_p95_us": metrics["frame_p95_us"],
        }
        with a.history.open("a", encoding="utf-8") as hf:
            hf.write(json.dumps(record) + "\n")
        # Append-only would grow forever under scheduled CI — keep the
        # newest records so the file stays bounded. Rewrite via tmp +
        # replace so a crash can't truncate the live history.
        try:
            lines = a.history.read_text(encoding="utf-8",
                                      errors="replace").splitlines()
            if len(lines) > 2000:
                tmp = a.history.with_suffix(a.history.suffix + ".tmp")
                tmp.write_text("\n".join(lines[-2000:]) + "\n",
                               encoding="utf-8")
                tmp.replace(a.history)
        except OSError as exc:
            print(f"history prune skipped: {exc}", file=sys.stderr)
        print(f"history -> {a.history}")
    print(f"metrics -> {out}")
    print(f"  ticks={trace_result['ticks']} diverged={trace_result['diverged']} "
          f"desync_rate={metrics['desync_rate']:.4f} "
          f"worst_max_rollback={metrics['worst_max_rollback']} "
          f"resim/tick={metrics['rollback_frames_per_tick']:.3f}")

    bad = trace_result["diverged"] or any(p["desync"] for p in peers.values())
    if a.fail_on_desync and bad:
        return 1

    if a.gate is not None:
        return gate(metrics, a.gate, a.label or a.dir.name, a.tolerance)
    return 0


# Numeric counters gated with headroom — determinism booleans gate hard.
GATE_NUMERIC = ("worst_max_rollback", "rollback_frames_per_tick",
                "input_delay", "late_inputs", "dropped_datagrams",
                "stalls", "frame_p50_us", "frame_p95_us", "frame_p99_us")


def gate(metrics: dict, baseline_path: Path, label: str,
         tolerance: float) -> int:
    """Compare this run's metrics to baseline[label]; 0 pass / 1 regress."""
    try:
        baseline = json.loads(baseline_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        baseline = {}
    if label not in baseline:
        print(f"gate: no baseline entry for '{label}' — metrics recorded, "
              f"not gated (nightly refresh pins it)")
        return 0
    base = baseline[label]
    fails = []
    # Determinism regressions are hard failures regardless of tolerance.
    if base.get("desync_rate", 0.0) == 0.0 and metrics["desync_rate"] > 0:
        fails.append(f"desync_rate regressed: {metrics['desync_rate']} "
                     f"(baseline was clean)")
    if not base.get("diverged", False) and metrics["traces"]["diverged"]:
        fails.append(f"traces diverged (baseline was in sync, first diff "
                     f"line {metrics['traces']['first_divergence_line']})")
    for key in GATE_NUMERIC:
        ref = float(base.get(key, 0) or 0)
        cur = float(metrics.get(key, 0) or 0)
        # A zero baseline can't gate "how much worse" — skip; the first
        # measured value becomes the baseline at next refresh.
        if ref <= 0:
            continue
        limit = ref * (1.0 + tolerance)
        if cur > limit:
            fails.append(f"{key}: {cur:g} > baseline {ref:g} "
                         f"(+{tolerance:.0%} headroom = {limit:g})")
    if fails:
        print(f"gate FAIL [{label}]:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print(f"gate PASS [{label}] (tolerance +{tolerance:.0%})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
