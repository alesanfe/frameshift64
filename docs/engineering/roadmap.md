# Release Roadmap

Public plan for upcoming releases. Dates are targets, not commitments —
each release ships when its quality gates (determinism suite, regression
catalog, smoke tests) are green. Version numbers follow the compatibility
surfaces in `include/smash_classic/version.h` (ADR 0005).

## v1.7 — Quality hardening (current)

Focus: close the gaps the quality audit identified (docs/investigations).
No new features gated behind this release.

- [x] Compiler-enforced module boundaries (`SSB64_MODULE_SCOPE_*`)
- [x] LLVM coverage workflow (`coverage.yml`) — published metrics, gate TBD
- [x] ROM-free determinism job on PRs (`determinism.yml` `no-rom`)
- [x] Desync/rollback metrics aggregation (`tools/analysis/netplay_metrics.py`)
- [x] `netlobby.cpp` / `lbreloc_byteswap.cpp` split under the 2000-LOC bar
- [ ] Coverage baseline recorded → diff-coverage gate on PRs
- [ ] Crash-bundle + diag docs refresh for the new `src/` layout

## v1.8 — Online depth

Focus: make online play match Slippi-class expectations.

- [ ] Matchmaking / quick-play queue on the lobby service
- [ ] Local Elo + season scaffolding (no server account needed)
- [ ] Friends list + invites (lobby service v3 room protocol)
- [ ] Desync-rate dashboard fed by `metrics.json` artifacts (30-day view)

## v1.9 — Modding SDK

- [ ] `smash-mod` CLI (init/build/package/publish)
- [ ] Mod dependency resolution (`requires:` in mod manifests)
- [ ] In-game mod browser backed by a community index

## v2.0 — Cross-platform parity

- [ ] Android feature parity (menus, netplay, replays) — currently partial
- [ ] Steam Deck verified preset (Proton + controller profile)
- [ ] Performance regression CI (frame-time budget on golden scenes)
- [ ] Screenshot regression across all menus/locales

## Backlog (unscheduled)

Tournament mode + brackets, spectator improvements, replay sharing/search,
cloud saves, Discord Rich Presence beyond status, ranked seasons.

## How releases are cut

`release.yml` builds per-platform packages + manifests + signatures.
A release requires: green `build.yml` on all three desktop platforms,
green `determinism.yml` rollback suite (when `BASEROM_US_Z64` is set),
an entry in the changelog notes, and the version bump following ADR 0005.
