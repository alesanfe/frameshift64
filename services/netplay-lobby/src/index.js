// BattleShip netplay lobby on Cloudflare Workers.
//
// Each room is a Durable Object keyed by its 6-character code. Players
// connect over WebSocket:
//
//   GET /ws?room=CODE&role=host|join[&v=2][&up=1&slot=N&key=K]  (WebSocket upgrade)
//   GET /health
//
// Protocol v1 (1v1, unchanged):
//   C->S {type:"endpoint", public:"ip:port", local:"ip:port"}
//   S->C {type:"welcome", room, role}
//   S->C {type:"peer", role, public, local}   once both endpoints are known
//   C->S {type:"relay"}          direct UDP failed; relay game traffic here
//   S->C {type:"relaying"}       to both; binary frames are now forwarded
//   C->S {type:"keepalive"}  ->  S->C {type:"keepalive_ack"}
//   S->C {type:"peer_left"}
//   S->C {type:"error", reason}  then close 4000
//
// Protocol v2 (up to 4 players, star topology through the host):
//   Slots: host is always slot 0, joiners get the lowest free slot 1..3.
//   S->C {type:"welcome", room, role, slot}
//   S->C {type:"peers", players:[{slot, public, local}]}  full roster,
//        resent whenever a player joins, leaves, or posts its endpoint;
//        public/local are "-" until that player's endpoint arrives
//   C->S {type:"endpoint", public:"ip:port", local:"ip:port"}
//   C->S {type:"relay"}          this client can't reach the host directly;
//        the Worker marks its link as relayed and tells the host
//   S->C {type:"peer_relay", slot}   to the host: that client's link is relay
//   S->C {type:"relaying"}           to the client that asked
//   S->C {type:"peer_left", slot}
//   S->C {type:"error", reason}  then close 4000
//   Binary frames are routed: client sends [to_slot][payload] and the
//   Worker delivers [from_slot][payload] to that socket. The star is
//   enforced here: clients may only address slot 0 and the host may only
//   address client slots; relayed traffic additionally only flows on
//   links the owner marked relayed (client -> host needs the sender's
//   relay flag, host -> client the target's).
//
// A room's protocol version is fixed by the host. A joiner on a different
// version gets {type:"error", reason:"version-mismatch"} so old and new
// builds never silently share a room (the game's own version check would
// also catch it).
//
// &up=1&slot=N&key=K upgrades a send-only uplink for the client that
// already owns room slot N: synchronous WinHTTP websocket sends can
// serialize badly on a socket with a pending receive, so all outbound
// game traffic prefers this second connection. K is the per-connection
// upkey the Worker issued in the owner's welcome, so a room-code guesser
// cannot hijack another player's slot. Uplinks carry no roster state and
// are excluded from close/roster bookkeeping; they die with their owner.
//
// Rate limiting is per socket (text/binary per second) and per room
// (join attempts per 10 s window) — the 6-character room code is the only
// room secret, so guessing and relay abuse need a bounded cost.
//
// The Durable Object uses the classic non-hibernating websocket API
// (ws.accept() + addEventListener): hibernation reloads proved unreliable
// under continuous game traffic in wrangler dev, and the room only needs to
// be alive while players are connected anyway.
//
// Binary frames carry relayed game packets. Game traffic normally goes
// peer-to-peer over UDP and never touches the Worker.

import { DurableObject } from "cloudflare:workers";

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
// 12 joins): rejected attempts also count, so a budget of exactly 12 let a
// code-holder hold the room closed at ~1.2 conn/s (AUD-24).
const MAX_JOINS_PER_WINDOW = 24;
// Cross-code join throttle at the edge: per-DO budgets can't slow a scan
// across room codes, so bound attempts per client IP in the isolate (AUD-25).
const MAX_JOINS_PER_IP_WINDOW = 30;
// Idle reaper: a client sends a keepalive every 20 s, so 90 s of silence
// means a zombie socket holding a slot and the DO resident (AUD-23).
const SOCKET_IDLE_MS = 90000;
const IDLE_SWEEP_MS = 30000;

// Per-isolate IP join accounting (approximate — isolates aren't shared, and
// that's fine: this bounds scanning cost, it isn't a security boundary).
const ipJoins = new Map();  // ip -> timestamps[]

function allowIpJoin(ip) {
  const now = Date.now();
  const ts = (ipJoins.get(ip) || []).filter((t) => now - t < JOIN_WINDOW_MS);
  ts.push(now);
  ipJoins.set(ip, ts);
  return ts.length <= MAX_JOINS_PER_IP_WINDOW;
}

// Per-connection uplink key (v2): the uplink upgrade must prove it knows
// this secret, so slot ownership comes from the welcome, not the URL.
function newUpkey() {
  const bytes = new Uint8Array(16);
  crypto.getRandomValues(bytes);
  return Array.from(bytes, (b) => b.toString(16).padStart(2, "0")).join("");
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);

    if (url.pathname === "/health") {
      return Response.json({ ok: true, service: "smash-netplay" });
    }
    if (url.pathname !== "/ws") {
      return new Response("not found", { status: 404 });
    }
    if (request.headers.get("Upgrade") !== "websocket") {
      return new Response("expected websocket", { status: 426 });
    }
    const room = (url.searchParams.get("room") || "").toUpperCase();
    const role = url.searchParams.get("role");
    if (!CODE_RE.test(room) || (role !== "host" && role !== "join" && role !== "spec")) {
      return new Response("bad room or role", { status: 400 });
    }
    // Cross-code scan throttle: room-existence replies differ
    // ("no-such-room" vs "room-full"), so joining is the only oracle a code
    // guesser has — make it cost connections, not just requests (AUD-25).
    const ip = request.headers.get("cf-connecting-ip") || "unknown";
    if (!allowIpJoin(ip)) {
      return new Response("rate-limited", { status: 429 });
    }
    const stub = env.ROOMS.get(env.ROOMS.idFromName(room));
    return stub.fetch(request);
  },
};

export class NetplayRoom extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    // Non-hibernating sockets: every open ws lives in this.sockets and keeps
    // the DO resident, so in-memory state is authoritative.
    this.sockets = new Map();   // WebSocket -> { kind, version, slot, endpoint, relay, upkey, owner, rate }
    this.version = 0;           // 0 = empty room, else 1|2 fixed by the host
    this.relayV1 = false;       // v1: whole session relayed
    this.recentJoins = [];      // timestamps, bounded join/uplink rate
  }

  /* ---- socket bookkeeping ---- */

  info(ws) {
    return this.sockets.get(ws);
  }

  socketV1(role) {
    for (const [ws, i] of this.sockets) {
      if (i.kind !== "up" && i.version === 1 && i.kind === role) return ws;
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
    for (let slot = 1; slot < MAX_PLAYERS_V2; slot++) {
      if (!this.socketForSlot(slot)) return slot;
    }
    return -1;
  }

  sendJson(ws, obj) {
    try { ws.send(JSON.stringify(obj)); } catch { /* socket already gone */ }
  }

  reject(reason) {
    const pair = new WebSocketPair();
    pair[1].accept();
    pair[1].send(JSON.stringify({ type: "error", reason }));
    pair[1].close(4000, reason);
    return new Response(null, { status: 101, webSocket: pair[0] });
  }

  attach(ws, info) {
    info.rate = { win: 0, text: 0, bin: 0 };
    info.endpoints = 0;
    info.lastSeen = Date.now();
    this.sockets.set(ws, info);
    ws.addEventListener("message", (event) => this.onMessage(ws, event.data));
    ws.addEventListener("close", () => this.onClose(ws));
    ws.addEventListener("error", () => this.onClose(ws));
    this.armSweep();
  }

  /* Idle reaper (AUD-23): arm a DO alarm once sockets exist; the alarm
   * closes silent sockets and re-arms while any remain. A dead TCP peer
   * never fires "close" on its own, so without this a zombie holds its
   * slot (and the DO's billable residency) forever. */
  armSweep() {
    if (this.sweepArmed) return;
    this.sweepArmed = true;
    this.ctx.storage.setAlarm(Date.now() + IDLE_SWEEP_MS)
      .catch(() => { this.sweepArmed = false; });
  }

  async alarm() {
    this.sweepArmed = false;
    this.sweepIdle();
    if (this.sockets.size > 0) this.armSweep();
  }

  sweepIdle() {
    const now = Date.now();
    for (const [ws, i] of this.sockets) {
      // Uplinks are send-only and can sit silent legitimately — their life
      // is the owner's: fresh owner => fresh uplink.
      const seen = (i.kind === "up" && i.owner)
        ? (this.info(i.owner)?.lastSeen ?? i.lastSeen)
        : i.lastSeen;
      if (now - seen > SOCKET_IDLE_MS) {
        try { ws.close(4004, "idle-timeout"); } catch { /* gone */ }
        /* onClose does the real bookkeeping (delete + kill owned uplinks +
         * peer_left + host-gone teardown). Deleting first would make it a
         * no-op and strand the uplinks. */
        this.onClose(ws);
      }
    }
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

  /* ---- connection ---- */

  fetch(request) {
    const url = new URL(request.url);
    const room = url.searchParams.get("room").toUpperCase();
    const role = url.searchParams.get("role");
    const version = url.searchParams.get("v") === "2" ? 2 : 1;

    // Bounded join rate: the room code is the only room secret, so make
    // upgrade attempts (control + uplink) cost real connections.
    const now = Date.now();
    this.recentJoins = this.recentJoins.filter((t) => now - t < JOIN_WINDOW_MS);
    this.recentJoins.push(now);
    if (this.recentJoins.length > MAX_JOINS_PER_WINDOW) {
      return this.reject("rate-limited");
    }

    // Send-only uplink (see file header): tagged {kind:"up"} with the owner's
    // slot, excluded from the roster; inbound text applies to the owner.
    // The &key= must match the upkey the owner got in its welcome — slot
    // ownership is authenticated by the connection, not by the URL.
    if (url.searchParams.get("up") === "1") {
      const slot = parseInt(url.searchParams.get("slot") || "-1", 10);
      const key = url.searchParams.get("key") || "";
      const owner = (version === 2 && this.version === 2 && slot >= 0 &&
                     slot < MAX_PLAYERS_V2) ? this.socketForSlot(slot) : null;
      const oi = owner ? this.info(owner) : null;
      if (!oi || !oi.upkey || key !== oi.upkey) {
        /* Failed auth doesn't spend the room's join budget — an attacker
         * without the upkey shouldn't be able to crowd out the legitimate
         * owner's reconnect (AUD-24). */
        this.recentJoins.pop();
        return this.reject("no-such-room");
      }
      // One uplink per slot: a reconnect replaces the stale socket.
      for (const [ws, i] of this.sockets) {
        if (i.kind === "up" && i.slot === slot) {
          try { ws.close(4003, "uplink-replaced"); } catch { /* gone */ }
          this.sockets.delete(ws);
        }
      }
      const pair = new WebSocketPair();
      pair[1].accept();
      this.attach(pair[1], { kind: "up", version: 2, slot, endpoint: null, relay: false, owner });
      return new Response(null, { status: 101, webSocket: pair[0] });
    }

    // Spectator (v2): watches a live match over the relay. Spectators get
    // slots 4..7, never punch (no endpoints), are relayed by definition, and
    // may never send binary game traffic — the host pushes every player's
    // confirmed input stream to them.
    if (role === "spec") {
      if (version !== 2) return this.reject("version-mismatch");
      if (!this.socketForSlot(0)) return this.reject("no-such-room");
      let slot = -1;
      for (let s = MAX_PLAYERS_V2; s < MAX_PLAYERS_V2 + MAX_SPECS; s++) {
        if (!this.socketForSlot(s)) { slot = s; break; }
      }
      if (slot < 0) return this.reject("room-full");
      const pair = new WebSocketPair();
      pair[1].accept();
      this.attach(pair[1], { kind: "spec", version: 2, slot, endpoint: null, relay: true });
      pair[1].send(JSON.stringify({ type: "welcome", room, role, slot }));
      this.broadcastRoster();
      return new Response(null, { status: 101, webSocket: pair[0] });
    }

    if (role === "host") {
      const oldHost = version === 2 ? this.socketForSlot(0) : this.socketV1("host");
      if (oldHost) return this.reject("room-in-use");
      // A new host may only reset a room that is empty or that already runs
      // his protocol version (hostless). A join with the OTHER version on a
      // live room must not evict its members — anyone holding the room code
      // could otherwise kill the room with a single connection.
      if (this.sockets.size > 0) {
        if (this.version !== version) return this.reject("version-mismatch");
        for (const [ws] of this.sockets) {
          try { ws.close(4002, "room-reset"); } catch { /* gone */ }
        }
        this.sockets.clear();
      }
      this.version = version;
      this.relayV1 = false;
    } else {
      if (this.version !== version) return this.reject("version-mismatch");
      const host = version === 2 ? this.socketForSlot(0) : this.socketV1("host");
      if (!host) return this.reject("no-such-room");
      if (version === 1 && this.socketV1("join")) return this.reject("room-full");
    }

    if (version === 1) {
      const pair = new WebSocketPair();
      pair[1].accept();
      this.attach(pair[1], { kind: role, version: 1, slot: role === "host" ? 0 : 1, endpoint: null, relay: false });
      pair[1].send(JSON.stringify({ type: "welcome", room, role }));
      this.introduceV1();
      return new Response(null, { status: 101, webSocket: pair[0] });
    }

    // v2: assign the slot atomically with the accept so two joins can't
    // grab the same one (no await between freeSlot and accept).
    const slot = role === "host" ? 0 : this.freeSlot();
    if (role !== "host" && slot < 0) return this.reject("room-full");
    const upkey = newUpkey();
    const pair = new WebSocketPair();
    pair[1].accept();
    this.attach(pair[1], { kind: role, version: 2, slot, endpoint: null, relay: false, upkey });
    pair[1].send(JSON.stringify({ type: "welcome", room, role, slot, upkey }));
    this.broadcastRoster();
    return new Response(null, { status: 101, webSocket: pair[0] });
  }

  /* ---- v1: two players, endpoint exchange + symmetric relay ---- */

  otherV1(ws) {
    const i = this.info(ws);
    if (!i) return null;
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

  /* ---- v2: roster broadcast + routed relay ---- */

  broadcastRoster(exclude) {
    /* Spectators receive the roster with every endpoint redacted: they are
     * relay-only by definition, so player IPs are information they don't
     * need (AUD-46). Players keep the full roster for hole punching. */
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

  /* ---- websocket events ---- */

  onMessage(ws, message) {
    const info = this.info(ws);
    if (!info) return;
    info.lastSeen = Date.now();
    this.armSweep();   // re-arm if a setAlarm failure cleared the flag

    if (typeof message !== "string") {
      // Relayed game packet. Production workerd delivers ws binary frames as
      // Blob (wrangler dev delivered ArrayBuffer); normalize through a
      // per-socket queue so frames keep arrival order.
      info.bin = (info.bin || Promise.resolve())
        .then(async () => this.onBinary(ws, message instanceof Blob ? await message.arrayBuffer() : message))
        .catch(() => {});
      return;
    }
    if (!this.allowMessage(info, false)) {
      try { ws.close(4029, "rate-limit"); } catch { /* gone */ }
      return;
    }
    // Counted above even when oversized: an attacker flooding >cap frames
    // must pay rate budget per frame, not get free drops.
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
    try {
      msg = JSON.parse(message);
    } catch {
      return;
    }
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
      case "relay": {
        if (wi.version === 2) {
          // Per-link relay: this client's path to the host goes over the
          // WebSocket. Mark the socket and tell both ends.
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
      default:
        break;
    }
  }

  onBinary(ws, message) {
    const info = this.info(ws);
    if (!info) return;

    // Count first so oversized-frame floods still spend rate budget.
    if (!this.allowMessage(info, true)) {
      try { ws.close(4029, "rate-limit"); } catch { /* gone */ }
      return;
    }
    if (message.byteLength > MAX_BINARY_BYTES) return;
    // Uplinks speak with their owner control socket's identity.
    const src = (info.kind === "up" && info.owner) ? this.info(info.owner) : info;
    if (!src) return;
    if (info.version === 2) {
      // Routed: [to_slot][payload] -> [from_slot][payload]. The star is
      // enforced: clients only reach slot 0, the host only client slots,
      // and the hop must be a link the room marked as relayed.
      const bytes = new Uint8Array(message);
      if (bytes.length < 2) return;
      const toSlot = bytes[0];
      // Spectators may only address the host (lobby signaling keeps their
      // link alive); their packets carry no game authority anyway - no
      // remote owns a spec slot at the engine - but restricting the route
      // keeps spec traffic off player links entirely. The payload itself
      // must be an "SSBL " text datagram: the host feeds relayed payloads
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
      const framed = new Uint8Array(bytes.length);
      framed[0] = src.slot;
      framed.set(bytes.subarray(1), 1);
      try { target.send(framed); } catch { /* gone */ }
      return;
    }
    if (this.relayV1) {
      const other = this.otherV1(ws);
      if (other) {
        try { other.send(message); } catch { /* gone */ }
      }
    }
  }

  onClose(ws) {
    const info = this.info(ws);
    if (!info) return;
    const otherV1 = this.otherV1(ws);   // resolve before removing ws
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
        // Host left: the room is over for everyone.
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
