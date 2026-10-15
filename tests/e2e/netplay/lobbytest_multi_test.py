#!/usr/bin/env python3
"""Multi-player e2e against the lobby (protocol v2): a host creates a room
and up to three clients join by code. The host waits for --players
participants, starts a rollback match with fuzzed inputs (star topology:
clients link only to the host, which forwards input datagrams), then every
peer's confirmed-state trace is compared pairwise.

Replaces scripts/testing/netplay_lobbytest_multi.ps1 (kept as a wrapper).
Exit: 0 in sync, 1 desync, 2 no match / missing trace.
"""

import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "support" / "harness" / "netplay"))
import session_runner as sr  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--players", type=int, default=3)
    ap.add_argument("--seconds", type=int, default=60)
    ap.add_argument("--latency-ms", type=int, default=0)
    ap.add_argument("--jitter-ms", type=int, default=0)
    ap.add_argument("--loss-pct", type=int, default=0)
    ap.add_argument("--server", default="")
    ap.add_argument("--stage", type=int, default=0)
    ap.add_argument("--force-relay", action="store_true")
    ap.add_argument("--debug-stages", action="store_true")
    ap.add_argument("--headless", action="store_true",
                    help="run every peer with SSB64_HEADLESS=1 (no rendering, "
                         "timer-paced frames — fits more instances on one machine)")
    ap.add_argument("--build-dir", type=Path, default=sr.DEFAULT_BUILD_DIR)
    ap.add_argument("--out-dir", type=Path, default=sr.ROOT / "out" / "logs" / "lobby-test-multi")
    a = ap.parse_args()

    out = sr.clean_out(a.out_dir.resolve())
    build = a.build_dir.resolve()

    server = a.server
    server_proc = None
    if not server:
        server = "ws://127.0.0.1:8799"
        if not sr.lobby_healthy():
            server_proc = sr.ensure_lobby(out)
            if server_proc is None and not sr.lobby_healthy():
                print("lobby server did not start")
                return 2
    print(f"lobby server: {server}   players: {a.players}")

    def start_peer(name, fighter, fuzz, is_host):
        env = {
            "SSB64_LOBBY_CODE_FILE": str(out / "code.txt"),
            "SSB64_LOBBY_FKIND": fighter, "SSB64_LOBBY_STAGE": a.stage,
            "SSB64_NETPLAY_FUZZ": fuzz, "SSB64_MUTE": "1",
            "SSB64_NO_UPDATE_CHECK": "1",
            "SSB64_NETPLAY_SIM_LATENCY_MS": a.latency_ms,
            "SSB64_NETPLAY_SIM_JITTER_MS": a.jitter_ms,
            "SSB64_NETPLAY_SIM_LOSS_PCT": a.loss_pct,
            "SSB64_NETPLAY_SIM_SEED": fuzz,
            "SSB64_NETPLAY_CONFIRMED_TRACE": str(out / f"{name}.trace"),
            "SSB64_LOG_PATH": str(out / f"{name}.log"),
            "SSB64_SAVE_PATH": str(out / f"{name}.sav"),
        }
        if a.force_relay:
            env["SSB64_LOBBY_FORCE_RELAY"] = "1"
        if a.headless:
            env["SSB64_HEADLESS"] = "1"
        if a.debug_stages:
            env["SSB64_NETPLAY_DEBUG_STAGES"] = "1"
        if is_host:
            env["SSB64_LOBBY_HOST"] = server
            env["SSB64_LOBBY_AUTOSTART"] = "1"
            env["SSB64_LOBBY_MIN_PLAYERS"] = a.players
        else:
            env["SSB64_LOBBY_JOIN"] = server
        return sr.spawn_game(build, name, env, out)

    procs = [start_peer("host", 0, 111, True)]
    time.sleep(3)
    for i in range(1, a.players):
        procs.append(start_peer(f"client{i}", i, 200 + i, False))
        time.sleep(0.8)
    time.sleep(a.seconds)
    sr.stop_all(procs)
    sr.kill_tree(server_proc)

    names = ["host"] + [f"client{i}" for i in range(1, a.players)]
    for n in names:
        print(f"--- {n}")
        for l in sr.tail_log(out / f"{n}.log",
                             r"SSB64 Lobby:|SSB64 WS:|online match|netplay tick|"
                             r"DESYNC|rollback FAILED|dropping|silent"):
            print(l)

    code, msg = sr.compare_traces(names, out, min_fields=13)
    print(msg)
    return code


if __name__ == "__main__":
    sys.exit(main())
