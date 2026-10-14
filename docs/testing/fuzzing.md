# Fuzzing

## In place

- `-PacketFuzz` in `netplay_localtest.ps1` — seeded hostile datagrams
  through the real UDP parser; asserted non-crash + drop accounting.
- `services/netplay-lobby/tests/test-lobby-fuzz.js` — lobby message fuzzing.
- Mutation suite — proves each individual validation rejects its attack.

## libFuzzer targets (planned — TD-010)

    tests/fuzz/
    ├── net_packet_fuzzer.cpp      # SSNP datagrams → syNetPeerHandlePacket
    ├── save_file_fuzzer.cpp       # port_save load path
    ├── config_fuzzer.cpp          # config parse/validate
    ├── replay_fuzzer.cpp          # replay headers + streams
    ├── o2r_metadata_fuzzer.cpp    # archive directory/manifest parse
    └── mod_manifest_fuzzer.cpp    # manifest.json + registry

## Parser requirements for any fuzzable input

Bytes must never cause: out-of-bounds access, unbounded allocation,
infinite loops, partially-applied state, or blocking reads. Every field
is range-checked before use; every failure path leaves state untouched.

The SSNP parser is close to this by construction (fixed-size families,
no embedded lengths); save/config/replay parsers are not yet hardened to
this bar.
