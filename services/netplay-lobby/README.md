# netplay-lobby

The SSB64 PC-port lobby/relay service — deployable independently of the game binary.

## Layout

| Entry | Purpose |
|-------|---------|
| `src/domain/` | Shared lobby logic both adapters run — `protocol.mjs` (frame schema, session ids, upkey auth, rate limits, close codes) and `room.mjs` (slots, star routing, spectator state). Edit room behavior here only: a change lands in both deployables by construction. |
| `src/index.js` | Cloudflare adapter — Worker + Durable Object (`NetplayRoom`) driving `domain/` under the Workers runtime (ws hibernation API). Deploy with `npx wrangler deploy`. |
| `src/node-lobby.mjs` | Node adapter — same wire protocol for `wrangler dev`-free local testing (`node src/node-lobby.mjs 8799`). This is the process every test and netplay script launches. |
| `tests/` | `test-lobby-security.js` (session ids, upkey auth, rate limits, star-routing drops) and `test-lobby-fuzz.js` (malformed-frame fuzz driver). Both run against `node-lobby.mjs`. |
| `package.json` / `package-lock.json` | Shared deps (`wrangler`, `ws`). `npm ci` at this directory, not per-subdir. |
| `wrangler.jsonc` | Worker config; `main: src/index.js`. |

Local test scripts: `scripts/testing/netplay_lobbytest.ps1`, `netplay_lobbytest_multi.ps1`, `spectate_test.ps1` (auto-start `wrangler dev` or the node lobby on `ws://127.0.0.1:8799`).

Design docs: `docs/netplay/`. Dated audits: `docs/investigations/netplay/`.
