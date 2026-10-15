#!/usr/bin/env python3
"""Compatibility-handshake test: a LAN host and a joiner whose
SSB64_COMPAT_TOKEN is forced.

Default mode: a different token -> somebody must reject with a
build/mod-mismatch failure and no match may start.
--legacy mode: the joiner sends a pre-handshake PICK (no token field) and
answers host tokens like an old build -> the match must still start.

Replaces scripts/testing/netplay_compat_test.ps1 (kept as a wrapper).
Exit: 0 expected outcome, 2 unexpected outcome.
"""

import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "support" / "harness" / "netplay"))
import session_runner as sr  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--legacy", action="store_true")
    ap.add_argument("--port", type=int, default=47500)
    ap.add_argument("--build-dir", type=Path, default=sr.DEFAULT_BUILD_DIR)
    ap.add_argument("--out-dir", type=Path, default=sr.ROOT / "out" / "logs" / "compat-test")
    a = ap.parse_args()

    out = sr.clean_out(a.out_dir.resolve())
    build = a.build_dir.resolve()

    def start_peer(name, fighter, extra):
        env = {
            "SSB64_LOBBY_FKIND": fighter, "SSB64_LOBBY_AUTOSTART": "1",
            "SSB64_LOBBY_MIN_PLAYERS": "2", "SSB64_NETPLAY_FUZZ": "111",
            "SSB64_MUTE": "1", "SSB64_NO_UPDATE_CHECK": "1",
            "SSB64_LOG_PATH": str(out / f"{name}.log"),
            "SSB64_SAVE_PATH": str(out / f"{name}.sav"),
        }
        env.update(extra)
        return sr.spawn_game(build, name, env, out)

    host = start_peer("host", 0, {"SSB64_LAN_HOST": a.port})
    deadline = time.time() + (30 if a.legacy else 20)
    client = start_peer("client1", 1, {
        "SSB64_LAN_JOIN": f"127.0.0.1:{a.port}",
        "SSB64_COMPAT_TOKEN": "none" if a.legacy else "bogus:0000000000000000",
    })
    # Poll the logs for an outcome instead of fixed sleeps, bounded by the
    # deadline.
    while time.time() < deadline:
        time.sleep(0.5)
        if (sr.log_matches(out / "host.log", r"mismatch|online match")
                or sr.log_matches(out / "client1.log", r"mismatch|online match")):
            break
    sr.stop_all([host, client], grace=2)

    for f in ("host", "client1"):
        print(f"--- {f}")
        for l in sr.tail_log(out / f"{f}.log",
                             r"SSB64 Lobby:|mismatch|online match"):
            print(l)

    mismatch = (sr.log_matches(out / "host.log", r"mismatch")
                or sr.log_matches(out / "client1.log", r"mismatch"))
    matched = (sr.log_matches(out / "host.log", r"online match")
               or sr.log_matches(out / "client1.log", r"online match"))

    if a.legacy:
        if matched and not mismatch:
            print("PASS: legacy (tokenless) peer interoperates - match started")
            return 0
        print("FAIL: legacy peer was rejected or the match never started")
        return 2
    # Either side may reject first: a client can see the host's PICK before
    # its own PICK reaches the host. Somebody rejecting + no match = pass.
    if mismatch and not matched:
        print("PASS: incompatible build/mod token rejected before the match")
        return 0
    print("FAIL: mismatch was not rejected")
    return 2


if __name__ == "__main__":
    sys.exit(main())
