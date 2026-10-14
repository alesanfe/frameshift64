# netplay-lobby

The SSB64 PC-port lobby/relay service — deployable independently of the game binary.

## Layout

| Entry | Purpose |
|-------|---------|
| `src/index.js` | Cloudflare Worker + Durable Object (`NetplayRoom`): WebSocket sessions, slot assignment, `upkey` uplink auth, rate limiting, star routing (v1 1v1 + v2 up to 4 players). Deploy with `npx wrangler deploy`. |
| `src/node-lobby.js` | Local-dev stand-in implementing the same wire protocol for `wrangler dev`-free testing (`node src/node-lobby.js 8799`). |
| `tests/` | `test-lobby-security.js` (session ids, upkey auth, rate limits, star-routing drops) and `test-lobby-fuzz.js` (malformed-frame fuzz driver). Both run against `node-lobby.js`. |
| `package.json` / `package-lock.json` | Shared deps (`wrangler`, `ws`). `npm ci` at this directory, not per-subdir. |
| `wrangler.jsonc` | Worker config; `main: src/index.js`. |

Local test scripts: `scripts/testing/netplay_lobbytest.ps1`, `netplay_lobbytest_multi.ps1`, `spectate_test.ps1` (auto-start `wrangler dev` or the node lobby on `ws://127.0.0.1:8799`).

Design docs: `docs/netplay/`. Dated audits: `docs/investigations/netplay/`.
