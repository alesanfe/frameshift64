# Asset trust

## ROM handling

- The build never ships ROM-derived data. `baserom.*.z64` is user-supplied
  and gitignored; extraction runs locally (desktop Torch or on-device
  `libtorch_runner`).
- The ROM's SHA-1 must match an entry in `config.yml` — unsupported dumps
  are rejected with an explicit error (`docs/bugs/` →
  `android_torch_silent_failure` hardened the error path).
- Only the user's own extracted `BattleShip.o2r` is ever loaded.

## Archive integrity

- Build↔runtime consistency is guarded by the baked recipe hash + the
  `.recipe` sidecar (auto re-extract on mismatch). This detects *stale*
  archives; it is not a signature — an attacker who can write the user's
  game directory can replace both. That is accepted: the threat model does
  not protect against local write access.
- Downloaded shader/asset packs (ShaderDownloader) are treated as
  untrusted content: parsed, never executed.

## Logs

Extraction and runtime logs must not include the ROM path on shared
systems beyond necessity, and diagnostics bundles must never include the
ROM, extracted assets, keys, or chat content (see
`incident-response.md` and the diagnostics spec in
`docs/testing/strategy.md`).
