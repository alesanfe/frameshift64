# Netplay lobby protocol (contract summary)

Canonical implementations: `services/netplay-lobby/src/infrastructure/cloudflare/worker.js`
(Cloudflare DO), `services/netplay-lobby/src/infrastructure/local/node-lobby.mjs` (local
mirror), `src/netplay/session/netlobby.cpp` (client).

Invariants every implementation must keep:

- `role=spec` admitted at the edge; spec receives a redacted roster
  (no player endpoint details).
- Cross-version `reset` rejected unless the room is empty.
- Spec→host control frames must carry the `SSBL ` magic payload.
- Oversize frames count against rate limits *before* being dropped.
- PICK token format: `<build_sha>:<mod_content_hash>:<asset_archive_hash>`;
  cosmetic mods (`"simulation": false`) are excluded from the hash.
- `SSB64_LAN_KEY` authenticates LAN HELLO adoption; unauthenticated
  LAN adopts are rate-limited.

Narrative protocol docs: `docs/netplay/protocol/`.
