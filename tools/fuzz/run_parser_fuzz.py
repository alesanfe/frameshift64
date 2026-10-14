#!/usr/bin/env python3
"""run_parser_fuzz.py — deterministic mutation fuzzing for the port's
binary/JSON parsers (TD-010).

Drives the game binary through its test-hook fuzz entry point:

    SSB64_FUZZ_PARSER=<manifest|replay|json> SSB64_FUZZ_INPUT=<file> \
        ./BattleShip          # parse then exit; a crash is a finding

Parsers covered:
    manifest  mod manifest.json -> Ship::Archive::ParseManifestBuffer
    replay    .ssbreplay file   -> syNetReplayLoadDebugFile (netreplay.c)
    json      raw nlohmann::json::parse — the parser config + manifests share

Requires a build with -DSSB64_TEST_HOOKS=ON (default for dev builds;
release packaging compiles the hook out — check_release_binary.py verifies).

Usage:
    python tools/fuzz/run_parser_fuzz.py --parser manifest \
        --binary path/to/BattleShip --iters 2000 [--seed 1] [--keep-corpus]

Exit code: 0 clean, 1 = at least one crashing/hanging input (saved under
out/fuzz/crashes/), 2 = usage/binary missing.
"""
import argparse
import hashlib
import json
import os
import random
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT_DIR = ROOT / "out" / "fuzz"
SEEDS_DIR = ROOT / "tests" / "fuzz" / "corpora"

# ── Seed corpora ─────────────────────────────────────────────────────────

VALID_MANIFEST = json.dumps({
    "name": "fuzzmod",
    "author": "td-010",
    "version": "1.0.0",
    "description": "seed",
    "license": "MIT",
    "code-version": 1,
    "game-version": 0,
    "simulation": True,
    "main": "main.c",
    "binaries": {"win": "a.dll", "lin": "a.so"},
    "dependencies": ["other-mod@1.0"],
    "checksum": "0" * 64,
}).encode()


def synth_replay(frames: int = 8) -> bytes:
    """A valid .ssbreplay: SYNetReplayFileHeader (7 u32 LE) + metadata
    (80 bytes) + frames x players x SYNetInputFrame (12 bytes)."""
    MAGIC, VERSION, MAXCONTROLLERS = 0x53534E52, 1, 4
    meta_size, frame_size = 80, 12
    header = struct.pack("<7I", MAGIC, VERSION, meta_size, frame_size,
                         frames, MAXCONTROLLERS, 0xDEADBEEF)
    metadata = struct.pack("<10I", MAGIC, VERSION, 0, MAXCONTROLLERS,
                           0, 3, 0, 0, 0, 0xC0FFEE) + bytes(meta_size - 40)
    body = bytearray()
    for tick in range(frames):
        for _ in range(MAXCONTROLLERS):
            body += struct.pack("<IHbbBBBx", tick, 0, 0, 0, 0, 0, 1)
    return header + metadata + bytes(body)


SEEDS = {
    "manifest": [VALID_MANIFEST, b"{}", b'{"name":"x"}', b"[1,2,3]",
                 b'{"binaries":null,"dependencies":"x"}'],
    "replay":   [synth_replay(8), synth_replay(0), synth_replay(1)[:-1]],
    "json":     [b'{"a":1}', VALID_MANIFEST, b'[0,"x",true,null]', b'{}'],
}

# ── Mutator ──────────────────────────────────────────────────────────────

INTERESTING = [0, 1, 0x7F, 0x80, 0xFF, 0xFFFF // 256]


def mutate(buf: bytes, rng: random.Random) -> bytes:
    b = bytearray(buf)
    ops = rng.randint(1, 4)
    for _ in range(ops):
        if not b or rng.random() < 0.05:
            b = bytearray(rng.randbytes(rng.randint(0, 64)))
            continue
        op = rng.randrange(6)
        if op == 0:                        # byte flip
            i = rng.randrange(len(b))
            b[i] ^= 1 << rng.randrange(8)
        elif op == 1:                      # interesting value poke
            i = rng.randrange(len(b))
            b[i] = rng.choice(INTERESTING) & 0xFF
        elif op == 2 and len(b) > 2:       # truncate
            del b[rng.randrange(1, len(b)):]
        elif op == 3:                      # splice another position
            i = rng.randrange(len(b))
            j = rng.randrange(len(b))
            n = min(64, len(b) - j)
            b[i:i] = b[j:j + n]
        elif op == 4:                      # delete range
            i = rng.randrange(len(b))
            del b[i:i + rng.randint(1, min(64, len(b) - i))]
        else:                              # nudge a u32 field
            i = rng.randrange(len(b))
            for _ in range(rng.randint(1, 4)):
                if i < len(b):
                    b[i] = rng.randrange(256)
                    i += 1
    return bytes(b)


def run_one(binary: Path, parser: str, data: bytes, timeout: float) -> int:
    with tempfile.NamedTemporaryFile(delete=False, suffix=".fuzz") as tmp:
        tmp.write(data)
        path = tmp.name
    env = dict(os.environ)
    env["SSB64_FUZZ_PARSER"] = parser
    env["SSB64_FUZZ_INPUT"] = path
    try:
        proc = subprocess.run([str(binary)], env=env, timeout=timeout,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        rc = proc.returncode
    except subprocess.TimeoutExpired:
        rc = -999
    finally:
        os.unlink(path)
    return rc


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--parser", required=True, choices=sorted(SEEDS))
    ap.add_argument("--binary", required=True, type=Path,
                    help="BattleShip binary built with -DSSB64_TEST_HOOKS=ON")
    ap.add_argument("--iters", type=int, default=1000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--timeout", type=float, default=10.0)
    ap.add_argument("--seeds-dir", type=Path, default=SEEDS_DIR)
    ap.add_argument("--keep-corpus", action="store_true",
                    help="also save every mutated input to out/fuzz/corpus/<parser>/")
    args = ap.parse_args()

    if not args.binary.is_file():
        print(f"fuzz: binary not found: {args.binary}")
        return 2

    crashes_dir = OUT_DIR / "crashes"
    corpus_dir = OUT_DIR / "corpus" / args.parser
    crashes_dir.mkdir(parents=True, exist_ok=True)
    if args.keep_corpus:
        corpus_dir.mkdir(parents=True, exist_ok=True)

    # Seed pool: built-ins + any files in tests/fuzz/corpora/<parser>/
    pool = list(SEEDS[args.parser])
    pdir = args.seeds_dir / args.parser
    if pdir.is_dir():
        pool += [p.read_bytes() for p in sorted(pdir.iterdir()) if p.is_file()]

    rng = random.Random(args.seed)
    crashes, runs = 0, 0
    for i in range(args.iters):
        data = mutate(rng.choice(pool), rng)
        rc = run_one(args.binary, args.parser, data, args.timeout)
        runs += 1
        if args.keep_corpus:
            (corpus_dir / f"iter{i:06d}.bin").write_bytes(data)
        # Expected exits: 0 (parsed/rejected cleanly). 2 = harness misuse.
        if rc not in (0, 2):
            crashes += 1
            digest = hashlib.sha1(data).hexdigest()[:12]
            out = crashes_dir / f"{args.parser}-{digest}-rc{rc}.bin"
            out.write_bytes(data)
            print(f"fuzz: CRASH iter={i} rc={rc} -> {out}")
            if rc == -999:
                print(f"      (timeout after {args.timeout}s — hang?)")

    print(f"fuzz: {args.parser}: {runs} iters, {crashes} crash(es), seed={args.seed}")
    return 1 if crashes else 0


if __name__ == "__main__":
    sys.exit(main())
