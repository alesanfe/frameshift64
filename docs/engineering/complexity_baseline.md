# Complexity Baseline (lizard 1.23)

Generated: `python -m lizard src/ --csv > out/lizard_full.csv`
Targets (professionalization plan, phase 0): **CCN < 15**, **NLOC < 100** per function.

## Snapshot

| Metric | Count |
|---|---|
| Functions analyzed | 1551 |
| CCN >= 15 | 116 |
| NLOC >= 100 | 28 |

## Worst offenders (CCN)

| CCN | NLOC | Function |
|---|---|---|
| 163 | 666 | `Ship::Menu::DrawElement` — src/presentation/Menu.cpp |
| 135 | 402 | `RenderNetplayLobby` — src/presentation/PortMenuOnline.cpp |
| 101 | 421 | `portBuildSyntheticRelocResource` — src/game/engine/relocation/SyntheticReloc.cpp |
| 95 | 314 | `Ship::Menu::MenuDrawItem` — src/presentation/Menu.cpp |
| 82 | 287 | `InstallWorker` — src/application/update/downloads/ModPackDownloader.cpp |
| 76 | 214 | `portUnOtrDisplayList` — src/game/engine/relocation/SyntheticReloc.cpp |
| 75 | 397 | `PortInitImpl` — src/application/port.cpp |
| 65 | 68 | `port_diag_get_scene_name` — src/platform/compatibility/port_diag_stubs.c (switch-table, mechanical) |
| 59 | 153 | `main` — src/application/port.cpp |
| 55 | 266 | `portRelocLoadFileFromBytesImpl` — src/game/engine/decomp/lbreloc_bridge.cpp |
| 49 | 152 | `on_server_text` — src/networking/netplay/session/netlobby.cpp |

## Strategy

**Ratchet, not big-bang.** Files >1000 LOC get split by responsibility first
(phase 0.1); per-function CCN/NLOC refactors happen opportunistically after
the split. A lint gate (`tools/analysis/lint/check_complexity.py`) blocks
*new* violations while the allowlist records the pre-existing ones — the
allowlist only ever shrinks.

### Exempt by design

- `components/` submodules (decomp, LUS, Torch) — out of scope; decomp keeps
  IDO semantics per AGENTS.md directive 5.
- Switch-table lookups like `port_diag_get_scene_name` (CCN counts branches,
  the function is mechanically fine).

## File-size table (>1000 LOC)

| LOC | File | Status |
|---|---|---|
| **2054→565** | src/networking/netplay/session/netlobby.cpp | **done** — split into netlobby_{state,transport,protocol,start}.cpp + netlobby_internal.h (largest TU: protocol 929) |
| 1945 | src/presentation/Menu.cpp | blocked — uncommitted edits in flight (DrawElement CCN=163 is the worst single function) |
| **1790→366** | src/game/engine/decomp/lbreloc_byteswap.cpp | **done** — split into lbreloc_fixup_{state,chain,runtime,sprite}.cpp + lbreloc_fixup_internal.h |
| 1780 | src/presentation/PortMenu.cpp | blocked — uncommitted edits in flight |
| **1680→336** | src/game/engine/decomp/lbreloc_bridge.cpp | **done** — split into lbreloc_{status,loader,query}.cpp + lbreloc_bridge_internal.h |
| **1353→1391** | src/application/port.cpp | **done** — PortInitImpl (714 LOC) split into 6 named stage functions (TestHooks/Context/FeatureLatches/CoreRuntime/AssetsAndServices/Mods); impl is now a 14-line sequence. Stage bodies still carry some >100-NLOC linear init blocks — tracked in the ratchet baseline |
| 1290 | src/presentation/UIWidgets.cpp | blocked — uncommitted edits in flight |
| 1254 | src/presentation/PortMenuCustomization.cpp | blocked — uncommitted edits in flight |
| **1224→247** | src/observability/heap_check_boot.cpp | **done** — split into heap_check_{report,hooks,gwatch}.cpp + heap_check_internal.h (MSVC Debug test-hooks only) |
| **1217→113** | src/game/engine/relocation/SyntheticReloc.cpp | **done** — split into synth_{unotr,build}.cpp + synth_reloc_internal.h |
| 1162 | src/application/update/downloads/ModPackDownloader.cpp | blocked — uncommitted edits in flight |
| **1115→78** | src/game/engine/css_icons/port_css_stage_assets.cpp | **done** — split into css_stage_build.cpp + css_stage_assets_internal.h |
| 1421 | src/networking/netplay/transport/websocket_transport.cpp | blocked — libcurl rewrite in flight |

## Golden gameplay tests (phase 1.3/1.4)

`tests/e2e/golden/golden_gameplay_test.py` replays committed synthetic input
tapes (`tests/support/fixtures/replays/*.ssr`, produced by
`tools/developer/testing/make_replay.py` — exact `SYNetReplayFileHeader` /
metadata / frame layout + the FNV-1a input checksum the verifier recomputes)
and diffs the per-tick `SSB64_SYNC_TRACE` state hashes against committed
goldens in `tests/support/golden/*.trace`.

- Cases: `idle_2p`, `mario_fair`, `fox_usmash`, `kirby_inhale`,
  `falcon_punch` (Peach's Castle, no items, fixed seed — see
  `tests/support/golden/manifest.json`).
- Game exit codes drive the verdict: 0 verified / 1 input FAIL /
  2 incomplete / 3 oracle load-fail / 4 DESYNC (`SSB64_RIG_EXIT=1`).
- Missing binary or `*.o2r` → exit 77 → CTest `SKIP_RETURN_CODE`, labelled
  `e2e;golden;deterministic;requires-rom`. Runs in `determinism.yml`'s
  ROM-gated job; skipped on forks/ROM-less CI.
- Goldens are per-build-flavor (MSVC Debug-captured); recapture with
  `--capture` if a legit config change diverges the sim — investigate
  first, a DESYNC is supposed to mean something.
- Verified locally: 5/5 PASS on `out/build/verify` (Debug); mutated trace
  → exit 4 as designed; one transient boot AV during capture (watch —
  if it recurs it is a real bug, not a test bug).
