// Lobby protocol domain — shared constants and pure helpers.
//
// Consumed by BOTH lobby adapters so they can't drift:
//   src/index.js          — Cloudflare Worker + Durable Object
//   src/node-lobby.mjs    — local mirror used by tests and LAN rigs
//
// Everything in this file is transport-free: no WebSocketPair, no `ws`
// package, no Durable Object APIs. Node >= 19 provides the same global
// `crypto` the Workers runtime does.

export const CODE_RE = /^[A-HJ-NP-Z2-9]{6}$/;
export const MAX_TEXT_BYTES = 2048;
export const MAX_BINARY_BYTES = 1024;
export const MAX_PLAYERS_V2 = 4;
export const MAX_SPECS = 4;            /* spectator slots 4..7: receive-only, always relayed */
export const RATE_WINDOW_MS = 1000;
export const MAX_TEXT_PER_SEC = 20;
export const MAX_BINARY_PER_SEC = 600;   // ~10x the 60 fps input packet rate
export const MAX_ENDPOINT_UPDATES = 10;  // per connection lifetime
export const JOIN_WINDOW_MS = 10000;
export const MAX_JOINS_PER_WINDOW = 24;
export const MAX_JOINS_PER_IP_WINDOW = 30;
export const SOCKET_IDLE_MS = 90000;
export const IDLE_SWEEP_MS = 30000;

// Spec→host control datagrams must carry this magic: the host feeds
// relayed payloads straight into its lobby parser, and raw binary could
// trip fatal version/compat checks and kill the room.
export const SPEC_MAGIC = [0x53, 0x53, 0x42, 0x4c, 0x20];  // "SSBL "

export const VALID_ROLES = new Set(["host", "join", "spec"]);

/* Per-connection uplink key (v2): the uplink upgrade must prove it knows
 * this secret, so slot ownership comes from the welcome, not the URL. */
export function newUpkey() {
  const bytes = new Uint8Array(16);
  crypto.getRandomValues(bytes);
  return Array.from(bytes, (b) => b.toString(16).padStart(2, "0")).join("");
}

/* Sliding-window limiter factory. On Workers this is per-isolate
 * (approximate — isolates aren't shared, and that's fine: it bounds
 * scanning cost, it isn't a security boundary); on Node it's process-wide. */
export function makeJoinLimiter() {
  const hits = new Map();  // key -> timestamps[]
  return function allow(key) {
    const now = Date.now();
    const ts = (hits.get(key) || []).filter((t) => now - t < JOIN_WINDOW_MS);
    ts.push(now);
    hits.set(key, ts);
    return ts.length <= MAX_JOINS_PER_IP_WINDOW;
  };
}
