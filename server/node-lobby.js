// Standalone Node.js netplay lobby — same wire protocol as
// server/cloudflare/src/index.js (protocols v1 + v2 + send-only uplinks),
// for local testing without wrangler. Run: node server/node-lobby.js [port]
//
// Mirrors the Worker's hardening: per-connection uplink keys, star-topology
// enforcement, relay-gated binary routing, and per-socket/per-room rate
// limits, so local tests exercise the same rules production enforces.
//
// The ws package resolves from server/cloudflare/node_modules.

const path = require("path");
const http = require("http");
const crypto = require("crypto");
const { WebSocketServer } = require(path.join(__dirname, "cloudflare", "node_modules", "ws"));

const CODE_RE = /^[A-HJ-NP-Z2-9]{6}$/;
const MAX_TEXT_BYTES = 2048;
const MAX_BINARY_BYTES = 1024;
const MAX_PLAYERS_V2 = 4;
const RATE_WINDOW_MS = 1000;
const MAX_TEXT_PER_SEC = 20;
const MAX_BINARY_PER_SEC = 600;   // ~10x the 60 fps input packet rate
const MAX_ENDPOINT_UPDATES = 10;  // per connection lifetime
const JOIN_WINDOW_MS = 10000;
const MAX_JOINS_PER_WINDOW = 12;  // control sockets + uplinks combined

const rooms = new Map();   // code -> Room

function newUpkey() {
  return crypto.randomBytes(16).toString("hex");
}

class Room {
  constructor() {
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
    const players = [];
    for (const [ws, i] of this.sockets) {
      if (ws === exclude || i.kind === "up") continue;
      players.push({
        slot: i.slot,
        public: i.endpoint?.public || "-",
        local: i.endpoint?.local || "-",
      });
    }
    const text = JSON.stringify({ type: "peers", players });
    for (const [ws, i] of this.sockets) {
      if (ws === exclude || i.kind === "up") continue;
      try { ws.send(text); } catch { /* gone */ }
    }
  }

  onMessage(ws, message, isBinary) {
    const info = this.info(ws);
    if (!info) return;

    if (isBinary) {
      if (message.length > MAX_BINARY_BYTES) return;
      if (!this.allowMessage(info, true)) {
        try { ws.close(4029, "rate-limit"); } catch { /* gone */ }
        return;
      }
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
    if (message.length > MAX_TEXT_BYTES) return;
    if (!this.allowMessage(info, false)) {
      try { ws.close(4029, "rate-limit"); } catch { /* gone */ }
      return;
    }

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
      // Uplinks carry no roster state; never let one end the room.
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
  if (!CODE_RE.test(room) || (role !== "host" && role !== "join")) {
    ws.send(JSON.stringify({ type: "error", reason: "bad room or role" }));
    ws.close(4000, "bad room or role");
    return;
  }
  let r = rooms.get(room);
  if (!r) {
    r = new Room();
    rooms.set(room, r);
  }
  if (!r.allowJoin()) {
    ws.send(JSON.stringify({ type: "error", reason: "rate-limited" }));
    ws.close(4029, "rate-limited");
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
      ws.send(JSON.stringify({ type: "error", reason: "no-such-room" }));
      ws.close(4000, "no-such-room");
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

  if (role === "host") {
    const oldHost = version === 2 ? r.socketForSlot(0) : r.socketV1("host");
    if (oldHost) {
      ws.send(JSON.stringify({ type: "error", reason: "room-in-use" }));
      ws.close(4000, "room-in-use");
      return;
    }
    for (const [s] of r.sockets) {
      try { s.close(4002, "room-reset"); } catch { /* gone */ }
    }
    r.sockets.clear();
    r.version = version;
    r.relayV1 = false;
  } else {
    if (r.version !== version) {
      ws.send(JSON.stringify({ type: "error", reason: "version-mismatch" }));
      ws.close(4000, "version-mismatch");
      return;
    }
    const host = version === 2 ? r.socketForSlot(0) : r.socketV1("host");
    if (!host) {
      ws.send(JSON.stringify({ type: "error", reason: "no-such-room" }));
      ws.close(4000, "no-such-room");
      return;
    }
    if (version === 1 && r.socketV1("join")) {
      ws.send(JSON.stringify({ type: "error", reason: "room-full" }));
      ws.close(4000, "room-full");
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
    ws.send(JSON.stringify({ type: "error", reason: "room-full" }));
    ws.close(4000, "room-full");
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
  room.sockets.set(ws, info);
  ws.on("message", (d, b) => room.onMessage(ws, d, b));
  ws.on("close", () => room.onClose(ws));
  ws.on("error", () => room.onClose(ws));
}

console.log(`node-lobby ws listening on :${port}`);
