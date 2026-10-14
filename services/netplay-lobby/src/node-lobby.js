// Standalone Node.js netplay lobby — same wire protocol as
// services/netplay-lobby/src/index.js (protocols v1 + v2 +
// send-only uplinks + spectator slots), for local testing without
// wrangler. Run: node services/netplay-lobby/src/node-lobby.js [port]
//
// Mirrors the Worker's hardening: per-connection uplink keys, star-topology
// enforcement, relay-gated binary routing, and per-socket/per-room rate
// limits, so local tests exercise the same rules production enforces.
//
// `ws` resolves from services/netplay-lobby/node_modules (npm install
// there once — it's the same package.json the worker uses).

const http = require("http");
const crypto = require("crypto");
const { WebSocketServer } = require("ws");

const CODE_RE = /^[A-HJ-NP-Z2-9]{6}$/;
const MAX_TEXT_BYTES = 2048;
const MAX_BINARY_BYTES = 1024;
const MAX_PLAYERS_V2 = 4;
const MAX_SPECS = 4;            /* spectator slots 4..7: receive-only, always relayed */
const RATE_WINDOW_MS = 1000;
const MAX_TEXT_PER_SEC = 20;
const MAX_BINARY_PER_SEC = 600;   // ~10x the 60 fps input packet rate
const MAX_ENDPOINT_UPDATES = 10;  // per connection lifetime
const JOIN_WINDOW_MS = 10000;
// Headroom over a full legitimate room (4 control + 4 uplinks + 4 specs =
// 12 joins): rejected attempts also count (AUD-24).
const MAX_JOINS_PER_WINDOW = 24;
const MAX_JOINS_PER_IP_WINDOW = 30;
const SOCKET_IDLE_MS = 90000;
const IDLE_SWEEP_MS = 30000;

const rooms = new Map();   // code -> Room
const ipJoins = new Map(); // ip -> timestamps[] — cross-code scan throttle (AUD-25)

function allowIpJoin(ip) {
  const now = Date.now();
  const ts = (ipJoins.get(ip) || []).filter((t) => now - t < JOIN_WINDOW_MS);
  ts.push(now);
  ipJoins.set(ip, ts);
  return ts.length <= MAX_JOINS_PER_IP_WINDOW;
}

function newUpkey() {
  return crypto.randomBytes(16).toString("hex");
}

class Room {
  constructor(code) {
    this.code = code;
    this.sockets = new Map();  // ws -> { kind, version, slot, endpoint, relay, upkey, owner, rate, endpoints }
    this.version = 0;
    this.relayV1 = false;
    this.recentJoins = [];
  }

  info(ws) { return this.sockets.get(ws); }

  socketV1(role) {
    for (const [ws, i] of this.sockets) {
      if (i.version === 1 && i.kind === role) return ws;
    }
    return null;
  }

  socketForSlot(slot) {
    for (const [ws, i] of this.sockets) {
      if (i.kind !== "up" && i.version === 2 && i.slot === slot) return ws;
    }
    return null;
  }

  freeSlot() {
    for (let s = 1; s < MAX_PLAYERS_V2; s++) {
      if (!this.socketForSlot(s)) return s;
    }
    return -1;
  }

  sendJson(ws, obj) {
    try { ws.send(JSON.stringify(obj)); } catch { /* gone */ }
  }

  allowJoin() {
    const now = Date.now();
    this.recentJoins = this.recentJoins.filter((t) => now - t < JOIN_WINDOW_MS);
    this.recentJoins.push(now);
    return this.recentJoins.length <= MAX_JOINS_PER_WINDOW;
  }

  /* Per-socket token window; over the limit the socket is dropped. */
  allowMessage(info, binary) {
    const now = Date.now();
    const r = info.rate;
    if (now - r.win >= RATE_WINDOW_MS) {
      r.win = now;
      r.text = 0;
      r.bin = 0;
    }
    return binary ? ++r.bin <= MAX_BINARY_PER_SEC : ++r.text <= MAX_TEXT_PER_SEC;
  }

  broadcastRoster(exclude) {
    /* Spectators get the roster with endpoints redacted — they are
     * relay-only and don't need player IPs (AUD-46). */
    const full = [];
    const redacted = [];
    for (const [ws, i] of this.sockets) {
      if (ws === exclude || i.kind === "up") continue;
      full.push({
        slot: i.slot,
        role: i.kind,
        public: i.endpoint?.public || "-",
        local: i.endpoint?.local || "-",
      });
      redacted.push({ slot: i.slot, role: i.kind, public: "-", local: "-" });
    }
    const fullText = JSON.stringify({ type: "peers", players: full });
    const redactedText = JSON.stringify({ type: "peers", players: redacted });
    for (const [ws, i] of this.sockets) {
      if (ws === exclude || i.kind === "up") continue;
      try { ws.send(i.kind === "spec" ? redactedText : fullText); } catch { /* gone */ }
    }
  }

  /* Idle reaper (AUD-23): a dead TCP peer never fires "close" — sweep
   * silent sockets so zombies stop holding slots. Uplinks inherit their
   * owner's freshness (they are legitimately send-only). */
  sweepIdle() {
    const now = Date.now();
    for (const [ws, i] of this.sockets) {
      const seen = (i.kind === "up" && i.owner)
        ? (this.info(i.owner)?.lastSeen ?? i.lastSeen)
        : i.lastSeen;
      if (now - seen > SOCKET_IDLE_MS) {
        try { ws.close(4004, "idle-timeout"); } catch { /* gone */ }
        /* onClose does the real bookkeeping (delete + kill owned uplinks +
         * peer_left + host-gone teardown + gc). */
        this.onClose(ws);
      }
    }
  }

  onMessage(ws, message, isBinary) {
    const info = this.info(ws);
    if (!info) return;
    info.lastSeen = Date.now();

    if (isBinary) {
      // Count first so oversized-frame floods still spend rate budget.
      if (!this.allowMessage(info, true)) {
        try { ws.close(4029, "rate-limit"); } catch { /* gone */ }
        return;
      }
      if (message.length > MAX_BINARY_BYTES) return;
      // Uplinks speak with their owner control socket's identity.
      const src = (info.kind === "up" && info.owner) ? this.info(info.owner) : info;
      if (!src) return;
      if (info.version === 2) {
        // Routed: [to_slot][payload] -> [from_slot][payload]. The star is
        // enforced: clients only reach slot 0, the host only client slots,
        // and the hop must be a link the room marked as relayed.
        const bytes = message;
        if (bytes.length < 2) return;
        const toSlot = bytes[0];
        // Spectators may only address the host (lobby signaling keeps the
        // link alive); game packets from a spec die at the host's engine
        // anyway - no remote owns its slot - but routing only to slot 0
        // keeps them from reaching players at all. The payload itself must
        // be an "SSBL " text datagram: the host feeds relayed payloads
        // straight into its lobby parser, so raw binary could trip fatal
        // version/compat checks and kill the room.
        if (info.kind === "spec") {
          if (toSlot !== 0) return;
          if (bytes.length < 6 || bytes[1] !== 0x53 /* S */ ||
              bytes[2] !== 0x53 || bytes[3] !== 0x42 /* B */ ||
              bytes[4] !== 0x4c /* L */ || bytes[5] !== 0x20 /* space */) return;
        }
        if (toSlot === src.slot || (src.slot !== 0 && toSlot !== 0)) return;
        if (src.slot !== 0 && !src.relay) return;
        const target = this.socketForSlot(toSlot);
        if (!target || target === ws || target === info.owner) return;
        if (src.slot === 0) {
          const ti = this.info(target);
          if (!ti || !ti.relay) return;
        }
        const framed = Buffer.concat([Buffer.from([src.slot]), bytes.subarray(1)]);
        try { target.send(framed); } catch { /* gone */ }
        return;
      }
      if (this.relayV1) {
        const other = this.otherV1(ws);
        if (other) { try { other.send(message); } catch { /* gone */ } }
      }
      return;
    }
    if (!this.allowMessage(info, false)) {
      try { ws.close(4029, "rate-limit"); } catch { /* gone */ }
      return;
    }
    // Counted above even when oversized: >cap floods still spend budget.
    if (message.length > MAX_TEXT_BYTES) return;

    // Uplink sockets are send-only: their text messages (endpoint, relay,
    // ping) apply to the owner socket the uplink was authenticated against.
    if (info.kind === "up") {
      if (!info.owner || !this.info(info.owner)) return;
      ws = info.owner;
    }
    const wi = this.info(ws);
    if (!wi || wi.kind === "up") return;

    let msg;
    try { msg = JSON.parse(message.toString()); } catch { return; }
    switch (msg.type) {
      case "ping":
      case "keepalive":
        this.sendJson(ws, { type: "keepalive_ack" });
        break;
      case "endpoint":
        if (++wi.endpoints > MAX_ENDPOINT_UPDATES) break;
        wi.endpoint = {
          public: String(msg.public || "-").slice(0, 64),
          local: String(msg.local || "-").slice(0, 64),
        };
        if (wi.version === 2) this.broadcastRoster(); else this.introduceV1();
        break;
      case "relay":
        if (wi.version === 2) {
          wi.relay = true;
          this.sendJson(ws, { type: "relaying" });
          const host = this.socketForSlot(0);
          if (host && host !== ws) this.sendJson(host, { type: "peer_relay", slot: wi.slot });
        } else {
          this.relayV1 = true;
          const text = JSON.stringify({ type: "relaying" });
          for (const [s] of this.sockets) {
            try { s.send(text); } catch { /* gone */ }
          }
        }
        break;
    }
  }

  otherV1(ws) {
    const i = this.info(ws);
    if (!i || i.kind === "up") return null;
    return this.socketV1(i.kind === "host" ? "join" : "host");
  }

  introduceV1() {
    const host = this.socketV1("host");
    const join = this.socketV1("join");
    if (!host || !join) return;
    const he = this.info(host)?.endpoint;
    const je = this.info(join)?.endpoint;
    if (!he || !je) return;
    this.sendJson(host, { type: "peer", role: "join", public: je.public, local: je.local });
    this.sendJson(join, { type: "peer", role: "host", public: he.public, local: he.local });
  }

  onClose(ws) {
    const info = this.info(ws);
    if (!info) return;
    const otherV1 = this.otherV1(ws);
    this.sockets.delete(ws);
    try { ws.close(1000, "closed"); } catch { /* already closed */ }

    if (info.kind === "up") {
      // Uplinks carry no roster state; never let one end the room. (A stale
      // uplink can still be the last socket standing — GC anyway.)
      this.gc();
      return;
    }
    // A control socket's uplinks die with it.
    for (const [s, si] of this.sockets) {
      if (si.kind === "up" && si.owner === ws) {
        this.sockets.delete(s);
        try { s.close(1000, "owner-gone"); } catch { /* gone */ }
      }
    }
    if (info.version === 2) {
      if (info.slot === 0) {
        for (const [other, oi] of this.sockets) {
          if (oi.kind !== "up") this.sendJson(other, { type: "peer_left", slot: 0 });
          try { other.close(4001, "host-left"); } catch { /* gone */ }
        }
        this.sockets.clear();
        this.version = 0;
      } else {
        for (const [other, oi] of this.sockets) {
          if (oi.kind !== "up") this.sendJson(other, { type: "peer_left", slot: info.slot });
        }
        this.broadcastRoster();
      }
      this.gc();
      return;
    }
    if (otherV1) {
      this.sendJson(otherV1, { type: "peer_left" });
      if (info.kind === "host") {
        try { otherV1.close(4001, "host-left"); } catch { /* gone */ }
      }
    }
    if (this.sockets.size === 0) {
      this.version = 0;
      this.relayV1 = false;
    }
    this.gc();
  }

  /* Empty rooms are worthless state — remove them so failed joins and
   * closed sessions don't accumulate Room objects. Called only from close
   * handlers, after all socket bookkeeping for the room is done. */
  gc() {
    if (this.sockets.size === 0) rooms.delete(this.code);
  }
}

const port = parseInt(process.argv[2] || "8799", 10);
const server = http.createServer((req, res) => {
  if (req.url === "/health") {
    res.writeHead(200, { "content-type": "application/json" });
    res.end(JSON.stringify({ ok: true, service: "smash-netplay" }));
    return;
  }
  res.writeHead(404);
  res.end("not found");
});
const wss = new WebSocketServer({ noServer: true });
server.on("upgrade", (req, socket, head) => {
  wss.handleUpgrade(req, socket, head, (ws) => wss.emit("connection", ws, req));
});
server.listen(port);

wss.on("connection", (ws, req) => {
  const url = new URL(req.url, "http://localhost");
  if (url.pathname !== "/ws") { ws.close(); return; }
  const room = (url.searchParams.get("room") || "").toUpperCase();
  const role = url.searchParams.get("role");
  const version = url.searchParams.get("v") === "2" ? 2 : 1;
  if (!CODE_RE.test(room) || (role !== "host" && role !== "join" && role !== "spec")) {
    ws.send(JSON.stringify({ type: "error", reason: "bad room or role" }));
    ws.close(4000, "bad room or role");
    return;
  }
  // Cross-code scan throttle (AUD-25): the no-such-room / room-full
  // distinction is a room-existence oracle — bound attempts per client IP.
  if (!allowIpJoin(req.socket.remoteAddress || "unknown")) {
    ws.send(JSON.stringify({ type: "error", reason: "rate-limited" }));
    ws.close(4029, "rate-limited");
    return;
  }
  let r = rooms.get(room);
  // Only a host mints a room: joins and uplinks to a nonexistent room fail
  // fast instead of accumulating empty Room objects in the map.
  if (!r && (role !== "host" || url.searchParams.get("up") === "1")) {
    ws.send(JSON.stringify({ type: "error", reason: "no-such-room" }));
    ws.close(4000, "no-such-room");
    return;
  }
  if (!r) {
    r = new Room(room);
    rooms.set(room, r);
  }
  // Rejection after (possibly) minting the room: if no socket ever
  // attached, the empty Room must not linger in the map.
  const fail = (code, reason) => {
    ws.send(JSON.stringify({ type: "error", reason }));
    ws.close(code, reason);
    r.gc();
  };
  if (!r.allowJoin()) {
    fail(4029, "rate-limited");
    return;
  }

  // Send-only uplink: &key= must match the upkey issued in the owner's
  // welcome — slot ownership is authenticated, not URL-derived.
  if (url.searchParams.get("up") === "1") {
    const slot = parseInt(url.searchParams.get("slot") || "-1", 10);
    const key = url.searchParams.get("key") || "";
    const owner = (version === 2 && r.version === 2 && slot >= 0 &&
                   slot < MAX_PLAYERS_V2) ? r.socketForSlot(slot) : null;
    const oi = owner ? r.info(owner) : null;
    if (!oi || !oi.upkey || key !== oi.upkey) {
      /* Failed auth doesn't spend the room's join budget (AUD-24). */
      r.recentJoins.pop();
      fail(4000, "no-such-room");
      return;
    }
    // One uplink per slot: a reconnect replaces the stale socket.
    for (const [s, i] of r.sockets) {
      if (i.kind === "up" && i.slot === slot) {
        r.sockets.delete(s);
        try { s.close(4003, "uplink-replaced"); } catch { /* gone */ }
      }
    }
    attach(r, ws, { kind: "up", version: 2, slot, endpoint: null, relay: false, owner });
    return;
  }

  // Spectator (v2): slots 4..7, no endpoints, always relayed, send-never —
  // the host pushes every player's confirmed input stream to them.
  if (role === "spec") {
    if (version !== 2) {
      fail(4000, "version-mismatch");
      return;
    }
    if (!r.socketForSlot(0)) {
      fail(4000, "no-such-room");
      return;
    }
    let slot = -1;
    for (let s = MAX_PLAYERS_V2; s < MAX_PLAYERS_V2 + MAX_SPECS; s++) {
      if (!r.socketForSlot(s)) { slot = s; break; }
    }
    if (slot < 0) {
      fail(4000, "room-full");
      return;
    }
    attach(r, ws, { kind: "spec", version: 2, slot, endpoint: null, relay: true });
    ws.send(JSON.stringify({ type: "welcome", room, role, slot }));
    r.broadcastRoster();
    return;
  }

  if (role === "host") {
    const oldHost = version === 2 ? r.socketForSlot(0) : r.socketV1("host");
    if (oldHost) {
      fail(4000, "room-in-use");
      return;
    }
    // Only reset a room that is empty or already on this protocol version
    // (hostless): a cross-version join on a live room must not evict it.
    if (r.sockets.size > 0) {
      if (r.version !== version) {
        fail(4000, "version-mismatch");
        return;
      }
      for (const [s] of r.sockets) {
        try { s.close(4002, "room-reset"); } catch { /* gone */ }
      }
      r.sockets.clear();
    }
    r.version = version;
    r.relayV1 = false;
  } else {
    if (r.version !== version) {
      fail(4000, "version-mismatch");
      return;
    }
    const host = version === 2 ? r.socketForSlot(0) : r.socketV1("host");
    if (!host) {
      fail(4000, "no-such-room");
      return;
    }
    if (version === 1 && r.socketV1("join")) {
      fail(4000, "room-full");
      return;
    }
  }

  if (version === 1) {
    attach(r, ws, { kind: role, version: 1, slot: role === "host" ? 0 : 1, endpoint: null, relay: false });
    ws.send(JSON.stringify({ type: "welcome", room, role }));
    r.introduceV1();
    return;
  }

  // v2: the slot assignment and upkey are issued with the welcome.
  const slot = role === "host" ? 0 : r.freeSlot();
  if (role !== "host" && slot < 0) {
    fail(4000, "room-full");
    return;
  }
  const upkey = newUpkey();
  attach(r, ws, { kind: role, version: 2, slot, endpoint: null, relay: false, upkey });
  ws.send(JSON.stringify({ type: "welcome", room, role, slot, upkey }));
  r.broadcastRoster();
});

function attach(room, ws, info) {
  info.rate = { win: 0, text: 0, bin: 0 };
  info.endpoints = 0;
  info.lastSeen = Date.now();
  room.sockets.set(ws, info);
  ws.on("message", (d, b) => room.onMessage(ws, d, b));
  ws.on("close", () => room.onClose(ws));
  ws.on("error", () => room.onClose(ws));
}

// Idle sweep across all rooms (client keepalive cadence is 20 s; 90 s of
// silence means a dead peer that will never emit "close").
setInterval(() => {
  for (const r of rooms.values()) r.sweepIdle();
}, IDLE_SWEEP_MS).unref();

console.log(`node-lobby ws listening on :${port}`);
