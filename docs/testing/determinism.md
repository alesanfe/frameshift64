# Determinism testing

Determinism is a correctness requirement: peers that diverge desync, and
desync in production is silent match corruption.

## What exists today

- **SyncTest** (`scripts/development/rollback_synctest.ps1`): snapshot →
  resimulate self-check each tick at configurable rollback distance.
  Distance 6 is the worst case (`SYNETPEER_MAX_PREDICTION`).
- **Localtest** (`scripts/testing/netplay_localtest.ps1`): two real
  instances over loopback UDP with seeded fuzz inputs; confirmed-state
  traces are diffed line-by-line.
- **Degraded-net matrix**: `-LatencyMs -JitterMs -LossPct -DupPct
  -ReorderPct -CorruptPct` knobs apply loss → corruption → delay/jitter →
  reorder → duplication in that order, seedable via
  `SSB64_NETPLAY_SIM_SEED`.
- **Mutation suite** (`scripts/testing/netplay_mutation_suite.ps1`):
  each run removes one validation (e.g. `SSB64_ROLLBACK_MUTANT=
  skip_rng_restore`) and MUST fail — a passing mutant means the guard is
  dead code.
- **Packet fuzz** (`-PacketFuzz N`): seeded hostile datagrams through the
  real parser; the harness must not crash and must keep counting drops.
- **Cross-build** (`determinism.yml` `cross-build` job): Release vs Debug
  traces must be identical for the same seed.

## Gaps

- No single-headless-process determinism check (needs
  `smash-classic-sim`, see strategy.md).
- No replay corpus in `tests/fixtures/replays/`.
- libm parity is solved by compiling the decomp's own trig — any new
  transcendental in game code must go through the same path, not host
  libm (`docs/bugs/` → `trig_host_libm_divergence`).
