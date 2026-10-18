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
| 1945 | src/presentation/Menu.cpp | pending |
| **1790→366** | src/game/engine/decomp/lbreloc_byteswap.cpp | **done** — split into lbreloc_fixup_{state,chain,runtime,sprite}.cpp + lbreloc_fixup_internal.h |
| 1780 | src/presentation/PortMenu.cpp | pending |
| 1680 | src/game/engine/decomp/lbreloc_bridge.cpp | pending |
| 1353 | src/application/port.cpp | pending |
| 1290 | src/presentation/UIWidgets.cpp | pending |
| 1254 | src/presentation/PortMenuCustomization.cpp | pending |
| 1224 | src/observability/heap_check_boot.cpp | pending |
| 1217 | src/game/engine/relocation/SyntheticReloc.cpp | pending |
| 1162 | src/application/update/downloads/ModPackDownloader.cpp | pending |
| 1115 | src/game/engine/css_icons/port_css_stage_assets.cpp | pending |
| 1044 | src/networking/netplay/transport/websocket_transport.cpp | pending |
