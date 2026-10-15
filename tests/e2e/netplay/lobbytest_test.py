#!/usr/bin/env python3
"""1v1 e2e against the lobby: host creates a room, client joins by code,
they hole-punch, pick fighters, start a rollback match with fuzzed inputs,
and the two confirmed-state traces are compared.

Replaces scripts/testing/netplay_lobbytest.ps1 (kept as a wrapper).
Exit: 0 in sync, 1 desync, 2 no match / no trace.
"""

import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "support" / "harness" / "netplay"))
import session_runner as sr


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=int, default=60)
    ap.add_argument("--latency-ms", type=int, default=0)
    ap.add_argument("--jitter-ms", type=int, default=0)
    ap.add_argument("--loss-pct", type=int, default=0)
    ap.add_argument("--server", default="")
    ap.add_argument("--host-fighter", type=int, default=0)
    ap.add_argument("--client-fighter", type=int, default=1)
    ap.add_argument("--stage", type=int, default=0)
    ap.add_argument("--force-relay", action="store_true")
    ap.add_argument("--addr-jitter", type=int, default=0,
                    help="client shifts its heap layout like a different PC would")
    ap.add_argument("--host-dir", type=Path, default=None,
                    help="run the host from another install (its own cfg + save)")
    ap.add_argument("--build-dir", type=Path, default=sr.DEFAULT_BUILD_DIR)
    ap.add_argument("--out-dir", type=Path, default=sr.ROOT / "out" / "logs" / "lobby-test")
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
    print(f"lobby server: {server}")

    def start_peer(role, fighter, fuzz):
        env = {
            "SSB64_LOBBY_CODE_FILE": str(out / "code.txt"),
            "SSB64_LOBBY_FKIND": fighter, "SSB64_LOBBY_STAGE": a.stage,
            "SSB64_NETPLAY_FUZZ": fuzz, "SSB64_MUTE": "1",
            "SSB64_NO_UPDATE_CHECK": "1",
            "SSB64_NETPLAY_SIM_LATENCY_MS": a.latency_ms,
            "SSB64_NETPLAY_SIM_JITTER_MS": a.jitter_ms,
            "SSB64_NETPLAY_SIM_LOSS_PCT": a.loss_pct,
            "SSB64_NETPLAY_SIM_SEED": fuzz,
            "SSB64_NETPLAY_CONFIRMED_TRACE": str(out / f"{role}.trace"),
            "SSB64_LOG_PATH": str(out / f"{role}.log"),
            "SSB64_LOBBY_FORCE_RELAY": "1" if a.force_relay else "",
        }
        exe_dir = None
        if role == "host":
            env["SSB64_LOBBY_HOST"] = server
            env["SSB64_LOBBY_AUTOSTART"] = "1"
            if a.host_dir:
                exe_dir = a.host_dir
        else:
            env["SSB64_LOBBY_JOIN"] = server
            if a.addr_jitter:
                env["SSB64_ADDR_JITTER"] = a.addr_jitter
        if exe_dir is None:
            env["SSB64_SAVE_PATH"] = str(out / f"{role}.sav")
        return sr.spawn_game(build, role, env, out, exe_dir=exe_dir)

    procs = [start_peer("host", a.host_fighter, 111)]
    time.sleep(2)
    procs.append(start_peer("client", a.client_fighter, 222))
    time.sleep(a.seconds)
    sr.stop_all(procs)
    sr.kill_tree(server_proc)

    for n in ("host", "client"):
        print(f"--- {n}")
        for l in sr.tail_log(out / f"{n}.log",
                             r"SSB64 Lobby:|SSB64 WS:|online match|netplay tick|"
                             r"DESYNC|rollback FAILED"):
            print(l)

    code, msg = sr.compare_traces(["host", "client"], out, min_fields=13)
    print(msg)
    return code


if __name__ == "__main__":
    sys.exit(main())
