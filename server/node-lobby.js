// Standalone Node.js netplay lobby — same wire protocol as
// server/cloudflare/src/index.js (protocols v1 + v2 + send-only uplinks),
// for local testing without wrangler. Run: node server/node-lobby.js [port]
//
// The ws package resolves from server/cloudflare/node_modules.

const path = require("path");
const http = require("http");
const { WebSocketServer } = require(path.join(__dirname, "cloudflare", "node_modules", "ws"));

const CODE_RE = /^[A-HJ-NP-Z2-9]{6}$/;
const MAX_TEXT_BYTES = 2048;
const MAX_BINARY_BYTES = 1024;
const MAX_PLAYERS_V2 = 4;

const rooms = new Map();   // code -> Room

class Room {
  constructor() {
    this.sockets = new Map();  // ws -> { kind, version, slot, endpoint, relay }
    this.version = 0;
    this.relayV1 = false;
    this.bin = 0;
    this.counts = {};
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
      if (info.version === 2) {
        const bytes = message;
        if (bytes.length < 2) return;
        this.bin++;
        const key = info.kind + ":" + info.slot;
        this.counts[key] = (this.counts[key] || 0) + 1;
        const target = this.socketForSlot(bytes[0]);
        if (!target) return;
        const framed = Buffer.concat([Buffer.from([info.slot]), bytes.subarray(1)]);
        try { target.send(framed); } catch { /* gone */ }
        if (this.bin % 1000 === 0) {
          console.log(`room bin#${this.bin} counts=${JSON.stringify(this.counts)}`);
        }
        return;
      }
      if (this.relayV1) {
        const other = this.otherV1(ws);
        if (other) { try { other.send(message); } catch { /* gone */ } }
      }
      return;
    }
    if (message.length > MAX_TEXT_BYTES) return;

    if (info.kind === "up") {
      const owner = this.socketForSlot(info.slot);
      if (!owner) return;
      ws = owner;
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

    if (info.kind === "up") return;
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
    ws.close(4000, "bad room or role");
    return;
  }
  let r = rooms.get(room);
  if (!r) {
    r = new Room();
    rooms.set(room, r);
  }

  if (url.searchParams.get("up") === "1") {
    const slot = parseInt(url.searchParams.get("slot") || "-1", 10);
    if (version !== 2 || r.version !== 2 || slot < 0 ||
        slot >= MAX_PLAYERS_V2 || !r.socketForSlot(slot)) {
      ws.send(JSON.stringify({ type: "error", reason: "no-such-room" }));
      ws.close(4000, "no-such-room");
      return;
    }
    r.sockets.set(ws, { kind: "up", version: 2, slot, endpoint: null, relay: false });
    ws.on("message", (d, b) => r.onMessage(ws, d, b));
    ws.on("close", () => r.onClose(ws));
    ws.on("error", () => r.onClose(ws));
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
    r.sockets.set(ws, { kind: role, version: 1, slot: role === "host" ? 0 : 1, endpoint: null, relay: false });
    ws.on("message", (d, b) => r.onMessage(ws, d, b));
    ws.on("close", () => r.onClose(ws));
    ws.on("error", () => r.onClose(ws));
    ws.send(JSON.stringify({ type: "welcome", room, role }));
    r.introduceV1();
    return;
  }

  const slot = role === "host" ? 0 : r.freeSlot();
  if (role !== "host" && slot < 0) {
    ws.send(JSON.stringify({ type: "error", reason: "room-full" }));
    ws.close(4000, "room-full");
    return;
  }
  r.sockets.set(ws, { kind: role, version: 2, slot, endpoint: null, relay: false });
  ws.on("message", (d, b) => r.onMessage(ws, d, b));
  ws.on("close", () => r.onClose(ws));
  ws.on("error", () => r.onClose(ws));
  ws.send(JSON.stringify({ type: "welcome", room, role, slot }));
  r.broadcastRoster();
});

console.log(`node-lobby ws listening on :${port}`);
