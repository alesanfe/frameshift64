#!/usr/bin/env python3
"""Synthetic .ssr replay generator.

Emits replay files in the exact format components/decomp/src/sys/netreplay.c
reads/writes — SYNetReplayFileHeader + SYNetInputReplayMetadata +
SYNetInputFrame[player][tick] — with the input_checksum the playback
verifier recomputes (FNV-1a over player/tick/buttons/stick_x/stick_y,
tick-major then player-minor, matching syNetInputGetReplayInputChecksum).

Usage:
    python tools/developer/testing/make_replay.py --preset mario_fair -o out.ssr
    python tools/developer/testing/make_replay.py --preset mario_fair -o out.ssr --frames 900
    python tools/developer/testing/make_replay.py --self-test

Input scripts are lists of [start_tick, end_tick, player, buttons, x, y]
entries (end_tick exclusive); every tick not covered emits a valid
neutral frame, exactly like a real recording of an idle player.

Button names follow PR/os.h (A_BUTTON=0x8000 ...). Presets model the
golden-gameplay cases from the professionalization roadmap; the golden
side of the comparison is the SSB64_SYNC_TRACE captured on first run.
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# File format constants (mirrors components/decomp/src/sys/netreplay.c)

REPLAY_MAGIC = 0x53534E52          # 'SSNR'
REPLAY_VERSION = 1
REPLAY_MAX_FRAMES = 21600          # 6 minutes at 60 Hz
MAXCONTROLLERS = 4

HEADER_FMT = "<7I"                 # magic, version, metadata_size, frame_size,
                                   # frame_count, player_count, input_checksum
HEADER_SIZE = struct.calcsize(HEADER_FMT)

FRAME_FMT = "<IHbbBBB"             # tick u32, buttons u16, stick_x/y s8,
                                   # source u8, is_predicted u8, is_valid u8
FRAME_SIZE = struct.calcsize(FRAME_FMT) + 1   # padded to 12 like the C struct

FNV_BASIS = 2166136261
FNV_PRIME = 16777619

# nSYNetInputSourceSaved — recorded frames carry this tag on the wire.
SOURCE_SAVED = 3

# ---------------------------------------------------------------------------
# Game constants (mirrors decomp enums — keep in sync if headers change)

# SCKind (scdef.h): nSCKindNoController=0 ... nSCKindVSBattle=22
SCENE_VS_BATTLE = 22

# FTKind (ftdef.h)
FT_MARIO, FT_FOX, FT_DONKEY, FT_SAMUS, FT_LUIGI, FT_LINK, FT_YOSHI, \
    FT_CAPTAIN, FT_KIRBY, FT_PIKACHU, FT_PURIN, FT_NESS = range(12)
FT_NULL = 25                        # nFTKindNull

# GRKind (grdef.h)
GR_CASTLE, GR_SECTOR, GR_JUNGLE, GR_ZEBES, GR_HYRULE, GR_YOSTER, \
    GR_PUPUPU, GR_YAMABUKI, GR_INISHIE = range(9)

# FTPlayerKind (ftdef.h)
PKIND_MAN, PKIND_COM, PKIND_NOT = 0, 1, 2

# SCBattleGameType / rules / switches (scdef.h)
GAME_TYPE_DEMO = 0
GAMERULE_TIME, GAMERULE_STOCK = 1, 2
ITEM_SWITCH_NONE = 0
HANDICAP_OFF = 0

# PR/os.h button bits
BTN = {
    "A": 0x8000, "B": 0x4000, "Z": 0x2000, "START": 0x1000,
    "UP": 0x0800, "DOWN": 0x0400, "LEFT": 0x0200, "RIGHT": 0x0100,
    "L": 0x0020, "R": 0x0010,
    "C_UP": 0x0008, "C_DOWN": 0x0004, "C_LEFT": 0x0002, "C_RIGHT": 0x0001,
}
BUTTON_MASK = 0xFFFF


def btn(*names: str) -> int:
    """Combine named button bits: btn('A'), btn('C_UP'), ..."""
    out = 0
    for n in names:
        out |= BTN[n]
    return out


# ---------------------------------------------------------------------------
# Replay metadata — SYNetInputReplayMetadata, 80 bytes packed:
#   10 u32 + 9 u8 scalars + 7 u8[MAXCONTROLLERS] arrays (+3 tail pad)

def pack_metadata(spec: dict) -> bytes:
    players = spec["players"]
    pk = [PKIND_NOT] * MAXCONTROLLERS
    fk = [FT_NULL] * MAXCONTROLLERS
    lv = [0] * MAXCONTROLLERS
    for i, p in enumerate(players):
        pk[i] = p.get("pkind", PKIND_COM)
        fk[i] = p["fkind"]
        lv[i] = p.get("level", 9)

    scalars = [
        spec.get("game_type", GAME_TYPE_DEMO),           # game_type
        spec.get("game_rules", GAMERULE_STOCK),          # game_rules
        0,                                              # is_team_battle
        HANDICAP_OFF,                                   # handicap
        0,                                              # is_team_attack
        0,                                              # is_stage_select
        spec.get("damage_ratio", 100),                  # damage_ratio
        spec.get("item_rate", ITEM_SWITCH_NONE),        # item_appearance_rate
        0,                                              # is_not_teamshadows
    ]
    arrays = [
        pk, fk,
        [0] * MAXCONTROLLERS,                           # costumes
        [0] * MAXCONTROLLERS,                           # teams
        [9] * MAXCONTROLLERS,                           # handicaps (9 = neutral)
        lv,                                             # levels
        [0] * MAXCONTROLLERS,                           # shades
    ]
    blob = struct.pack(
        "<10I",
        REPLAY_MAGIC, REPLAY_VERSION,
        SCENE_VS_BATTLE,                                # scene_kind
        len(players),                                   # player_count
        spec.get("stage", GR_CASTLE),                   # stage_kind
        spec.get("stocks", 3),                          # stocks
        spec.get("time_limit", 0),                      # time_limit (0 = off)
        spec.get("item_rate", ITEM_SWITCH_NONE),        # item_switch
        spec.get("item_toggles", 0),                    # item_toggles
        spec.get("seed", 0xC0FFEE),                     # rng_seed
    )
    blob += bytes(scalars)
    blob += b"".join(bytes(a) for a in arrays)
    return blob.ljust(80, b"\x00")


def pack_frame(tick: int, buttons: int, x: int, y: int) -> bytes:
    f = struct.pack(FRAME_FMT, tick, buttons & BUTTON_MASK,
                    max(-128, min(127, x)), max(-128, min(127, y)),
                    SOURCE_SAVED, 0, 1)
    return f.ljust(FRAME_SIZE, b"\x00")


def input_checksum(frames: list) -> int:
    """FNV-1a over (player, tick, buttons, stick_x, stick_y) for every valid
    frame — same fold as syNetInputGetReplayInputChecksum."""
    checksum = FNV_BASIS
    for tick in range(len(frames[0])):
        for player in range(MAXCONTROLLERS):
            tick_f, buttons, x, y = frames[player][tick]
            for v in (player, tick_f, buttons, x & 0xFF, y & 0xFF):
                checksum = ((checksum ^ v) * FNV_PRIME) & 0xFFFFFFFF
    return checksum


def build_replay(spec: dict) -> bytes:
    frame_count = spec["frames"]
    if not (0 < frame_count <= REPLAY_MAX_FRAMES):
        raise ValueError(f"frame_count {frame_count} out of range")

    # Neutral valid frame for every (player, tick) — like a real recording.
    frames = [[[t, 0, 0, 0] for t in range(frame_count)]
              for _ in range(MAXCONTROLLERS)]
    for entry in spec.get("script", []):
        start, end, player, buttons, x, y = entry
        for t in range(start, min(end, frame_count)):
            frames[player][t] = [t, buttons, x, y]

    meta = pack_metadata(spec)
    checksum = input_checksum(frames)

    out = struct.pack(HEADER_FMT, REPLAY_MAGIC, REPLAY_VERSION,
                      len(meta), FRAME_SIZE, frame_count,
                      MAXCONTROLLERS, checksum)
    out += meta
    for tick in range(frame_count):
        for player in range(MAXCONTROLLERS):
            t, b, x, y = frames[player][tick]
            out += pack_frame(t, b, x, y)
    return out


# ---------------------------------------------------------------------------
# Preset scenarios — golden-gameplay cases from the roadmap.
# Each: 1v1 on Peach's Castle, 3 stocks, no items, fixed seed.
# Timing tuned so the attack lands mid-replay; idle tail lets the sim settle.

def _vs(fkind0: int, fkind1: int, script: list, frames: int = 600,
        seed: int = 0xC0FFEE) -> dict:
    return {
        "players": [
            {"fkind": fkind0, "pkind": PKIND_COM, "level": 9},
            {"fkind": fkind1, "pkind": PKIND_COM, "level": 9},
        ],
        "stage": GR_CASTLE, "stocks": 3, "frames": frames,
        "seed": seed, "script": script,
    }


PRESETS = {
    # Baseline: nobody touches anything — pure sim determinism.
    "idle_2p": _vs(FT_MARIO, FT_FOX, [], frames=300),

    # Mario walks right, jumps (C-up), forward air (A + stick right).
    "mario_fair": _vs(FT_MARIO, FT_FOX, [
        (60, 110, 0, 0, 70, 0),                    # walk right
        (110, 114, 0, btn("C_UP"), 0, 0),         # jump
        (135, 145, 0, btn("A"), 80, 0),           # f-air while airborne
    ]),

    # Fox runs in and up-smashes (tap up + A).
    "fox_usmash": _vs(FT_FOX, FT_MARIO, [
        (60, 100, 0, 0, -80, 0),                   # dash left toward Mario
        (110, 114, 0, btn("A"), 0, 80),           # up-smash
    ], seed=0xBEEF01),

    # Kirby walks in and inhales (B).
    "kirby_inhale": _vs(FT_KIRBY, FT_MARIO, [
        (60, 100, 0, 0, 70, 0),                    # walk right
        (110, 118, 0, btn("B"), 0, 0),            # inhale
    ], seed=0xBEEF02),

    # Captain Falcon: walk + falcon punch (B) — long startup exercises hitlag.
    "falcon_punch": _vs(FT_CAPTAIN, FT_MARIO, [
        (60, 95, 0, 0, 70, 0),                     # walk right
        (100, 104, 0, btn("B"), 0, 0),            # falcon punch
    ], seed=0xBEEF03),
}


def load_spec(arg: str, frames_override: int | None) -> dict:
    if arg.endswith(".json"):
        spec = json.loads(Path(arg).read_text(encoding="utf-8"))
    elif arg in PRESETS:
        spec = dict(PRESETS[arg])
    else:
        raise SystemExit(f"unknown preset {arg!r} (have: {sorted(PRESETS)})")
    if frames_override:
        spec["frames"] = frames_override
    return spec


def self_test() -> int:
    """Round-trip: build every preset, re-parse header, recompute checksum."""
    ok = True
    for name, preset in PRESETS.items():
        blob = build_replay(preset)
        magic, ver, msize, fsize, fcount, pcount, cksum = struct.unpack(
            HEADER_FMT, blob[:HEADER_SIZE])
        exp = HEADER_SIZE + msize + fcount * MAXCONTROLLERS * fsize
        good = (magic == REPLAY_MAGIC and ver == REPLAY_VERSION and
                msize == 80 and fsize == FRAME_SIZE and
                pcount == MAXCONTROLLERS and len(blob) == exp)
        print(f"{name:14} frames={fcount:5} checksum=0x{cksum:08X} "
              f"size={len(blob):7} {'OK' if good else 'BAD'}")
        ok &= good
    print("self-test:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("spec", nargs="?", help="preset name or spec .json")
    ap.add_argument("-o", "--output", type=Path)
    ap.add_argument("--frames", type=int, help="override frame_count")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()

    if a.self_test:
        return self_test()
    if not a.spec or not a.output:
        ap.error("need <spec> and -o <out.ssr> (or --self-test)")

    spec = load_spec(a.spec, a.frames)
    blob = build_replay(spec)
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_bytes(blob)
    cksum = struct.unpack("<I", blob[24:28])[0]
    print(f"wrote {a.output} ({len(blob)} bytes, {spec['frames']} frames, "
          f"checksum=0x{cksum:08X})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
