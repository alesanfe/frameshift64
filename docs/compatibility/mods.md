# Mod compatibility

## Contract

A mod is a directory or archive under `mods/` carrying a `manifest.json`
with `name`, `version`, `author`, `description` (see
`src/bridge/modding/ModRegistry.h`). Mods hook engine/game events through
the symbol table + `HookManager` (`src/bridge/modding/`,
`src/bridge/hooks/list/`).

## Rules

- `SMASH_MOD_API_VERSION` is the contract version. When manifests gain an
  `apiVersion` field, `ModRegistry` must refuse mods requesting a newer
  API than the runtime implements, with a load-state entry explaining why
  (extends `ModLoadState`).
- **Simulation-affecting mods must self-declare**: a manifest field
  (`simulation: local_only | synchronized | ruleset_controlled |
  netplay_unsafe`) is required for any mod that registers gameplay hooks.
  Netplay session startup compares the effective set between peers.
- Texture-only / cosmetic packs must never be able to alter simulation
  state; that is enforced by not exposing gameplay hooks to them.
- A mod must not be able to crash the host on load: manifest parse,
  symbol resolution, and compilation errors are all captured as load
  states, not fatal exits.

## Today

`ModRegistry` records Loaded/InvalidManifest/etc. per mod; the
`simulation` manifest field and `apiVersion` check are pending (TD-007).
Examples live in `mods/examples/`, templates in `mods/templates/`.
