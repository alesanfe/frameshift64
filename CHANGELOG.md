# Changelog

All notable changes to this project are documented here. Format loosely
follows [Keep a Changelog](https://keepachangelog.com/); versions follow the
`VERSION` file and per-surface compatibility constants in
`include/smash_classic/version.h`.

## Unreleased

### Changed
- Repository reorganized by responsibility: `port/` → `src/`, `server/` →
  `services/netplay-lobby/`, `workspace/` → `mods/`, `yamls/` →
  `assets/configuration/extraction/`, `android/`+`uwp/` → `platforms/`,
  tooling under `tools/`+`scripts/`+`config/`, generated output under `out/`.
- All GitHub Actions pinned to commit SHAs; all `FetchContent` deps pinned
  to commit SHAs.
- Netplay protocol, compatibility surfaces and security posture documented
  normatively under `docs/netplay/protocol/`, `docs/compatibility/`,
  `docs/security/`.
- Save files now carry a `.meta` sidecar with `format_version` /
  `game_version` / `sram_size`; size mismatches and newer-format sidecars
  log loudly (warn-and-read, user saves are never discarded).

### Security
- All test/mutation/fuzz/desync/boot-override env hooks are compiled only
  with `-DSSB64_TEST_HOOKS=ON` (default ON for dev builds); packaging
  scripts force it OFF and `tools/lint/check_release_binary.py` fails the
  package if any gated env-var name appears in the shipped binary.
- Releases publish `SHA256SUMS.txt`, a CycloneDX SBOM
  (`smash-classic.cdx.json`), and Sigstore attestations for assets and the
  checksum manifest.

### Added
- Independent compatibility version constants (`include/smash_classic/version.h`).
- `tools/lint/check_docs.py` documentation checker,
  `tools/lint/check_env_vars.py` env-var catalog gate (103 `SSB64_*` vars),
  `tools/lint/check_release_binary.py` release strings gate,
  `tools/lint/gen_sbom.py` SBOM generator.
- `repo-checks.yml` workflow: docs check, env-var catalog, lobby
  security+fuzz suites, prohibited-artifact scan, SHA-pin enforcement.
- Issue templates, `.gitattributes`, `.editorconfig`, CMake presets.
- Test catalog with stable case IDs (`docs/testing/test-catalog.md`).
