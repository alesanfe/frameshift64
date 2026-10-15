#!/usr/bin/env python3
"""Train-from-replay smoke test: plays back a recorded .ssr and, with
--takeover <player>:<tick>, checks the slot flips to live input at the
requested tick. Without --takeover it verifies clean playback.

Replaces scripts/testing/replay_takeover_test.ps1 (kept as a wrapper).
Exit: 0 verdict found, 2 no verdict.
"""

import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "support" / "harness" / "netplay"))
import session_runner as sr


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--replay", type=Path, required=True)
    ap.add_argument("--takeover", default="", help="e.g. '1:100'")
    ap.add_argument("--seconds", type=int, default=20)
    ap.add_argument("--build-dir", type=Path, default=sr.DEFAULT_BUILD_DIR)
    ap.add_argument("--out-dir", type=Path, default=sr.ROOT / "out" / "logs" / "takeover-test")
    a = ap.parse_args()

    out = a.out_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    log = out / "playback.log"
    log.unlink(missing_ok=True)
    build = a.build_dir.resolve()

    env = {
        "SSB64_REPLAY_PLAY": str(a.replay.resolve()),
        "SSB64_MUTE": "1",
        "SSB64_NO_UPDATE_CHECK": "1",
        "SSB64_LOG_PATH": str(log),
    }
    if a.takeover:
        env["SSB64_REPLAY_TAKEOVER"] = a.takeover

    p = sr.spawn_game(build, "playback", env, out)
    time.sleep(a.seconds)
    sr.stop_all([p], grace=2)

    for l in sr.tail_log(log, r"Replay|takeover|playback|FAIL", last=12):
        print(l)

    if a.takeover:
        if sr.log_matches(log, r"takeover player="):
            print("PASS: takeover fired")
            return 0
        print("FAIL: takeover never fired")
        return 2
    if sr.log_matches(log, r"playback verify.*result=PASS"):
        print("PASS: replay verified")
        return 0
    print("FAIL: no verification verdict")
    return 2


if __name__ == "__main__":
    sys.exit(main())
