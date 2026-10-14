# Technical debt register

Architectural index of tracked debt. Each entry that is executable work
should also have a GitHub issue; this file is the map, not the tracker.

| ID | Component | Description | Impact | Desired resolution | Status |
|----|-----------|-------------|--------|--------------------|--------|
| TD-001 | src/netplay | Flat module — protocol/transport/rollback/lobby live in 4 files | Netplay changes couple unrelated concerns | Split per `docs/decisions/0004-netplay-module-boundary.md` | planned |
| TD-002 | CMake | Single-root target model; `Codegen`/`RuntimeAssets` extracted, component targets not | Can't enforce module dependency rules or per-target warnings | `smash_*` library targets with explicit deps | planned |
| TD-003 | submodules | decomp/libultraship/torch at root | Root clutter; logical boundary documented but not physical | Move to `game/` + `third_party/` once worktree tooling is verified | deferred (ADR-0002) |
| TD-004 | saves | Single unversioned format; no atomic write/migration | Corrupt save = data loss; format change breaks users | schemaVersion + migration + atomic write | **partial (2026-10-14)**: `.meta` sidecar now records format_version + size sanity-check on read; atomic write + in-image versioning still open |
| TD-005 | config | Untyped CVar bag; no schema or unknown-key policy | Silent misconfig; no local-vs-sim split for netplay | typed schema + migrations (planned config module under `src/`) | open |
| TD-006 | assets | No golden/hash tests for generated archives | Asset regression ships silently | fixtures + structural validation | **done (2026-10-14)**: `tests/unit/test_extraction_recipes.py` — structural validation + sha256 golden manifest over all 246 recipes, wired into repo-checks.yml; `--update` after intentional recipe edits |
| TD-007 | mods | No `apiVersion`/`simulation` manifest fields | Incompatible/gameplay mods load into netplay | manifest fields + registry enforcement | open |
| TD-008 | netplay | No session manifest handshake | Same-protocol different-build peers desync silently | pre-match manifest exchange + verdict | **done (2026-10-14)**: PICK token is now `<build_sha>:<mod_content_hash>:<asset_archive_hash>` — mod archives hash their payload bytes (folder mods hash relpaths+sizes) and the base o2r is FNV-1a hashed once (cached); different code, mods, or extracted assets all fail closed pre-match |
| TD-009 | updater | sha256 check exists but digest comes from the SAME release metadata as the payload — corruption check, not tamper resistance; Windows zip unsigned | Forged release ships both zip+digest | verify artifact attestation (Sigstore) or offline-signed manifest in `Updater.cpp` | open |
| TD-010 | fuzzing | Only scripted packet fuzz + lobby fuzz | Parser edge cases unprobed | libFuzzer targets (net/save/config/replay/o2r/manifest) | open |
| TD-011 | observability | printf-style `port_log`; no categories/redaction/correlation | Netplay debugging is grep-archaeology | `LogSink` seam → structured `LogEvent` + diagnostic bundle | open |
| TD-012 | supply chain | No OpenSSF Scorecard | Repo posture unmeasured | scorecard workflow + badge | **done (2026-10-14)**: `.github/workflows/scorecard.yml` — weekly + on-push analysis, SARIF to code scanning, SHA-pinned |
| TD-013 | CI | dependency-review action still absent; lobby tests + check_docs + env-var catalog now wired via `repo-checks.yml` | Vulnerable deps can merge | `actions/dependency-review-action` on PRs | **done (2026-10-14)**: `.github/workflows/dependency-review.yml` — fails PRs on high-severity dependency additions |
| TD-014 | release | CycloneDX SBOM now generated per release via `tools/lint/gen_sbom.py` (submodules + FetchContent pins + npm lockfile) | — | done (2026-10-14); vcpkg/system-lib enumeration still shallow | closed |
| TD-015 | diagnostics | `port_symbolize` exists but no crash bundle/privacy filter | Crash reports unactionable | diagnostics module under `src/` + symbol archive per release | open |
| TD-016 | services/netplay-lobby | `npm audit`: 4 high-severity findings in transitive `wrangler` deps (miniflare/undici chain); `npm audit fix --force` would jump major versions and risk breaking the deploy tool | Lobby deploy tooling carries known vulns (dev-only surface, not shipped) | Track wrangler upstream; bump when a compatible release lands; keep `ws` pinned | open (2026-10-14) |

## TODO comment policy

Vague TODOs are disallowed in `src/`. Use `// TODO(#NNNN): what + why +
removal condition` — the issue number is mandatory. `decomp/` keeps its
upstream comment style.

## Fixed this cycle (2026-10-14)

- Test/mutation/fuzz/forge hooks compiled out of shipped builds
  (`-DSSB64_TEST_HOOKS`, OFF in packaging scripts, verified by a
  binary-strings gate). Gated surface: netplay mutants/packetfuzz/test
  desync/input fuzz, UDP impairment sim, compat-token forge, rollback rig
  (AutoVS/SyncTest/mutant), replay record/play/takeover + rig-exit,
  sync trace/verify oracle, item/container forcing, scene/boot overrides,
  `NO_MOTIONFIX`, `SCENE_HEAP_FREE`, `ADDR_JITTER`, `MAX_FRAMES`,
  `WIZARD_AUTOCANCEL`.
- Release-binary check enforced: `tools/lint/check_release_binary.py`
  derives the denylist from `env_vars.md` and runs inside all three
  `scripts/packaging/package-*` scripts (fails the package if a gated
  env name appears in the binary).
- Env-var catalog: `docs/development/env_vars.md` enumerates all 103
  `SSB64_*` reads; `tools/lint/check_env_vars.py` fails CI on
  undocumented vars.
- Save format: `port_save.cpp` writes a `.meta` sidecar with
  `format_version`/`game_version`/`sram_size`, logs on size mismatch and
  on newer-format sidecars (warn-and-read, never discards user saves).
- Repo hygiene: legacy `ci/` → `platforms/uwp/ci/`, `prs.json` removed,
  `server/` → `services/netplay-lobby/`, empty placeholders removed.
- FetchContent deps now all SHA-pinned (funchook, discord-rpc, uwp-dep).
- Governance: CODEOWNERS, PR template, GOVERNANCE/MAINTAINERS/SUPPORT/
  CODE_OF_CONDUCT, `.editorconfig`, CHANGELOG, `.git-blame-ignore-revs`.
- CI: `repo-checks.yml` (docs check, env-var catalog, lobby security+fuzz,
  prohibited artifacts, SHA-pin enforcement).
- Release: `SHA256SUMS.txt` + artifact attestation for assets and the
  manifest + CycloneDX SBOM published with each release.
