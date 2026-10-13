# Test plan — rollback & netplay (mapped to what exists)

The full spec (R1–R7, harness layers, test IDs) lives in the methodology
doc. This file maps each requirement to existing infrastructure and lists
what's missing, in priority order. All env hooks verified in code
2026-10-13.

## What already exists

| Spec component | Existing implementation |
|---|---|
| DeterministicGameRunner | `SSB64_ROLLBACK_AUTOVS` + `SSB64_RIG_EXIT` + `SSB64_MAX_FRAMES` + `SSB64_MUTE` — unattended headless VS match, exits at a fixed tick (`netrollback.c`, `netplay_auto` path in `port.cpp`) |
| InputGenerator | `SSB64_NETPLAY_FUZZ` — deterministic per-tick hash-driven synthetic inputs (`netinput.c:298-344`), same on all peers |
| VirtualNetworkSimulator | `SSB64_NETPLAY_SIM_{LATENCY_MS,JITTER_MS,LOSS_PCT,SEED}` — delay queue in `port_udp.cpp` (`sSimQueue`, seeded `sSimRng`). Missing: reorder, duplication, corruption, bandwidth, cut/route-change |
| StateRecorder | `SSB64_NETPLAY_CONFIRMED_TRACE` — per-peer confirmed-state trace file; `SSB64_SYNC_TRACE`/`SYNC_VERIFY` — per-tick field-grouped hashes (fighters/joints/items/stage/camera/vars/...) |
| Oracle | pairwise trace diff in `scripts/*lobbytest*.ps1`, `netplay_localtest.ps1`; `syNetRollbackCheckRemoteConfirmedHash` compares remote hash each tick |
| Rollback correctness self-test | `SSB64_ROLLBACK_SYNCTEST=N` — every tick is snapshotted, N frames resimulated, hashes compared in-process (`netrollback.c`); `scripts/rollback_synctest.ps1` |
| Multiplayer e2e | `scripts/netplay_lobbytest_multi.ps1` — real Cloudflare worker (wrangler dev or prod), N players, `-ForceRelay`, fuzzed inputs, exit 0=sync/1=desync/2=no match |
| Side-effect suppression | `syNetRollbackIsResimulating()` consulted by audio (`n_env.c:5340`, `audio.c:1671`) — R6 partially enforced |

## Gap analysis (spec → repo)

### R2 metamorphic equivalence — mostly covered
`netplay_localtest.ps1` runs two processes over localhost UDP with sim
latency/jitter/loss and diffs confirmed traces. Missing variants:
- **duplication / reorder** — the sim queue can't produce them today
  (`port_udp.cpp` enqueue only delays). Reorder comes free once jitter >
  latency, duplication doesn't exist at all.
- **relay path** — covered only by `lobbytest_multi -ForceRelay` (real
  worker). No direct↔relay mid-match migration test (NAT-004).

### R4/R5 input integrity — partially covered
Duplicate packets repeat the last 16 frames (`netpeer.c:1180`) and
`syNetInputSetRemoteInput` dedupes via remote-history lookup — loss is
covered, dupe/order stats exist (`seq_gaps/dup/ooo`). Missing: explicit
assertion tests, wrong-session rejection test (PROTO-005), future-frame
flood test (UDP-006).

### R6 side effects — weakest link
Only audio checks `IsResimulating`. Unverified: rumble, Rich Presence,
replay writer (`SSB64_REPLAY_RECORD`), statistics, UI messages, any other
`port_log` of match events. Needs an audit pass + `EffectJournal`-style
counter in a test.

### Determinism cross-platform — not covered
SyncTest is same-process/same-build. No harness compares hash across
builds/architectures (DET-005..007). `SYNC_VERIFY` produces portable
per-tick field hashes — the pieces exist; what's missing is a CI job
running identical seeded matches on two builds and diffing traces.

### CI — empty
`.github/workflows/build.yml` compiles on 3 OSes and stops. Nothing runs
synctest/localtest. Minimum first step: Windows job running
`rollback_synctest.ps1` + `netplay_localtest.ps1` headless.

### Fuzzing — input fuzz exists, packet fuzz does not
`SSB64_NETPLAY_FUZZ` fuzzes *inputs*, not *packets*. No libFuzzer harness
for the datagram parser, SSBL text protocol, or worker messages (§11).

### Snapshot tests — N/A by design, with caveats
Snapshots are opaque `memcpy` of BSS/DATA/arena regions (not serialized),
so SNP-004/005 don't apply — but that also means **snapshot coverage is
implicit**: anything not in those regions or below the arena watermark
silently isn't state. The file-range exclusion table and aobj fixup
bitmaps are the known edge; `SSB64_SYNC_VERIFY` divergence is the tripwire.

## Implementation order (spec §27, grounded)

**Layer 1 — finish what exists:**
1. Extend `port_udp.cpp` sim: `SIM_REORDER_PCT`, `SIM_DUP_PCT`,
   `SIM_CORRUPT_PCT`, seeded like existing params (~50 lines).
2. `netplay_localtest.ps1` flags for the new sim params.
3. Effect-suppression audit: grep every subsystem that mutates observable
   state during sim and gate it on `syNetRollbackIsResimulating()`.

**Layer 2 — CI:**
4. CI job: synctest at distances 1/3/6 + localtest at P0/P2/P4 profiles.
5. Cross-build trace compare job (Release vs Debug same seed).

**Layer 3 — adversarial:**
6. libFuzzer harness on `syNetPeerHandlePacket` + `HandleForwardPacket` +
   SSBL parser (`handle_game_message` splits/atoi without validation is
   the easy first crash).
7. Mutation suite per spec §21 — cheapest 3 mutants: skip RNG restore,
   apply packet twice, accept wrong session_id. (NB: SEC-1/SEC-2 from the
   audit make "wrong session" mutants *currently passable* — fix those
   before mutation testing means anything.)

**Layer 4 — soak:**
8. `rollback_synctest.ps1` with `-Minutes` long runs; rematch loop
   (`port_netlobby_after_match` path); leak metrics.

## Non-goals for now

- Property-based testing framework — the seeded sim + fuzz inputs cover
  the same ground with less machinery.
- Pointer-independence snapshots (SNP-004) — irrelevant to memcpy-based
  save states.
