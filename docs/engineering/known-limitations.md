# Known limitations

- **No cross-region netplay.** US and JP builds use different ROM assets
  and different compiled decomp trees; cross-region sessions are
  unsupported by design.
- **Mods are trusted code.** No sandbox; a malicious mod has full process
  rights (`docs/security/mod-security.md`).
- **`wrangler dev` drops binary frames** under relay load — use
  `services/netplay-lobby/src/node-lobby.mjs` for local protocol testing
  (`docs/bugs/` → `netplay_relay_wrangler_dev_drop`).
- **Room codes are bearer tokens.** Anyone with the 6-char code can
  attempt joins at the bounded rate; the upkey prevents slot theft but
  not presence.
- **No save migration path yet** (TD-004) — a format change in
  `port_save.cpp` invalidates existing saves.
- **Detection, not prevention, for desync cheating** — a peer can send
  crafted inputs; divergent confirmed hashes catch it after the fact.
- **Debug builds are slower than Release** enough to matter for rollback
  distance; determinism is verified cross-build but netplay performance
  targets assume Release.
