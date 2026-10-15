# Testing strategy & test catalog — smash-classic

**Status:** living document · **Companion:** `strategy.md` (quality gates),
`determinism.md`, `fuzzing.md` · **Audit link:** `docs/investigations/professionalization_audit_2026-10-14.md`

This document converts "add tests" into a risk-traced, executable system.
Every case carries a stable ID, the risk it protects, the real hook or seam
it uses, and the pipeline tier where it runs.

ID scheme: `<AREA>-<GROUP>-<NNN>` — e.g. `NET-PROTO-LEN-001`.
Areas: `BOOT`, `CFG`, `SAVE`, `ASSET`, `INPUT`, `AUDIO`, `RENDER`,
`GAME`, `GUI`, `NET` (`NET-PROTO`, `NET-TRANS`, `NET-RB`, `NET-DET`,
`NET-DESYNC`), `LOBBY`, `MOD`, `UPD`, `PLAT`, `SEC`, `REL`.

---

## A. Master strategy

### Principles (binding)

1. Every test protects a behavior, contract, risk, or historical bug — never
   mirrors internals. Each `docs/bugs/` entry is a candidate regression case.
2. Deterministic oracles preferred: hash, golden trace, structured verdict.
   `SSB64_NETPLAY_SIM_SEED`/`SSB64_NETPLAY_SEED`/`SSB64_NETPLAY_FUZZ` already
   give seeded network/inputs — tests must set them, not rely on ambient RNG.
3. Isolation: no ordering, no shared mutable state between cases, no leftover
   files. Per-test working dirs under `out/test-runs/<id>/`.
4. Hermetic by default: no production lobby, no external network. Lobby tests
   run `node-lobby.mjs` or `wrangler dev` locally; updater tests run a local
   HTTP stub serving a synthetic release manifest.
5. ROM-dependent tests stay local/`requires-rom` labeled — fixtures derived
   from non-redistributable data are never committed.
6. A retry is not a fix. Flaky → quarantine label + issue, never silent retry.
7. Failure taxonomy is explicit in reports: **product bug** / **environment
   fault** / **test bug** — scripts already encode this (exit 0/1/2).
8. Coverage is a diagnostic, not a goal. Gates measure *risk coverage*:
   which invariants, errors, and state transitions have a test.

### Test pyramid mapped to this repo

| Layer | Form | What exists today | Gap |
|-------|------|-------------------|-----|
| Static | clang-tidy, cppcheck, semgrep, check_docs, yamllint | Workflows exist; `check_docs` not in CI (F-004) | wire CI |
| Unit | gtest (new) | `tests/unit/` (platform/, resources/) + `tests/integration/`; level-based layout — see tests/README.md, no gtest harness | everything |
| Component | in-proc harnesses + fakes | netpeer parser reachable via PACKETFUZZ path; pure codec at `src/networking/netplay/protocol/` with `netpacket_test` | no gtest harness (minimal EXPECT harness for now) |
| Integration | multi-process Python harness (`tests/support/harness/netplay/` + `tests/e2e/netplay/`; `scripts/testing/*.ps1` are thin wrappers) | `localtest/lobbytest/lobbytest_multi/spectate/lan/compat/soak/mutation/takeover` ✓ | config/save/updater none |
| System/E2E | headless runs | `AUTOVS` + `MAX_FRAMES` + `RIG_EXIT` | not packaged as one target |
| Soak/nightly | `determinism.yml` | ROM-gated nightly ✓ | forks get no signal |

### Environments

- **Hermetic CI**: ROM-free jobs only (codec, lobby, lint, unit).
- **ROM-gated CI**: nightly determinism (existing `BASEROM_US_Z64` gate).
- **Local dev**: full suite incl. real loopback UDP, wrangler dev.
- **Never**: production worker, public services, load tests vs shared infra.

---

## B. Testability inventory (real seams)

### Seams that already exist

| Seam | Mechanism | Enables |
|------|-----------|---------|
| Packet parser | `syNetPeerHandlePacket`/`HandleForwardPacket` are call paths over byte buffers; `SSB64_NETPLAY_PACKETFUZZ` proves injected datagrams reach them | codec unit/fuzz tests — codec extracted to `src/networking/netplay/protocol/netpacket.*`, covered by `netpacket_test` |
| Network sim | `SIM_{LATENCY,JITTER,LOSS,DUP,REORDER,CORRUPT}_PCT` + `SIM_SEED` in `udp_transport.cpp` | impairment matrix, replayable failures |
| Headless run | `SSB64_ROLLBACK_AUTOVS` + `SSB64_MAX_FRAMES` + `SSB64_MUTE` + `SSB64_RIG_EXIT` | `smash-classic-sim` target is mostly packaging |
| Deterministic inputs | `SSB64_NETPLAY_FUZZ` per-tick hash inputs | replayable matches |
| Traces | `CONFIRMED_TRACE` (per-tick hash file), `SYNC_VERIFY` (field-grouped subsystem hashes) | first-divergent-frame + subsystem attribution |
| Mutant suite | `SSB64_NETPLAY_MUTANT`, `SSB64_ROLLBACK_MUTANT` + 300 auto probes | validation-coverage mutation testing |
| Desync injector | `SSB64_NETPLAY_TEST_DESYNC=<tick>` (new) | desync detection/diagnostic tests |
| Replay | `REPLAY_RECORD/PLAY`, `REPLAY_TAKEOVER=<p>:<tick>` (new) | record/replay corpus, takeover determinism |
| Rollback self-check | `ROLLBACK_SYNCTEST=N` snapshot↔resim per tick | snapshot coverage |
| Lobby | `node-lobby.mjs` local + `test-lobby-security.js` (15 cases) + `test-lobby-fuzz.js` | contract + security + fuzz |

### Missing seams (needed before certain tests)

| Component | Problem | Minimal seam (no over-engineering) |
|-----------|---------|----------------------------------|
| `udp_transport` transport | real sockets in unit tests | extract `send_routed` behind a vtable `NetTransport` {send,recv,poll}; loopback fake = 60 lines |
| Updater | shells out to curl + powershell; hits github API | `UpdateTransport` interface (fetch json/fetch file); `FakeUpdaterServer` = local static dir over `http://127.0.0.1` |
| Save system | `port_save.cpp` writes via direct file IO, unversioned | `SaveStore` {read,write,list} + in-memory fake; also the fix for F-011 |
| Config (LUS CVars) | global registry, no unknown-key handling | in-memory CVar snapshot/restore helper for tests; don't rebuild LUS |
| Clock | `port_udp_now_ms()` ambient | inject a fake clock only at the transport/rollback boundary (single `gNowFn`), not globally |
| Logging | `port_log` text-only | `LogSink` interface → JSONL sink + in-memory spy; doubles as observability work |
| Lobby client | text protocol over ws | `SsblConnection` {send_line,recv_line}; fake = in-process script |
| Filesystem | paths + CWD assumptions | `PathResolver` for save/config/mods dirs; tests point it at temp dirs |
| Mods | TinyCC compiles real DLLs | test path: `ModLoader` accepts a precompiled fixture mod + a manifest-only mode for validation tests |

**Explicit non-goals**: no DI container, no interfaces around trivial
functions, no mocking decomp game logic. Characterization/Golden Master
(traces) is the correct tool for legacy code, not unit tests around it.

---

## C. Case template (binding for new entries)

```
ID: AREA-GROUP-NNN
Name / Area / Level / Type
Risk: what breaks if untested
Hooks: env vars, fakes, fixtures used
Given/When/Then or Steps
Expected: observable result + side effects + absence-of-effect
Oracle: exact | invariant | golden | model | property | diff
Labels: e.g. unit;netplay;protocol;negative
Tier: PR / nightly / release · Requires: rom|assets|network
Trace: bug doc / issue / ADR / threat-model row
```

---

## D. Test catalog

### D.1 Protocol codec (`NET-PROTO`) — *codec extracted (F-003 done); first native suite landed*

Implemented as `tests/unit/netplay/protocol/netpacket_test.c` (CTest name
`netpacket_test`, labels `unit;netplay;protocol;deterministic`) over the pure
codec `src/networking/netplay/protocol/netpacket.*`. `components/decomp/src/sys/netpeer.c` now calls
the codec — `SYNETPEER_*`/`syNetPeerRead|Write|Checksum*` names remain as
aliases; session state, counters, seq/dup/OOO tracking and link authorization
stay in netpeer. Covered today: LEN-001, LEN-002, MAG-001, VER-001, SES-001,
CKSM-001, RT-001 (INPUT/CONTROL/FORWARD — BOOTSTRAP vectors pending),
mutant-bypass flags, BOOTSTRAP prefix round trip (RT-001), TYPE-001 type
enumeration, CUR-001 valid/invalid corpus, and FUZZ-001 as a seeded in-test
loop (10k arbitrary + 10k mutated datagrams + every truncation). Pending:
a real libFuzzer entry point (needs the clang coverage lane — the seeded
loop is the interim), and the comp-level rows (DUP/OOO/SLOT — stateful,
live in `tests/e2e/netplay/`).

| ID | Case | Level | Oracle | Tier |
|----|------|-------|--------|------|
| NET-PROTO-LEN-001 | Declared INPUT size vs short buffer → reject, no state change, no alloc by untrusted length | unit | invariant | PR |
| NET-PROTO-LEN-002 | `frame_count` ∈ {0,16,17,255} → 0/16 accepted-bounded, >16 rejected | unit | boundary | PR |
| NET-PROTO-MAG-001 | wrong magic → reject, counter++ only | unit | code | PR |
| NET-PROTO-VER-001 | version = cur±1, cur, future, 0 → only exact wire version accepted | unit | code | PR |
| NET-PROTO-SES-001 | session_id mismatch → reject (mutant `accept_bad_session` regression) | unit | code | PR |
| NET-PROTO-CKSM-001 | flip each byte of checksum + each covered field → always reject | unit/fuzz-reg | property | PR |
| NET-PROTO-RT-001 | serialize→parse round trip: INPUT/CONTROL/BOOTSTRAP/FORWARD vectors | unit | golden bytes | PR |
| NET-PROTO-DUP-001 | same packet twice → `dup` stat, no double-apply of inputs | comp | counter | PR |
| NET-PROTO-OOO-001 | seq gaps/reorder → `seq_gaps`/`ooo` stats, reorder tolerated per window | comp | model | PR |
| NET-PROTO-SLOT-001 | FORWARD `src_player` forgery → rejected (post-2026-10-13 fix regression) | comp | code | PR+sec |
| NET-PROTO-TYPE-001 | every packet-type byte 0..255 → unknown types rejected cleanly | unit | enumeration | PR |
| NET-PROTO-FUZZ-001 | 10k seeded arbitrary datagrams incl. empty/oversized → no crash/OOB; desync allowed, crash never | fuzz | no-crash | nightly (exists in-engine; needs codec target) |
| NET-PROTO-CUR-001 | corpus: every valid message from spec §messages + every invalid sibling | unit | corpus | PR |

Real anchors: header `4+2+2+4+4+4+1+1+1+1`, INPUT=172B, CONTROL=20B,
FORWARD=189B, `SYNETPEER_MAX_PACKET_FRAMES=16`, FNV basis `2166136261`,
types 0..6 — see `docs/netplay/protocol/specification.md`. The spec's own
byte-level example becomes `NET-PROTO-RT-001` vectors.

### D.2 Transport impairment (`NET-TRANS`) — *exists; matrix it*

| ID | Case | Status | Tier |
|----|------|--------|------|
| NET-TRANS-CLEAN-001 | loopback, no impairment → IN SYNC | exists (localtest P0) | nightly |
| NET-TRANS-MAT-001 | matrix: latency {50,100,150} × jitter {10,25,50} × loss {0,3,5} | exists | nightly |
| NET-TRANS-DUP-001 | DupPct 5 → dedup counters nonzero, still IN SYNC | exists | nightly |
| NET-TRANS-ORD-001 | ReorderPct 15 + window → IN SYNC or bounded rollback | exists | nightly |
| NET-TRANS-COR-001 | CorruptPct 2 → checksum rejects, no crash, IN SYNC or controlled desync | exists | nightly |
| NET-TRANS-CUT-001 | full silence >kPeerTimeoutMs → `peer_left`/timeout, resources freed, no deadlock | **new** | nightly |
| NET-TRANS-MIG-001 | direct↔relay path change mid-match (NAT-004) | **new** | nightly |
| NET-TRANS-KILL-001 | host kill -9 mid-match → clients detect+exit, no hang | partial (soak) | nightly |

### D.3 Rollback (`NET-RB`)

| ID | Case | Hooks | Oracle | Tier |
|----|------|-------|--------|------|
| NET-RB-SYNC-001 | SynTest distances {1,3,6} every tick | `ROLLBACK_SYNCTEST` | in-proc hash | nightly (exists) |
| NET-RB-EQUIV-001 | resim(published inputs) ≡ un-predicted run | confirmed trace diff | golden | nightly (exists) |
| NET-RB-SIDE-001 | resim fires zero audio/rumble/stats side-effects | `IsResimulating` gates | counter | **new**, PR |
| NET-RB-SAVE-001 | resim never writes save/config | fake SaveStore spy | absence | **new**, PR |
| NET-RB-RNG-001 | RNG restored on rollback (mutant `skip_rng_restore` must FAIL) | mutant suite | mutation | nightly (exists) |
| NET-RB-RING-001 | runs > ring size (720) don't corrupt verification | `netreplay` bug regression | exit code | **new** regression |
| NET-RB-TAKE-001 | `REPLAY_TAKEOVER=p:t` — recorded≤t identical, slot live>t, verify-disabled verdict not a false FAIL | takeover hook | property | **new** |
| NET-RB-MAX-001 | rollback at max distance boundary (window edge) | sim latency>window | boundary | **new** |

### D.4 Determinism (`NET-DET`)

| ID | Case | Tier |
|----|------|------|
| NET-DET-XBUILD-001 | Release≡Debug identical traces, same seed | nightly (exists) |
| NET-DET-SEED-001 | same seed twice → byte-identical trace (self-compare) | **new**, cheap PR |
| NET-DET-VAR-001 | fighter×stage sweep: all 12×9 AUTOVS combos, per-combo hash stable | nightly |
| NET-DET-CONSEC-001 | back-to-back matches in one process → identical (BSS drift check) | **new** |
| NET-DET-XPAT-001 | cross-platform trace compare (Win↔Linux) | nightly, **new** |
| NET-DET-HASHDEF-001 | hash covers only documented regions — spec what mem is hashed; padding/pointers excluded assertion | **new**, doc+test |

### D.5 Desync diagnostics (`NET-DESYNC`)

| ID | Case | Oracle |
|----|------|--------|
| NET-DESYNC-INJ-001 | `SSB64_NETPLAY_TEST_DESYNC=<tick>` → detector fires at first divergent confirmed tick, not earlier/later | tick equality |
| NET-DESYNC-DIAG-001 | emitted diagnostics contain {first divergent tick, peer, protocol ver, ruleset, per-subsystem hashes} and **no** secrets/ROM paths | schema check on log |
| NET-DESYNC-LIE-001 | peer reporting a false hash → flagged as desync (no state corruption on honest peer) | negative |
| NET-DESYNC-COMPAT-001 | mismatched `compat_token` → handshake reject before any sim (netlobby gate) | code |

### D.6 Lobby (`LOBBY`) — Cloudflare + node-lobby share the conformance suite

| ID | Case | Exists? | Tier |
|----|------|---------|------|
| LOBBY-SEC-* | 15 security cases (upkey, star routing, slot bounds, spec isolation) | exists | PR |
| LOBBY-FUZZ-* | malformed text/binary + hostile params vs node-lobby, control room intact | exists | nightly |
| LOBBY-CONF-001 | **conformance**: same scenario transcript vs cloudflare AND node impls — MUST be identical | **new** (F-006) | PR |
| LOBBY-SPEC-001 | spec→host-only routing (new code path) + spec silent-keepalive not timing out | **new**, PR | PR |
| LOBBY-V12-001 | v1 client vs v2 room → `version-mismatch`, close 4000 | **new** | PR |
| LOBBY-RL-* | rate limiter: at-limit / +1 / burst / reconnect-evasion / room cap | partial | PR |
| LOBBY-WS-* | oversized binary, fragmented frames, Blob-vs-ArrayBuffer (prod regression — blob_drop bug), invalid UTF-8, after-close msgs | partial | PR+nightly |
| LOBBY-CONC-001 | simultaneous joins, join+leave race, two-host claim, DO restart mid-room | partial | nightly |
| LOBBY-LOAD-001 | rooms/connections limits + stop criteria (staging only, cost-capped) | **new** | release |

### D.7 Save / config (`SAVE`, `CFG`)

| ID | Case | Tier |
|----|------|------|
| SAVE-VER-001 | written save carries {magic, version, size, crc} — blocked on F-011 | PR |
| SAVE-COR-* | truncated / bad crc / future version / other-region → graceful reject, original preserved | PR |
| SAVE-ATOM-001 | kill mid-write → either old or new valid file, never partial | nightly |
| SAVE-MIG-001 | v_prev → v_cur migration incl. backup + rollback on failure | PR (post-F-011) |
| CFG-DEF-001 | fresh boot writes defaults; unknown keys warned-not-dropped; out-of-range clamped | PR |
| CFG-ATOM-001 | interrupted write → backup recovery | nightly |
| CFG-NET-001 | sim-affecting keys are in the manifest/netrules set; local-only keys provably don't change sim hash (metamorphic) | nightly |

### D.8 Assets (`ASSET`)

| ID | Case | Tier |
|----|------|------|
| ASSET-REC-001 | recipe-hash mismatch → re-extract (regression for `stale_o2r_recipe_mismatch`) | PR w/o ROM? no — requires-rom |
| ASSET-GOLD-001 | per-region golden {hash,size,count} of generated O2R (TD-006) | nightly/rom |
| ASSET-NOROM-001 | absent ROM → actionable first-run error, no crash | PR (ROM-free!) |
| ASSET-CORR-001 | corrupt/truncated O2R → clean error path | PR |
| ASSET-PROT-001 | CI check: no ROM-derived blob can be committed (F-026) | PR |

### D.9 Input / audio / render / gameplay

| ID | Case | Oracle | Tier |
|----|------|--------|------|
| INPUT-MAPP-001 | two same-GUID pads route to distinct ports (regression `controller_port_routing`) | routing table | PR if harnessable, else manual charter |
| INPUT-DISC-001 | disconnect mid-match → pause/notify, reconnect resumes | system | manual+auto |
| INPUT-FLOOD-001 | synthetic event flood → bounded queue, no unbounded alloc | property | PR |
| AUDIO-NODEV-001 | no audio device → boots, plays, mute path | system | PR |
| AUDIO-RESIM-001 | resim emits zero SFX (counted via hook) | counter | PR |
| RENDER-BACK-001 | boot on each backend; `SCENE` smoke frames hash-stable per backend (tolerance) | golden-perceptual | nightly |
| GAME-AUTOVS-001 | AUTOVS per fighter×stage (NET-DET-VAR-001 doubles as gameplay coverage) | hash | nightly |
| GAME-BUGREG-* | each `docs/bugs/` gameplay entry → replay/takeover reproduction where scriptable | golden | nightly |

### D.10 GUI / a11y / i18n (mostly manual + screenshot)

| ID | Case | Type |
|----|------|------|
| GUI-NAV-001 | full menu navigation w/o mouse (kbd+pad) | manual charter + scripted key injection |
| GUI-SCALE-001 | 4:3/16:9/extreme AR screenshots, no clipped critical UI | golden-visual, tolerance |
| GUI-OVERS-001 | overscan crop on/off (regression `overscan_black_border`) | screenshot |
| GUI-TOUCH-001 | Android fixed-stick regions after configChanges (regression `android_landscape_lock`) | device manual |

### D.11 Updater (`UPD`)

| ID | Case | Hooks | Tier |
|----|------|-------|------|
| UPD-INT-001 | tampered zip + forged digest → reject (F-001 acceptance) | fake release server | PR |
| UPD-COR-001 | truncated download → reject, no partial install | fake server | PR |
| UPD-INTERRUPT-001 | kill during install script → rollback or safe restart, exe intact | controlled | nightly |
| UPD-VER-001 | downgrade attempt, older/newer manifest → policy result | unit | PR |
| UPD-NONET-001 | no network → graceful "unavailable", game unaffected | fake server down | PR |
| UPD-SEC-001 | no secrets/tokens in updater logs; paths don't leak user dirs unredacted | log scan | PR |

### D.12 Boot / lifecycle (`BOOT`)

| ID | Case | Tier |
|----|------|------|
| BOOT-FIRST-001 | clean profile → first-run flow completes headless | smoke |
| BOOT-NOASSET-001 | missing o2r → clear error, exit code, no hang | smoke |
| BOOT-TWO-001 | two instances → second handles lock/contention per policy | smoke |
| BOOT-CRASH-001 | forced crash → crash handler produces dump+symbolizable trace (regression `crash_handler_devnull_probe` — probe must not target /dev/null) | nightly |
| BOOT-UNICODE-001 | install path with Unicode/spaces/long path → works | matrix |

### D.13 Security (`SEC`) — mapped to `docs/security/` threat rows

| ID | Threat | Test |
|----|--------|------|
| SEC-REPLAY-001 | replayed INPUT datagram | dedup/stat + no re-apply |
| SEC-IMPERS-001 | forged from_slot via FORWARD | reject (LOBBY + client both) |
| SEC-DESER-* | every NET-PROTO negative | codec suite |
| SEC-OVER-001 | declared length overflow → no alloc/amplification | unit |
| SEC-FLOOD-* | socket/room exhaustion | lobby rate-limit suite |
| SEC-DESYNC-001 | intentional desync actor | detected, honest peer unaffected |
| SEC-DOWNG-001 | protocol downgrade attempt | version field is exact-match; reject |
| SEC-SECL-001 | logs contain no upkeys/tokens/IPs-unredacted | log scan in every harness |
| SEC-HOOK-001 | release binary contains no mutant/fuzz env strings | `strings` gate (F-002) |

### D.14 Property/metamorphic/model/mutation

- **Properties** (PBT, once gtest lands — use rapidcheck or hand-rolled
  generators): `parse(serialize(x))==x` for all wire messages; rejected
  payload → zero state mutation; `serialize` canonical (same input → same
  bytes); migration idempotent; path normalize never escapes sandbox;
  rate limiter never exceeds model.
- **Metamorphic**: identical replay twice → identical trace; cosmetic-only
  changes (volume, resolution, HiRes pack) → identical sim hash;
  latency↑ lossless → same result, different rollback pattern;
  headless vs rendered → identical sim hashes.
- **Model-based**: lobby room lifecycle + session state machine random walks
  against `docs/netplay/protocol/lifecycle.md` states.
- **Mutation**: the existing `MUTANT` suite is *engine* mutation; add
  source-level mutation for the codec (invert bounds, `&&`→`||`, drop
  version check) once it's a pure target — survivors = weak assertions.

### D.15 Characterization / golden

Golden Masters (versioned, reviewable diffs — never auto-blesed):

| Corpus | Format | Source |
|--------|--------|--------|
| Wire messages | hex vectors | spec §messages |
| Sim traces | per-tick hash lines | `CONFIRMED_TRACE` |
| Replays | recorded input files | `REPLAY_RECORD` |
| Config migrations | before/after JSON | post-F-011 |
| Save migrations | before/after | post-F-011 |
| Screenshots | PNG + diff mask | screenshot hooks (`SCREENSHOT_*`) |
| O2R golden | hash manifest per region | requires-rom, local only |

### D.16 Docs/DX self-test

| ID | Case |
|----|------|
| DOCS-001 | `check_docs.py` in CI (links, path refs) |
| DOCS-002 | BUILDING.md command smoke: clean checkout → configure → build on a fresh runner quarterly |
| DOCS-003 | every documented env var exists in code; every `SSB64_*` in code is cataloged (env-var doc check, F-010) |
| DOCS-004 | protocol code changed but spec unchanged → CI warning (rule exists in strategy.md; needs impl) |

---

## E. Error matrix per component

| Component | Input error | Dep error | State error | Concurrency | Resource | Compat | Security | Recovery |
|-----------|-------------|-----------|-------------|-------------|----------|--------|----------|----------|
| codec | malformed len/type ✓ | — | wrong-state msg | — | oversized pkt | wrong version | forgery/replay | bad pkt → continue |
| transport | unreachable | DNS fail | — | send/recv overlap | queue full | v1/v2 | flood | reconnect, relay migrate |
| rollback | late input | snapshot absent | resim state | — | ring full | — | malicious hash | unrecoverable → abort match |
| lobby | bad SSBL line | DO storage | room full | join/leave race | rate limit | v1 client | upkey misuse | DO restart |
| save | corrupt file | disk full | versioned | — | backup | old fmt | path traversal | backup restore |
| config | bad type/range | unwritable | unknown key | — | huge file | old schema | — | defaults+backup |
| updater | bad manifest | net down | partial install | concurrent check | disk full | old/new ver | forged digest | rollback |
| mods | bad manifest | TinyCC fail | conflict hooks | — | — | api ver | sim-unsafe in netplay | safe mode |
| assets | missing/corrupt | torch fail | stale recipe | — | disk | region | prohibited blob | re-extract |

✓ = covered today; rest = catalog rows above.

---

## F. Fixtures & corpus

```
tests/
├── unit/            (existing 4 files + gtest suite)
├── netplay/         codec vectors, protocol corpus
├── fuzz/            libFuzzer targets (post-F-003): packet codec, SSBL parser,
│                    config, save, manifest, updater manifest
├── fixtures/
│   ├── protocol/    valid+invalid wire vectors (committed — synthetic, legal)
│   ├── saves/       synthetic saves (committed); real-format fixes only
│   ├── config/      valid/invalid/boundary configs
│   ├── replays/     recorded input replays (legal: they're inputs, not assets)
│   └── assets/      hash manifests only — never ROM-derived content
├── golden/          traces, screenshots, serialized vectors
└── fakes/           FakeTransport, FakeLobby, FakeUpdaterServer,
                     InMemorySaveStore, FakeClock
```

Corpus policy: every crash found by fuzz → minimized regression file in
`fixtures/` + test. Seeds committed; derived artifacts regenerated.

---

## G. Implementation plan

| Phase | Deliverable | Prereq | Effort |
|-------|-------------|--------|--------|
| T0 | gtest harness + first codec suite (NET-PROTO-*) + `check_docs` CI job | F-003 helps but codec tests can start against extracted parse fn | M |
| T1 | env-var catalog (F-010) + `SSB64_TEST_*` rename + release strip (F-002) | — | S |
| T2 | FakeTransport + loopback; component tests NET-TRANS-CUT/MIG | seam | M |
| T3 | save header (F-011) + SAVE-* suite + CFG-* | — | M |
| T4 | FakeUpdaterServer + UPD-* suite (incl. F-001 fix test) | F-001 | M |
| T5 | `smash-classic-sim` packaged target + ctest labels/presets | — | S |
| T6 | lobby conformance suite (LOBBY-CONF-001) | shared test spec | M |
| T7 | libFuzzer targets + corpus + ASan/UBSan presets | T0 | M |
| T8 | golden assets (ASSET-GOLD) + replay corpus + bug-regression replays | ROM-gated | M |
| T9 | NET-DET-XPAT cross-platform, GUI visual suite, a11y charters | — | L |

---

## H. Labels & presets

CTest labels: `unit component integration contract system e2e determinism
rollback netplay lobby security fuzz sanitizer performance slow
flaky-quarantine requires-rom requires-assets windows linux android uwp
release smoke`.

Presets (extend `CMakePresets.json` testPresets):

| Preset | Includes | Excludes | Timeout | Use |
|--------|----------|----------|---------|-----|
| test-unit | unit | — | 2m | dev loop |
| test-pr | unit;component;contract;netplay codec;lobby;smoke | requires-rom;slow;fuzz | 10m | PR gate |
| test-netplay | netplay;rollback;lobby | requires-rom(except local loopback) | 20m | netplay PRs |
| test-nightly | all minus flaky-quarantine | — | 90m | cron |
| test-sanitizers | unit+fuzz smoke under ASan/UBSan | tsan-incompatible | 30m | nightly |
| test-rom | requires-rom suites | — | 60m | gated |
| test-release | everything + packaging smoke | flaky-quarantine | per-job | release |

---

## I. Quality gates (delta from `strategy.md`)

New enforceable gates:
- `strings(Release.exe) | grep SSB64_.*MUTANT|PACKETFUZZ|COMPAT_TOKEN` → empty (F-002)
- codec suite green on every PR touching src/networking/netplay/protocol (planned) or components/decomp/sys/netpeer*
- lobby conformance green when either lobby impl changes
- updater tests must include a negative (tampered) case — a suite that only
  tests the happy path is red
- every `docs/bugs/` entry references its regression case ID or `manual` + charter

## Coverage-closing matrix (final)

| Area | Covered by | Automatable | Gaps / pending risk |
|------|------------|-------------|---------------------|
| happy paths | AUTOVS, lobbytest, codec vectors | yes | — |
| boundary | NET-PROTO-LEN/-CNT, RING, window edge | yes | — |
| invalid inputs | codec negatives, lobby fuzz, CFG/SAVE corrupt | yes | — |
| invalid states | wrong-state msgs, model-based walks | yes | model not built yet |
| partial errors | interrupted write/install/extract | yes | needs fault injection hooks |
| concurrency | lobby races, resim gates; TSan preset | partial | TSan on MSVC unsupported → Linux preset |
| security | SEC-* table, lobby security suite | yes | E2E lobby compromise residual |
| perf | none today — benchmarks unplanned | future | frame-time baseline needed |
| soak | netplay_soak exists | yes | rematch approx only |
| platforms | windows/linux; android/uwp manual | partial | no device CI |
| a11y | charters only | no | all manual by nature |
| privacy | SEC-SECL-001 log scan + diag-bundle audit | yes | bundle doesn't exist yet |
| install/update/rollback | UPD-* w/ fake server | yes | blocked on F-001 seam |
| no-net | UPD-NONET, BOOT-NOASSET | yes | — |
| resource-exhausted | queue full, ring full, disk full | partial | needs fakes |
| corrupt data | SAVE/CFG/ASSET/O2R corrupt | yes | — |
| malicious client | mutation suite + SEC-* | yes | — |
| abrupt interrupt | NET-TRANS-KILL, BOOT-CRASH | yes | — |
| offline behavior | LAN path, no-lobby flows | yes | — |

Non-claim: the input/state space is unbounded; this matrix asserts
systematic coverage of every *identified* category, boundary partition,
state transition and failure mode — not exhaustive coverage.
