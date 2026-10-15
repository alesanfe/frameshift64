#!/usr/bin/env python3
"""Two BattleShip instances over localhost UDP with rollback netplay,
seeded random inputs on both sides and an optional simulated bad network,
then the two peers' confirmed-state traces are diffed. Identical traces =
never desynced.

Replaces scripts/testing/netplay_localtest.ps1 (kept as a wrapper).
Exit: 0 in sync / expected outcome, 1 desync or crash, 2 no trace.
"""

import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "support" / "harness" / "netplay"))
import session_runner as sr  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=int, default=60)
    ap.add_argument("--delay", type=int, default=2)
    ap.add_argument("--latency-ms", type=int, default=0)
    ap.add_argument("--jitter-ms", type=int, default=0)
    ap.add_argument("--loss-pct", type=int, default=0)
    ap.add_argument("--dup-pct", type=int, default=0)
    ap.add_argument("--reorder-pct", type=int, default=0)
    ap.add_argument("--reorder-window-ms", type=int, default=100)
    ap.add_argument("--corrupt-pct", type=int, default=0)
    ap.add_argument("--seed", type=int, default=4242)
    ap.add_argument("--base-port", type=int, default=7801)
    ap.add_argument("--inject-desync", type=int, default=-1)
    ap.add_argument("--mutant", default="",
                    choices=["", "accept_bad_session", "accept_bad_checksum",
                             "no_forward_guard"],
                    help="disables one validation — the run must DESYNC")
    ap.add_argument("--packet-fuzz", type=int, default=0,
                    help="inject <n> seeded pseudo-random datagrams per drain")
    ap.add_argument("--build-dir", type=Path, default=sr.DEFAULT_BUILD_DIR)
    ap.add_argument("--out-dir", type=Path, default=sr.ROOT / "out" / "logs" / "netplay-test")
    a = ap.parse_args()

    out = a.out_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    sr.clean_out(out, patterns=("*.trace", "*.log"))
    build = a.build_dir.resolve()

    def start_peer(local, remote, bind_port, peer_port, is_host, fuzz, name):
        env = {
            "SSB64_NETPLAY": "1", "SSB64_NETPLAY_BOOTSTRAP": "1",
            "SSB64_NETPLAY_HOST": is_host, "SSB64_MUTE": "1",
            "SSB64_NO_UPDATE_CHECK": "1",
            "SSB64_NETPLAY_LOCAL_PLAYER": local,
            "SSB64_NETPLAY_REMOTE_PLAYER": remote,
            "SSB64_NETPLAY_BIND": f"127.0.0.1:{bind_port}",
            "SSB64_NETPLAY_PEER": f"127.0.0.1:{peer_port}",
            "SSB64_NETPLAY_DELAY": a.delay, "SSB64_NETPLAY_SEED": a.seed,
            "SSB64_NETPLAY_FUZZ": fuzz,
            "SSB64_NETPLAY_SIM_LATENCY_MS": a.latency_ms,
            "SSB64_NETPLAY_SIM_JITTER_MS": a.jitter_ms,
            "SSB64_NETPLAY_SIM_LOSS_PCT": a.loss_pct,
            "SSB64_NETPLAY_SIM_SEED": fuzz,
            "SSB64_NETPLAY_SIM_DUP_PCT": a.dup_pct,
            "SSB64_NETPLAY_SIM_REORDER_PCT": a.reorder_pct,
            "SSB64_NETPLAY_SIM_REORDER_WINDOW_MS": a.reorder_window_ms,
            "SSB64_NETPLAY_SIM_CORRUPT_PCT": a.corrupt_pct,
            "SSB64_NETPLAY_CONFIRMED_TRACE": str(out / f"{name}.trace"),
            "SSB64_LOG_PATH": str(out / f"{name}.log"),
            "SSB64_SAVE_PATH": str(out / f"{name}.sav"),
            "SSB64_NETPLAY_TEST_DESYNC": a.inject_desync if is_host else "-1",
            "SSB64_NETPLAY_MUTANT": a.mutant,
            "SSB64_NETPLAY_PACKETFUZZ": a.packet_fuzz,
        }
        return sr.spawn_game(build, name, env, out)

    procs = [
        start_peer(0, 1, a.base_port, a.base_port + 1, 1, 111, "host"),
        start_peer(1, 0, a.base_port + 1, a.base_port, 0, 222, "client"),
    ]
    time.sleep(a.seconds)
    sr.stop_all(procs)

    ta = sr.read_trace(out, "host")
    tb = sr.read_trace(out, "client")
    if ta is None or tb is None:
        print("missing trace")
        return 2
    n = min(len(ta), len(tb))
    print(f"host traced={len(ta)} client traced={len(tb)} compared={n}")
    for f in ("host", "client"):
        for l in sr.tail_log(
                out / f"{f}.log",
                r"netplay session|netplay tick|session stop|rollback FAILED|"
                r"DESYNC|TEST injecting|VECTORED|abort|FATAL", last=3):
            print(f"{f}> {l}")

    if a.packet_fuzz > 0:
        # Packet-fuzz oracle: tier-3 packets are indistinguishable from real
        # input traffic, so a desync is EXPECTED. A crash or a missing trace
        # is the failure.
        crashed = [f for f in ("host", "client")
                   if sr.log_matches(out / f"{f}.log",
                                     r"unhandled|second-chance|abort\(\)")]
        if crashed:
            print(f"CRASH during packet fuzz: {', '.join(crashed)}")
            return 1
        if n == 0:
            print("no ticks traced")
            return 2
        print(f"PACKETFUZZ survived {a.packet_fuzz} injected datagrams/drain, "
              f"{n} ticks traced (desync expected)")
        return 0

    code, msg = sr.compare_traces(["host", "client"], out, min_fields=13)
    print(msg)
    return code


if __name__ == "__main__":
    sys.exit(main())
