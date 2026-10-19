#!/usr/bin/env python3
"""Refresh the committed netplay metrics baseline from a finished run.

Reads <matrix-dir>/<label>/metrics.json for every label directory and
upserts the gate-relevant keys into the baseline file — the same keys
netplay_metrics.py --gate compares. Only runs under the nightly workflow
(schedule event), where a completed matrix means the numbers came off a
green run; PRs never write the baseline.

Usage:
    python tools/analysis/update_metrics_baseline.py \
        out/logs/netplay-matrix tests/support/metrics-baseline.json
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

# Same surface the gate compares — keep the baseline minimal so unrelated
# metric additions don't churn the file.
BASELINE_KEYS = ("desync_rate", "worst_max_rollback",
                 "rollback_frames_per_tick", "input_delay", "late_inputs",
                 "dropped_datagrams", "stalls",
                 "frame_p50_us", "frame_p95_us", "frame_p99_us")


def main() -> int:
    matrix_dir = Path(sys.argv[1])
    baseline_path = Path(sys.argv[2])
    try:
        baseline = json.loads(baseline_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        baseline = {}

    updated = []
    for metrics_path in sorted(matrix_dir.glob("*/metrics.json")):
        label = metrics_path.parent.name
        try:
            m = json.loads(metrics_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            continue
        entry = {k: m.get(k, 0) for k in BASELINE_KEYS}
        entry["diverged"] = bool(m.get("traces", {}).get("diverged", False))
        baseline[label] = entry
        updated.append(label)

    baseline_path.parent.mkdir(parents=True, exist_ok=True)
    baseline_path.write_text(json.dumps(baseline, indent=2) + "\n",
                             encoding="utf-8")
    print(f"baseline <- {len(updated)} labels: {', '.join(updated)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
