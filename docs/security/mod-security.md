# Mod security

Mods are **trusted native code** — TinyCC-compiled C loaded into the
process with access to the game's symbol table and funchook detours.
This is a power-user feature boundary, not a sandbox.

## What the runtime guarantees

- A mod is only loaded if it carries a parseable `manifest.json`
  (`ModRegistry`); archives without manifests (e.g. texture packs) are
  excluded automatically.
- Compile failures, symbol-resolution failures, and manifest errors
  become recorded load states — a bad mod cannot crash the host at load.
- Historical AV false-positive pressure on TinyCC is addressed by
  static-linking libtcc and shipping only headers + `libtcc1.a` +
  `BattleShip.def` (see `docs/bugs/` → `tinycc_av_flagging`).

## What is explicitly NOT guaranteed

- Memory safety or behavioral correctness of mod code.
- Protection against a malicious mod (it runs with full process rights).

## Netplay boundary

Mods that touch simulation are a compatibility surface, not just a
stability one. The manifest will declare
`simulation: local_only | synchronized | ruleset_controlled |
netplay_unsafe` and the session manifest exchanges the effective set
before a match (TD-007/TD-008). Until then, the user-facing rule is:
**no gameplay mods in netplay**.

## Distribution

Mods are loaded only from the user's own `mods/` directory. The game
never downloads or auto-installs mods.
