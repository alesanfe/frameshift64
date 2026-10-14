# ADR 0004 — Netplay is a first-class module

**Status:** accepted (2026-10)

Client netplay code lives in `src/netplay/` (was `src/netplay/`): lobby client (`netlobby.*`), ruleset sync (`netrules.*`), UDP transport (`udp_transport.*`), WebSocket transport (`websocket_transport.*`), rollback segment config (`netrollback_seg.h`).

The lobby **service** lives in `services/netplay-lobby/` (was `server/`): `cloudflare/` is the deployable Durable-Object worker, `node-lobby.js` the local-dev stand-in, `test-lobby-security.js` its security test.

Dependency direction: `presentation → netplay → bridge → platform`. The protocol layer must not know about the GUI or Cloudflare specifics; transport code must be testable without opening real sockets (see `SSB64_NETPLAY_SIM_*` env vars for the seeded loss/dup/reorder/corrupt simulator in `udp_transport.cpp`).

Any feature that alters simulation state must declare its netplay class: `LOCAL_ONLY`, `SYNCHRONIZED`, `RULESET_CONTROLLED`, or `NETPLAY_UNSAFE` (enforced via `netrules` at handshake).
