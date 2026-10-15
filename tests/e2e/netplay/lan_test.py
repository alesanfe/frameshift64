#!/usr/bin/env python3
"""LAN e2e: direct IP, no lobby server, no STUN, no WebSocket. The host
binds a UDP port; clients dial SSB64_LAN_JOIN=ip:port and the host assigns
room slots on first contact (SSBL HELLO -> HELLOACK slot).

Replaces scripts/testing/netplay_lan_test.ps1 (kept as a wrapper).
Exit: 0 in sync, 1 desync, 2 no match / missing trace.
"""

import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "support" / "harness" / "netplay"))
import session_runner as sr


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--players", type=int, default=2)
    ap.add_argument("--seconds", type=int, default=45)
    ap.add_argument("--port", type=int, default=47100)
    ap.add_argument("--latency-ms", type=int, default=0)
    ap.add_argument("--jitter-ms", type=int, default=0)
    ap.add_argument("--loss-pct", type=int, default=0)
    ap.add_argument("--profile", default=None,
                    help="named impairment profile (tests/support/config/network-profiles.yml)")
    ap.add_argument("--debug-stages", action="store_true")
    ap.add_argument("--replay-frames", type=int, default=0)
    ap.add_argument("--desync-tick", type=int, default=-1,
                    help="clients inject a desync at this tick (negative = off)")
    ap.add_argument("--build-dir", type=Path, default=sr.DEFAULT_BUILD_DIR)
    ap.add_argument("--out-dir", type=Path, default=sr.ROOT / "out" / "logs" / "lan-test")
    a = ap.parse_args()

    out = sr.clean_out(a.out_dir.resolve())
    build = a.build_dir.resolve()
    print(f"LAN test: host on 127.0.0.1:{a.port}   players: {a.players}")

    def start_peer(name, fighter, fuzz, is_host):
        env = {
            "SSB64_LOBBY_FKIND": fighter, "SSB64_LOBBY_STAGE": "0",
            "SSB64_LOBBY_AUTOSTART": "1", "SSB64_LOBBY_MIN_PLAYERS": a.players,
            "SSB64_NETPLAY_FUZZ": fuzz, "SSB64_MUTE": "1",
            "SSB64_NO_UPDATE_CHECK": "1",
            "SSB64_NETPLAY_SIM_SEED": fuzz,
            "SSB64_NETPLAY_CONFIRMED_TRACE": str(out / f"{name}.trace"),
            "SSB64_LOG_PATH": str(out / f"{name}.log"),
            "SSB64_SAVE_PATH": str(out / f"{name}.sav"),
        }
        if a.debug_stages:
            env["SSB64_NETPLAY_DEBUG_STAGES"] = "1"
        if a.profile:
            env.update(sr.load_profile(a.profile))   # overrides individual flags
        else:
            env["SSB64_NETPLAY_SIM_LATENCY_MS"] = a.latency_ms
            env["SSB64_NETPLAY_SIM_JITTER_MS"] = a.jitter_ms
            env["SSB64_NETPLAY_SIM_LOSS_PCT"] = a.loss_pct
        if a.replay_frames > 0:
            env["SSB64_REPLAY_RECORD_FRAMES"] = a.replay_frames
        if not is_host and a.desync_tick >= 0:
            env["SSB64_NETPLAY_TEST_DESYNC"] = a.desync_tick
        if is_host:
            env["SSB64_LAN_HOST"] = a.port
        else:
            env["SSB64_LAN_JOIN"] = f"127.0.0.1:{a.port}"
        return sr.spawn_game(build, name, env, out)

    procs = [start_peer("host", 0, 111, True)]
    time.sleep(2)
    for i in range(1, a.players):
        procs.append(start_peer(f"client{i}", i, 200 + i, False))
        time.sleep(0.8)
    time.sleep(a.seconds)
    sr.stop_all(procs)

    names = ["host"] + [f"client{i}" for i in range(1, a.players)]
    for n in names:
        print(f"--- {n}")
        for l in sr.tail_log(out / f"{n}.log",
                             r"SSB64 Lobby:|online match|netplay tick|DESYNC|"
                             r"rollback FAILED|dropping|silent"):
            print(l)

    code, msg = sr.compare_traces(names, out, min_fields=13)
    if code == 0:
        msg += " (LAN direct)"
    print(msg)
    return code


if __name__ == "__main__":
    sys.exit(main())
