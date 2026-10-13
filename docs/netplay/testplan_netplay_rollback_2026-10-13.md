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
| Oracle | pairwise trace diff in `scripts/testing/netplay_lobbytest*.ps1`, `netplay_localtest.ps1`; `syNetRollbackCheckRemoteConfirmedHash` compares remote hash each tick |
| Rollback correctness self-test | `SSB64_ROLLBACK_SYNCTEST=N` — every tick is snapshotted, N frames resimulated, hashes compared in-process (`netrollback.c`); `scripts/development/rollback_synctest.ps1` |
| Multiplayer e2e | `scripts/testing/netplay_lobbytest_multi.ps1` — real Cloudflare worker (wrangler dev or prod), N players, `-ForceRelay`, fuzzed inputs, exit 0=sync/1=desync/2=no match |
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

## Implementation status (2026-10-14)

All four layers are implemented. Env hooks are deterministic through
`SSB64_NETPLAY_SIM_SEED` / `SSB64_NETPLAY_FUZZ` / `SSB64_NETPLAY_SEED`.

### Layer 1 — done

- `src/netplay/port_udp.cpp` sim: `SSB64_NETPLAY_SIM_{DUP,REORDER,CORRUPT}_PCT`
  plus `SIM_REORDER_WINDOW_MS`. Order of application: loss drop → corrupt a
  copy (never the caller's buffer) → jitter/reorder delay → queue insert →
  optional duplicate twin. Only `send_routed` (game traffic) is affected;
  STUN/lobby `raw_send_to` paths bypass the sim.
- `scripts/testing/netplay_localtest.ps1`: `-DupPct`, `-ReorderPct`,
  `-ReorderWindowMs`, `-CorruptPct`, `-Mutant`, `-PacketFuzz`.
- R6 side-effect audit:
  - **audio** — already gated (`n_env.c`, `audio.c` check `IsResimulating`).
  - **rumble** — `controller.c` suppresses only the motor `osSendMesg`
    dispatch during resim; the `D_80045268` slot bookkeeping is snapshotted
    BSS and must still execute identically on both passes or SyncTest hashes
    diverge. (`syNetRollbackIsResimulating()` guard on the send.)
  - **replay writer** — records published frames only; resim republish is
    deduped by remote-history lookups in `netinput.c`. Intentional.
  - **match statistics** — live in BSS inside the snapshot; resim restores
    them, no double counting.
  - **Rich Presence** — `DiscordRichPresence.cpp` polls on scene change from
    the UI thread, outside the resim path. No guard needed.
  - **port_log** — logs may repeat for resim ticks; cosmetic only.

### Layer 2 — done

- `.github/workflows/determinism.yml` (ROM-gated, nightly + dispatch):
  SyncTest distances 1/3/6; localtest clean + degraded matrix including
  dup/reorder/corrupt; packet-fuzz step; mutation suite; artifact upload.
  Second job `cross-build` runs identical seeded localtest under Release and
  Debug and diffs host traces line-by-line.

### Layer 3 — done

- Packet fuzz in `netpeer.c`: `SSB64_NETPLAY_PACKETFUZZ=<count>` injects
  seeded pseudo-random datagrams through the real handlers each drain.
  Tiers: 0 random bytes / 1 valid magic+version / 2 +session /
  3 fully valid INPUT (lands in the staging window) / 4 wrong session with
  valid checksum / 5 bad checksum. Odd iterations go through the FORWARD
  wrapper path. Oracle = no crash; accepted garbage trips the desync
  detector. 300 probe packets of the mutant-shaped class are injected
  automatically when a `SSB64_NETPLAY_MUTANT` is set without a fuzz count.
- Mutants (`SSB64_NETPLAY_MUTANT`, `SSB64_ROLLBACK_MUTANT`):
  `accept_bad_session`, `accept_bad_checksum`, `no_forward_guard`,
  `skip_rng_restore`. Driven by `scripts/testing/netplay_mutation_suite.ps1`:
  baselines must pass, every mutant must produce exit 1.
- Lobby fuzz: `services/netplay-lobby/test-lobby-fuzz.js` hammers
  `node-lobby.js` with malformed text/binary frames and hostile connection
  params; asserts the process survives and an untouched control room stays
  functional.

### Layer 4 — done

- `scripts/testing/netplay_soak.ps1`: `-RematchRounds N` back-to-back
  sessions (fresh processes = full connect/teardown/rematch cycle) with
  adverse sim conditions, crash-signature scan, and log-growth leak check;
  optional `-SynctestMinutes M` long SyncTest run.
- `rollback_synctest.ps1` already supported `-Minutes`; added `-Mutant`.

### Commands

```powershell
# smoke (fast)
.\scripts\testing\netplay_localtest.ps1 -BuildDir build\x64\Release -Seconds 30
.\scripts\testing\netplay_mutation_suite.ps1 -BuildDir build\x64\Release -Minutes 1 -Seconds 30
node services\netplay-lobby\test-lobby-fuzz.js 400

# soak (slow)
.\scripts\testing\netplay_soak.ps1 -RematchRounds 5 -Seconds 60 -SynctestMinutes 5
```

Expected: localtest `IN SYNC`; mutation suite `PASS — all mutants detected`
(each mutated run exits 1 — a passing mutant is a real finding); fuzz
iterations never crash or kill an unrelated room.

### Verified on-device (2026-10-14, `build\x64\Release`)

- SyncTest d=1: `PASS tested=3826 failed=0 skipped=852`; mutant
  `skip_rng_restore` → `FAIL failed=3481` (RNG coverage proven).
- Localtest clean: `IN SYNC for 1501 ticks`; adversarial
  (60ms/20ms jitter/3% loss/5% dup/10% reorder/1% corrupt):
  `IN SYNC for 661 ticks` after the v4 checksum fix.
- Mutants `accept_bad_session` and `no_forward_guard` → DESYNC at line 2
  (probe stream auto-injects the mutant-shaped vector).
- Packet fuzz 50/drain × 30s: no crash; injected valid-tier inputs desync
  as expected.
- Lobby: `test-lobby-security.js` 15/15; `test-lobby-fuzz.js` survives
  malformed floods with the control room intact.

### Known limits

- A `-PacketFuzz` localtest run legitimately desyncs — the tier-3 valid
  packets apply random inputs. The script therefore skips the trace diff in
  fuzz mode: exit 1 = crash signature found, exit 2 = no trace, exit 0 =
  survived.
- The corruption sim already caught a real bug on first contact: the
  sync-check trailer (`check_tick`/`check_hash`/`remote_advantage`) rode
  outside the packet checksum, so a corrupted `check_hash` produced a false
  DESYNC with all columns divergent while the traces agreed. Fixed by making
  the checksum byte-wise over the whole datagram (wire v4) — see
  `docs/bugs/netplay_synccheck_trailer_corruption_2026-10-14.md`.
- `no_forward_guard` probing is meaningful on a 3+ player session; on a
  2-player localtest it still exercises the path because the probe FORWARD
  claims the remote's own slot with a valid inner packet.
- In-game rematch (`port_netlobby_after_match`) needs UI input that the
  headless rig can't drive; the soak approximates it at process granularity.
- A real libFuzzer target (coverage-guided, corpus) is still open — the
  in-engine seeded fuzz covers the same code paths but doesn't minimize
  crashing inputs.
