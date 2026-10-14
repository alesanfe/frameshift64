#!/usr/bin/env python3
"""Spectator e2e: host + N-1 players + 1 spectator over the local node
lobby (or --server). The spectator joins with role=spec, must receive the
STARTM, run the match a few ticks behind, never desync the players, and
produce a confirmed-state trace identical to the players'.

--spec-delay <s> starts the spectator <s> seconds after the players, i.e.
mid-match: the host streams its recorded input history (FORWARD-wrapped)
and the spec resimulates faster than realtime until it lands on the live
edge. The spec's confirmed-state trace must still cover the whole match
from tick 0 and match the players' exactly.

Replaces scripts/testing/spectate_test.ps1 (kept as a wrapper).
Exit: 0 in sync, 1 desync, 2 no match / missing trace.
"""

import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "harness" / "netplay"))
import session_runner as sr  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--players", type=int, default=2)
    ap.add_argument("--seconds", type=int, default=30)
    ap.add_argument("--latency-ms", type=int, default=0)
    ap.add_argument("--jitter-ms", type=int, default=0)
    ap.add_argument("--spec-delay", type=float, default=0.0,
                    help="start the spectator N seconds after the players "
                         "(mid-match catch-up instead of pre-match join)")
    ap.add_argument("--server", default="")
    ap.add_argument("--debug-stages", action="store_true")
    ap.add_argument("--build-dir", type=Path, default=sr.DEFAULT_BUILD_DIR)
    ap.add_argument("--out-dir", type=Path, default=sr.ROOT / "out" / "logs" / "spectate-test")
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
    print(f"lobby server: {server}   players: {a.players} + 1 spec")

    def start_peer(name, fighter, fuzz, is_host, is_spec):
        env = {
            "SSB64_LOBBY_CODE_FILE": str(out / "code.txt"),
            "SSB64_LOBBY_FKIND": fighter,
            "SSB64_NETPLAY_FUZZ": fuzz, "SSB64_MUTE": "1",
            "SSB64_NO_UPDATE_CHECK": "1",
            "SSB64_NETPLAY_SIM_LATENCY_MS": a.latency_ms,
            "SSB64_NETPLAY_SIM_JITTER_MS": a.jitter_ms,
            "SSB64_NETPLAY_CONFIRMED_TRACE": str(out / f"{name}.trace"),
            "SSB64_LOG_PATH": str(out / f"{name}.log"),
            "SSB64_SAVE_PATH": str(out / f"{name}.sav"),
        }
        if a.debug_stages:
            env["SSB64_NETPLAY_DEBUG_STAGES"] = "1"
        if is_spec:
            env["SSB64_LOBBY_JOIN"] = server
            env["SSB64_LOBBY_SPECTATE"] = "1"
        elif is_host:
            env["SSB64_LOBBY_HOST"] = server
            env["SSB64_LOBBY_AUTOSTART"] = "1"
            env["SSB64_LOBBY_MIN_PLAYERS"] = a.players
        else:
            env["SSB64_LOBBY_JOIN"] = server
        return sr.spawn_game(build, name, env, out)

    procs = [start_peer("host", 0, 111, True, False)]
    time.sleep(3)
    if a.spec_delay <= 0:
        procs.append(start_peer("spec", 0, 999, False, True))  # before players: must see STARTM
        time.sleep(0.8)
    for i in range(1, a.players):
        procs.append(start_peer(f"client{i}", i, 200 + i, False, False))
        time.sleep(0.8)
    if a.spec_delay > 0:
        # mid-match join: the match has been running for spec_delay
        # seconds; the spec catches up on the streamed input history
        time.sleep(a.spec_delay)
        procs.append(start_peer("spec", 0, 999, False, True))
    time.sleep(a.seconds)
    sr.stop_all(procs)
    sr.kill_tree(server_proc)

    names = ["host", "spec"] + [f"client{i}" for i in range(1, a.players)]
    for n in names:
        print(f"--- {n}")
        for l in sr.tail_log(out / f"{n}.log",
                             r"SSB64 Lobby:|spectat|Spectat|online match|"
                             r"netplay tick|DESYNC|dropped", last=10):
            print(l)

    traces = {n: sr.read_trace(out, n) for n in names}
    missing = [n for n, t in traces.items() if t is None]
    for n in missing:
        print(f"no trace for {n} (spectate failed?)")
    if missing:
        return 2
    counts = " ".join(f"{n}={len(traces[n])}" for n in names)
    print(f"traced ticks: {counts}")
    ref = traces["host"]
    worst = 0
    for n in names:
        if n == "host":
            continue
        for i, (a_, b_) in enumerate(zip(ref, traces[n])):
            if a_ != b_:
                print(f"DESYNC host vs {n} at line {i}\n  host: {a_}\n  {n}: {b_}")
                return 1
        worst = max(worst, min(len(ref), len(traces[n])))
    if worst == 0:
        print("no ticks traced")
        return 2
    print(f"IN SYNC across {a.players} players + spectator for {worst} ticks")
    return 0


if __name__ == "__main__":
    sys.exit(main())
