# Fuzzing

## In place

- `-PacketFuzz` in `netplay_localtest.ps1` — seeded hostile datagrams
  through the real UDP parser; asserted non-crash + drop accounting.
- `services/netplay-lobby/tests/fuzz/test-lobby-fuzz.js` — lobby message fuzzing.
- Mutation suite — proves each individual validation rejects its attack.

## Binary-level parser fuzzing (TD-010)

`tools/analysis/fuzz/run_parser_fuzz.py` mutates seed inputs and feeds each one
through the real parser in a test-hooks build:

    python tools/analysis/fuzz/run_parser_fuzz.py --parser manifest \
        --binary out/build/<preset>/BattleShip --iters 2000 --seed 1

| `--parser` | input → parser under test |
|---|---|
| `manifest` | `Ship::Archive::ParseManifestBuffer` (pure manifest.json parse — no archive mount) |
| `replay`   | `syNetReplayLoadDebugFile` (header + metadata + frame stream) |
| `json`     | `nlohmann::json::parse` — the parser config and manifests share |
| `save`     | `port_save_read` + `checkSaveFormat` + `.meta` sidecar sscanf — the fuzz bytes feed both the SRAM image (`SSB64_SAVE_PATH`) and the sidecar |

How it works: a test-hooks build honors `SSB64_FUZZ_PARSER` +
`SSB64_FUZZ_INPUT` as the first thing in `PortInitImpl` — parse, log,
`port_exit_process`. No SDL, no archives, no game boot: each iteration is
a few ms. Crashes/hangs are saved to `out/fuzz/crashes/`. Seed corpus
lives in `tests/fuzz/corpora/<parser>/` (drop new valid inputs there to
grow coverage) plus synthesized seeds in the driver (a structurally
valid `.ssbreplay` is generated from the wire format).

Expected exits: `0` always — parse success AND parse rejection both
return 0 (a rejected input is a pass, not a finding). Any other code is
a crash; a timeout is flagged as a hang.

## libFuzzer targets

Coverage-guided targets live in `tests/fuzz/` behind
`-DSSB64_BUILD_FUZZERS=ON` (clang only — `-fsanitize=fuzzer,address,undefined`,
registered in CTest with the `fuzz` label):

    tests/fuzz/
    ├── net_packet_fuzzer.c        # SSNP datagrams → netpacket.c parse entry points
    ├── mod_manifest_fuzzer.cpp    # manifest.json → Archive::ParseManifestBuffer
    ├── config_json_fuzzer.cpp     # raw JSON → nlohmann::json::parse + round-trip
    ├── save_file_fuzzer.cpp       # port_save load path  (still open — file-based)
    ├── replay_fuzzer.cpp          # replay headers + streams  (covered by driver)
    └── o2r_metadata_fuzzer.cpp    # archive directory/index parse  (still open)

Seed corpora live in `tests/fuzz/corpora/<parser>/` — `packet_parser/`
holds synthesized valid SSNP datagrams (INPUT / CONTROL / FORWARD plus
truncations). The run_parser_fuzz driver covers manifest/replay/raw-json
through the real binary; the libFuzzer targets cover the pure codecs with
coverage feedback. The o2r index walk and save/config file load paths
(file-based, need game state shims) remain open.

## Parser requirements for any fuzzable input

Bytes must never cause: out-of-bounds access, unbounded allocation,
infinite loops, partially-applied state, or blocking reads. Every field
is range-checked before use; every failure path leaves state untouched.

The SSNP parser is close to this by construction (fixed-size families,
no embedded lengths); save/config/replay parsers are not yet hardened to
this bar.
