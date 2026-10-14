# Technical debt register

Architectural index of tracked debt. Each entry that is executable work
should also have a GitHub issue; this file is the map, not the tracker.

| ID | Component | Description | Impact | Desired resolution | Status |
|----|-----------|-------------|--------|--------------------|--------|
| TD-001 | src/netplay | ~~Flat module — protocol/transport/rollback/lobby live in 4 files~~ | Netplay changes couple unrelated concerns | Split per `docs/decisions/0004-netplay-module-boundary.md` | **done**: `src/netplay/{domain,protocol,rollback,session,transport,diagnostics}` — pure wire codec in `protocol/netpacket.*` (covered by `tests/unit/netplay/protocol/netpacket_test.c`), transports renamed `udp_transport`/`websocket_transport`, session/lobby in `session/netlobby.*` |
| TD-002 | CMake | Single-root target model; `Codegen`/`RuntimeAssets` extracted, component targets not | Can't enforce module dependency rules or per-target warnings | `smash_*` library targets with explicit deps | planned |
| TD-003 | submodules | decomp/libultraship/torch at root | Root clutter; logical boundary documented but not physical | Move to `game/` + `third_party/` once worktree tooling is verified | deferred (ADR-0002) |
| TD-004 | saves | ~~Single unversioned format; no atomic write/migration~~ | Corrupt save = data loss; format change breaks users | `.meta` sidecar `format_version` + `kMigrations[]` dispatch chain (`runSaveMigrations` rewrites image via same temp+rename, re-stamps sidecar through the write-once flag); temp+fsync+atomic-rename writes; write failures surface in-game. In-image versioning deliberately rejected — flat SRAM chip mirror, absolute offsets | done |
| TD-005 | config | ~~Untyped CVar bag; no schema or unknown-key policy~~ | Silent misconfig; no local-vs-sim split for netplay | `src/config/schema.{h,cpp}` catalogs all `g*` keys with type + range + `affects_gameplay` flag; boot validation clamps OOB values, warns on type mismatch and unknown keys; `gConfig.SchemaVersion` stamp hooks future migrations; `check_cvar_schema.py` enforces catalog coverage in CI; `ConsoleVariable::GetVariableNames()` + `CVarExists` implemented (was a declared-never-defined bridge stub) | done |
| TD-006 | assets | No golden/hash tests for generated archives | Asset regression ships silently | fixtures + structural validation | **done (2026-10-14)**: `tests/integration/assets/extraction_recipes_test.py` — structural validation + sha256 golden manifest over all 246 recipes, wired into repo-checks.yml; `--update` after intentional recipe edits |
| TD-007 | mods | No `apiVersion`/`simulation` manifest fields | Incompatible/gameplay mods load into netplay | manifest fields + registry enforcement | **done (2026-10-14)**: `code-version` was already enforced by ScriptLoader; new `"simulation"` manifest field (default true) surfaces via ModRegistry and exempts cosmetic-only mods from the netplay compat hash |
| TD-008 | netplay | No session manifest handshake | Same-protocol different-build peers desync silently | pre-match manifest exchange + verdict | **done (2026-10-14)**: PICK token is now `<build_sha>:<mod_content_hash>:<asset_archive_hash>` — mod archives hash their payload bytes (folder mods hash relpaths+sizes) and the base o2r is FNV-1a hashed once (cached); different code, mods, or extracted assets all fail closed pre-match |
| TD-009 | updater | sha256 check exists but digest comes from the SAME release metadata as the payload — corruption check, not tamper resistance; Windows zip unsigned | Forged release ships both zip+digest | verify artifact attestation (Sigstore) or offline-signed manifest in `Updater.cpp` | **done (2026-10-14)**: `SHA256SUMS.txt` is minisign-signed (`-l`, raw Ed25519) in release.yml from `MINISIGN_SECRET_KEY`; updater verifies the manifest against an embedded pubkey (orlp/ed25519, FetchContent-pinned) and derives the authoritative digest from it — unsigned/mismatched manifests fail closed. Provisioning: docs/development/release_signing.md |
| TD-010 | fuzzing | Only scripted packet fuzz + lobby fuzz | Parser edge cases unprobed | libFuzzer targets (net/save/config/replay/o2r/manifest) | **partial**: `SSB64_FUZZ_{PARSER,INPUT}` test hook + `tools/fuzz/run_parser_fuzz.py` drive manifest (`Archive::ParseManifestBuffer`, extracted as pure parse), replay (`syNetReplayLoadDebugFile`), and raw-json parsers through the real binary; seeds under `tests/fuzz/corpora/`; crashes to `out/fuzz/crashes/`. libFuzzer coverage-guided targets for o2r index / save / config still open (see `docs/testing/fuzzing.md`) |
| TD-011 | observability | ~~printf-style `port_log`; no categories/redaction/correlation~~ | Netplay debugging is grep-archaeology | `port_log_ex(level, category, ...)` + `port_log_set_sink_ex` extended sink + `port_log_set_session` correlation id; JSONL records carry `{"t","sid","lvl","cat","msg"}` with `token=`/`key=`/`secret=`/`password=` redaction and home-dir collapse; crash bundles via TD-015 | done |
| TD-012 | supply chain | No OpenSSF Scorecard | Repo posture unmeasured | scorecard workflow + badge | **done (2026-10-14)**: `.github/workflows/scorecard.yml` — weekly + on-push analysis, SARIF to code scanning, SHA-pinned |
| TD-013 | CI | dependency-review action still absent; lobby tests + check_docs + env-var catalog now wired via `repo-checks.yml` | Vulnerable deps can merge | `actions/dependency-review-action` on PRs | **done (2026-10-14)**: `.github/workflows/dependency-review.yml` — fails PRs on high-severity dependency additions |
| TD-014 | release | CycloneDX SBOM now generated per release via `tools/lint/gen_sbom.py` (submodules + FetchContent pins + npm lockfile) | — | done (2026-10-14); vcpkg/system-lib enumeration still shallow | closed |
| TD-015 | diagnostics | ~~`port_symbolize` exists but no crash bundle/privacy filter~~ | Crash reports unactionable | `src/diagnostics/crash_bundle.*` — signal-safe marker + next-boot bundle assembly into `logs/crashes/` with rotation; per-release symbol archives (`.pdb` zip / `.debug` tar.xz / `.dSYM` zip) shipped beside each platform artifact; `/Zi`+`/DEBUG`/`-g` moved out of the scripting gate so script-disabled builds still symbolize; shipped binaries stripped of DWARF | done |
| TD-016 | services/netplay-lobby | `npm audit`: 4 high-severity findings in transitive `wrangler` deps (miniflare/undici chain); `npm audit fix --force` would jump major versions and risk breaking the deploy tool | Lobby deploy tooling carries known vulns (dev-only surface, not shipped) | Track wrangler upstream; bump when a compatible release lands; keep `ws` pinned | **done (2026-10-14)**: `wrangler` bumped 4.136.3 → 4.149.0 (first release clearing the sharp/undici advisories); `npm audit` reports 0 vulnerabilities; lobby tests 21/21 + fuzz PASS |

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
  `platforms/*/packaging/package-*` scripts (fails the package if a gated
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
- Mod archive verification: `Archive::IsChecksumValid()` + `IsSigned()`
  wired to `ReleaseSig::VerifyEd25519MinisigFields`; the Mods menu shows
  `[signed]` / `[signature invalid]` badges. Optional minisign `-l`
  signing supported via `tools/modding/package_mod.py --checksum
  --sign-key <key> --pubkey <pub>`.
