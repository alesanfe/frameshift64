# Documentation Index

| Directory | Contents |
|-----------|----------|
| `architecture/` | Living design docs: [`README.md`](architecture/README.md) (section index + invariants), [`overview.md`](architecture/overview.md), [`build_and_tooling.md`](architecture/build_and_tooling.md), plus `designs/` for accepted design plans (interpolation, freeze-frame clock, widescreen, JP ROM compat…). |
| `development/` | Contributor reference: [`building.md`](development/building.md) (build how-to, moved from repo root), `c_conventions.md`, `n64_reference.md`, debugging guides (`debug_gbi_trace.md`, `debug_ido_bitfield_layout.md`, `state_trace.md`), `classic_coop.md`, `env_vars.md` (SSB64_* catalog), `generated-dir.md` (the `generated/` dir contract). |
| `modules/` | Per-`src/` module readmes (`application.md`, `networking-netplay.md`, `rendering.md`, …) — moved out of the source tree; each file's header names the directory it documents. |
| `netplay/` | Netplay design docs (living): `netplay_architecture.md`, `netplay_online.md`, `rollback_design.md`, `netcode_agent_rules.md`, `lobby-service.md` (the `services/netplay-lobby` readme). **`netplay/protocol/`** is the normative wire spec — see ADR-0006. |
| `modding/` | Mod authoring guide (`modding.md`) plus the former in-tree readmes: `mods-dir.md` (the `mods/` skeleton), `example-*.md`, `template-basic-mod.md`, `mod-api-readme.md` (`include/smash_mod_api`), `vendor-notes.md`. Runtime lives in `src/game/modding/`. |
| `platforms/` | Per-platform feasibility/status notes plus each `platforms/<os>/` readme (`android.md`, `linux.md`, `macos.md`, `windows.md`, `uwp.md`, `switch-dir.md`, `tree.md` dir index). |
| `project/` | Governance docs moved from repo root: `governance.md`, `maintainers.md`, `support.md`. |
| `specs/` | Machine-checkable contracts + their `.schema.json` (`netplay/` lobby protocol, `mod-api/` manifest, `save-format/`, `updates/`, `package-format/`) — moved from repo root `specs/`. |
| `compatibility/` | Version policy per surface (saves, config, assets, mods, netplay, lobby). Canonical numbers in `include/smash_classic/version.h`. |
| `security/` | Threat models (network, mods, updater, assets), supply-chain posture, incident response. |
| `testing/` | Test strategy + quality gates (`strategy.md`), determinism harnesses, fuzzing, and `README.md` (the former `tests/` suite-layout guide). |
| `engineering/` | Technical-debt register, migration status, known limitations, deprecations. |
| `decisions/` | ADRs — one file per accepted architectural decision. |
| `investigations/` | Dated investigations, audits, triage notes and handoffs. Point-in-time documents; not living docs. Netplay audits under `investigations/netplay/`. Latest: [`professionalization_audit_2026-10-14.md`](investigations/professionalization_audit_2026-10-14.md) — full-repo audit with findings register, phased plan and backlog. |
| `bugs/` | Resolved-bug write-ups, indexed from `bugs/README.md` (`<topic>_<YYYY-MM-DD>.md`). |
| `archive/` | Superseded docs kept for history. |

Rule of thumb: **living docs** describe the current design (architecture/, development/, netplay/, modding/, platforms/, security/, testing/); **dated docs** describe a moment in time (investigations/, bugs/, decisions/).
