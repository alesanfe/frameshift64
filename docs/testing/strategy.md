# Testing strategy & quality gates

A merge must not happen just because the build compiles. Checks are
classified by tier — each tier exists so the *expensive* truth runs on a
schedule and the *cheap* truth gates every PR.

## Tier: PR (fast, mandatory)

- Format + lint (`tools/lint/run_project_lint.py`, pre-commit hooks)
- Windows + Linux build
- Unit tests (`tests/unit/`)
- Docs check (`tools/lint/check_docs.py` — links, file refs)
- Dependency review (TD-013)
- Generated-files check: codegen outputs committed must match regen
- Netplay protocol test (scripted, no ROM): `netplay_lobbytest*.ps1`,
  `test-lobby-security.js`

## Tier: nightly (matrix)

- All platforms incl. Android/UWP
- Sanitizers (ASan/UBSan) + `-DPACKET_FUZZ` suites
- `determinism.yml`: SyncTest distances 1/3/6, localtest clean+degraded
  matrix (latency/jitter/loss/dup/reorder/corrupt), mutation suite,
  cross-build Release≡Debug trace compare
- Save/asset fixture suites when they land

## Tier: release

- All of the above + packaging per platform + smoke test of the *packaged*
  artifact + SBOM (TD-014) + signatures/attestations + update/rollback
  verification

## PR-change → required-artifact rules (partially enforceable in CI)

| If a PR touches… | it must also… |
|------------------|---------------|
| `decomp/src/sys/netpeer*.c`, `src/netplay/` | update `docs/netplay/protocol/` or justify |
| `include/smash_classic/` public headers | update `CHANGELOG.md` |
| architecture decisions | add/update an ADR in `docs/decisions/` |
| config keys | add migration + doc |
| a bugfix | add a regression fixture/test where viable + `docs/bugs/` entry |

## Headless determinism harness (design — TD in register)

Target executable `smash-classic-sim`: load scene → fix RNG seed → replay
inputs → run N frames headless → emit per-tick state hashes → report the
first divergent frame with component-level diff (fighter velocities, item
counts, RNG state). Today the closest tools are `rollback_synctest.ps1`
and the confirmed-trace diff in `netplay_localtest.ps1`; the dedicated
harness is the planned unification.

## Replay corpus (target layout)

    tests/support/fixtures/replays/
    ├── 1v1/ 4-player/ items/ stages/ grabs/ projectiles/
    ├── respawns/ edge-cases/ historical-regressions/

Every significant fixed desync/simulation bug should land a replay or a
recorded trace as its regression fixture.

## Test catalog

The full risk-traced catalog — stable case IDs, seams, oracles, error
matrix, fixtures, CTest labels and presets — lives in
[`test-catalog.md`](test-catalog.md).
