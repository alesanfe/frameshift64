# Mod compatibility

## Contract

A mod is a directory or archive under `mods/` carrying a `manifest.json`
with `name`, `version`, `author`, `description` (see
`src/game/modding/ModRegistry.h`). Mods hook engine/game events through
the symbol table + `HookManager` (`src/game/modding/`,
`src/game/engine/hooks/list/`).

## Manifest fields

Required:

- `name` — display name.
- `version` — human-readable version.
- `main` — entry-point script path (for TCC mods) or `build.gen` list.
- `code-version` — API version the compiled scripts target; mismatch is a
  load failure.

Optional but load-bearing:

- `simulation` (`bool`, default `true`) — `false` declares a cosmetic-only
  mod. Cosmetic mods are excluded from the netplay compatibility hash; a
  mod that lies and still shadows gameplay data will desync peers.
- `game-version` — numeric game version the archive targets.
- `dependencies` — array of mod names that must load first.

Optional trust fields:

- `checksum` (`"fnv1a64:<16-hex>"`) — FNV-1a 64 over every file except
  `manifest.json`, sorted by name, including each file's name, size, and
  bytes. Recomputed at load; mismatch marks the mod as tampered.
- `signature` + `public-key` — minisign raw/legacy (`-l`) Ed25519 signature
  over the `checksum` string. The in-game Mods menu shows a green
  `[signed]` badge when both verify and a red `[signature invalid]` badge
  otherwise. Unsigned mods remain allowed; the fields exist so authors can
  prove authenticity and a future pinning policy can enforce them.

## Rules

- `code-version` is enforced: `ModRegistry` records an `InvalidManifest`
  load state when the manifest's `code-version` exceeds the runtime's
  supported version.
- **Simulation-affecting mods must self-declare**: `simulation: false` is
  required for cosmetic-only mods. Netplay session startup includes the
  effective set of simulation-affecting mods in the compatibility token.
- Texture-only / cosmetic packs must never be able to alter simulation
  state; that is enforced by not exposing gameplay hooks to them.
- A mod must not be able to crash the host on load: manifest parse,
  symbol resolution, and compilation errors are all captured as load
  states, not fatal exits.

## Tooling

- `tools/developer/modding/package_mod.py` packages a source mod into the runtime
  layout. Pass `--checksum` to inject an FNV-1a 64 checksum, and
  `--sign-key <minisign-secret.key>` to add a minisign `-l` signature
  plus the matching public key.
- Examples live in `mods/examples/`, templates in `mods/templates/`.
